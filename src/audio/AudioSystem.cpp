#include "audio/AudioSystem.hpp"

#include "assets/SoundRegistry.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mc::audio {
namespace {

struct ActiveSound final {
    ma_sound sound{};
    bool initialized = false;
    std::chrono::steady_clock::time_point createdAt = std::chrono::steady_clock::now();

    ~ActiveSound() {
        reset();
    }

    void reset() {
        if (!initialized) {
            return;
        }
        ma_sound_uninit(&sound);
        sound = {};
        initialized = false;
    }
};

struct CachedSoundAsset final {
    // Where the clip lives in the pack stack. Resolved to bytes on first play,
    // never to a filesystem path: asking a zip pack for a path is what made it
    // extract the whole sound tree to `.packcache`.
    assets::ResourceLocation location;
    bool exists = false;
    // The encoded OGG, held for as long as the audio system lives. miniaudio's
    // `register_encoded_data` does not copy, and the decode happens on the
    // audio callback thread, so the bytes have to outlive every voice that
    // could still be playing them — owning them here is what guarantees that.
    // Read lazily: a session plays a small fraction of the clips a pack ships,
    // and the old eager pass extracted every one of them.
    std::vector<std::byte> encoded;
    // The name the resource manager knows the registered bytes by. Not a path;
    // it never touches the filesystem.
    std::string virtualName;
    bool registered = false;
    // Keeping an unattached prototype alive pins miniaudio's decoded resource
    // node. Individual overlapping voices are cheap copies of this data source.
    std::unique_ptr<ActiveSound> prototype;
};

enum class BlockSoundAction : std::size_t { Break, Hit, Place, Step, Count };

constexpr std::array<std::string_view, static_cast<std::size_t>(BlockSoundAction::Count)>
    kBlockSoundActions{"break", "hit", "place", "step"};

} // namespace

const char* blockSoundFamilyName(BlockSoundFamily family) {
    switch (family) {
    case BlockSoundFamily::Stone:
        return "stone";
    case BlockSoundFamily::Grass:
        return "grass";
    case BlockSoundFamily::Wood:
        return "wood";
    case BlockSoundFamily::Sand:
        return "sand";
    case BlockSoundFamily::Gravel:
        return "gravel";
    case BlockSoundFamily::Cloth:
        // 26.1's event family is block.wool.*, even though the legacy OGG
        // filenames selected by sounds.json are still step/cloth*.ogg.
        return "wool";
    case BlockSoundFamily::Glass:
        return "glass";
    }
    return "stone";
}

class AudioSystem::Impl final {
  public:
    Impl(const assets::ResourceProvider& resourceProvider, float initialMasterVolume)
        : provider(&resourceProvider), registry(assets::SoundRegistry::load(resourceProvider)),
          masterVolume(std::clamp(initialMasterVolume, 0.0F, 1.0F)) {
        ma_engine_config engineConfig = ma_engine_config_init();
        engineConfig.listenerCount = 1;
        // Listener transforms are consumed by miniaudio's real-time callback.
        // Its setters write plain floats, so calling them from the render
        // thread races the spatializer. Publish atomics here and apply them at
        // the end of the audio callback instead.
        engineConfig.noAutoStart = MA_TRUE;
        engineConfig.onProcess = [](void* userData, float*, ma_uint64) {
            static_cast<Impl*>(userData)->applyPendingListener();
        };
        engineConfig.pProcessUserData = this;
        if (ma_engine_init(&engineConfig, &engine) == MA_SUCCESS) {
            const ma_result startResult = ma_engine_start(&engine);
            if (startResult == MA_SUCCESS) {
                initialized = true;
                ma_engine_set_volume(&engine, masterVolume);
                const ma_device* device = ma_engine_get_device(&engine);
                std::cout << "Audio system: miniaudio -> "
                          << (device != nullptr ? device->playback.name : "default playback device")
                          << " (master " << static_cast<int>(std::lround(masterVolume * 100.0F))
                          << "%)\n";
            } else {
                std::cerr << "Audio output failed to start (miniaudio result " << startResult
                          << ")\n";
                ma_engine_uninit(&engine);
            }
        } else {
            std::cerr << "Audio system unavailable; continuing without sound\n";
        }
        compileSoundAssets();
        compileBlockEvents();
    }

    ~Impl() {
        // Teardown order matters now that the clips are registered memory rather
        // than files. `register_encoded_data` does not copy, so the resource
        // manager points straight at the vectors in soundAssets and holds them
        // until the engine is torn down. Freeing the map first would leave the
        // engine's uninit walking freed buffers.
        activeSounds.clear();
        freeSounds.clear();
        // The prototypes are data sources over those same bytes.
        for (auto& [name, asset] : soundAssets) {
            static_cast<void>(name);
            asset.prototype.reset();
        }
        if (initialized) {
            ma_engine_uninit(&engine);
        }
        soundAssets.clear();
    }

    void setMasterVolume(float volume) {
        masterVolume = std::clamp(volume, 0.0F, 1.0F);
        if (initialized) {
            ma_engine_set_volume(&engine, masterVolume);
        }
    }

    void updateListener(const glm::vec3& position, const glm::vec3& direction,
                        const glm::vec3& up) {
        if (!initialized) {
            return;
        }
        listenerPositionX.store(position.x, std::memory_order_relaxed);
        listenerPositionY.store(position.y, std::memory_order_relaxed);
        listenerPositionZ.store(position.z, std::memory_order_relaxed);
        listenerDirectionX.store(direction.x, std::memory_order_relaxed);
        listenerDirectionY.store(direction.y, std::memory_order_relaxed);
        listenerDirectionZ.store(direction.z, std::memory_order_relaxed);
        listenerUpX.store(up.x, std::memory_order_relaxed);
        listenerUpY.store(up.y, std::memory_order_relaxed);
        listenerUpZ.store(up.z, std::memory_order_relaxed);
    }

    void applyPendingListener() {
        ma_engine_listener_set_position(
            &engine, 0, listenerPositionX.load(std::memory_order_relaxed),
            listenerPositionY.load(std::memory_order_relaxed),
            listenerPositionZ.load(std::memory_order_relaxed));
        ma_engine_listener_set_direction(
            &engine, 0, listenerDirectionX.load(std::memory_order_relaxed),
            listenerDirectionY.load(std::memory_order_relaxed),
            listenerDirectionZ.load(std::memory_order_relaxed));
        ma_engine_listener_set_world_up(
            &engine, 0, listenerUpX.load(std::memory_order_relaxed),
            listenerUpY.load(std::memory_order_relaxed),
            listenerUpZ.load(std::memory_order_relaxed));
    }

    void update() {
        const auto now = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < activeSounds.size();) {
            auto& sound = activeSounds[index];
            // An asynchronously decoded sound initially reports both
            // "not playing" and "at end". Keep it alive long enough for the
            // resource-manager job to finish instead of deleting it on the
            // frame immediately after ma_sound_start().
            if (now - sound->createdAt < std::chrono::seconds{2}) {
                ++index;
                continue;
            }
            if (ma_sound_at_end(&sound->sound) != MA_TRUE ||
                ma_sound_is_playing(&sound->sound) != MA_FALSE) {
                ++index;
                continue;
            }
            sound->reset();
            freeSounds.push_back(std::move(sound));
            if (index + 1U != activeSounds.size()) {
                activeSounds[index] = std::move(activeSounds.back());
            }
            activeSounds.pop_back();
        }
    }

    void playEvent(std::string_view event, const glm::vec3& position, float volume, float pitch) {
        if (event.empty()) {
            return;
        }
        const assets::SoundEntry* entry = registry.pick(event, randomState);
        if (entry == nullptr) {
            if (warnedMissingEvents.emplace(event).second) {
                std::cerr << "Missing sound event: " << event << '\n';
            }
            return;
        }
        // PX-6 Bug3: capture the played event's accessibility subtitle so the
        // renderer can show it when subtitles are on. Empty for events without a
        // caption (those simply do not show a subtitle).
        recordSubtitle(event);
        play(*entry, position, volume * entry->volume, pitch * entry->pitch);
    }

    void playEvent(assets::SoundEventId event, const glm::vec3& position, float volume,
                   float pitch) {
        const assets::SoundEntry* entry = registry.pick(event, randomState);
        if (entry != nullptr) {
            recordSubtitle(event);
            play(*entry, position, volume * entry->volume, pitch * entry->pitch);
        }
    }

    // PX-6 Bug3: remember the most recently played event's subtitle (empty if the
    // event has none), for the renderer's subtitle overlay to read.
    void recordSubtitle(std::string_view event) {
        const assets::SoundEvent* resolved = registry.find(event);
        lastSubtitle = resolved != nullptr ? resolved->subtitle : std::string{};
    }
    void recordSubtitle(assets::SoundEventId event) {
        const auto& events = registry.events();
        lastSubtitle = static_cast<std::size_t>(event) < events.size()
                           ? events[static_cast<std::size_t>(event)].subtitle
                           : std::string{};
    }

    // LivingEntity#getSoundPitch: (nextFloat() - nextFloat()) * 0.2 + 1.0, the
    // ±0.2 pitch roll every mob clip except footsteps uses.
    [[nodiscard]] float nextPitch() { return (randomUnit() - randomUnit()) * 0.2F + 1.0F; }

    [[nodiscard]] assets::SoundEventId blockEventId(BlockSoundFamily family,
                                                     BlockSoundAction action) const {
        return blockEvents[static_cast<std::size_t>(family)][static_cast<std::size_t>(action)];
    }

    void play(const assets::SoundEntry& entry, const glm::vec3& position, float volume,
              float pitch) {
        if (!initialized || masterVolume <= 0.0F) {
            return;
        }
        const auto cached = soundAssets.find(entry.name);
        if (cached == soundAssets.end() || !cached->second.exists) {
            if (warnedMissingAssets.emplace(entry.name).second) {
                std::cerr << "Missing sound asset: " << entry.name << '\n';
            }
            return;
        }
        CachedSoundAsset& asset = cached->second;
        if (!ensureRegistered(entry.name, asset)) {
            return;
        }
        std::unique_ptr<ActiveSound> active;
        if (freeSounds.empty()) {
            active = std::make_unique<ActiveSound>();
        } else {
            active = std::move(freeSounds.back());
            freeSounds.pop_back();
        }

        ma_result initResult = MA_ERROR;
        if (entry.stream) {
            initResult = ma_sound_init_from_file(&engine, asset.virtualName.c_str(),
                                                 MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_ASYNC,
                                                 nullptr, nullptr, &active->sound);
        } else {
            if (asset.prototype == nullptr) {
                asset.prototype = std::make_unique<ActiveSound>();
                const ma_result prototypeResult = ma_sound_init_from_file(
                    &engine, asset.virtualName.c_str(),
                    MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC |
                        MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT,
                    nullptr, nullptr, &asset.prototype->sound);
                if (prototypeResult == MA_SUCCESS) {
                    asset.prototype->initialized = true;
                } else {
                    asset.prototype.reset();
                }
            }
            if (asset.prototype != nullptr) {
                initResult = ma_sound_init_copy(&engine, &asset.prototype->sound, 0U, nullptr,
                                                &active->sound);
            }
        }
        if (initResult != MA_SUCCESS) {
            std::cerr << "Unable to decode sound asset: " << entry.name << '\n';
            freeSounds.push_back(std::move(active));
            return;
        }
        active->initialized = true;
        active->createdAt = std::chrono::steady_clock::now();
        ma_sound_set_position(&active->sound, position.x, position.y, position.z);
        ma_sound_set_volume(&active->sound, std::max(volume, 0.0F));
        ma_sound_set_pitch(&active->sound, std::max(pitch, 0.1F));
        // Block sounds in Java remain clearly audible across normal reach.
        // A four-block reference distance also avoids the default inverse
        // attenuation making a 4.5-block mining sound effectively inaudible.
        ma_sound_set_min_distance(&active->sound, 4.0F);
        ma_sound_set_max_distance(&active->sound, 48.0F);
        ma_sound_set_rolloff(&active->sound, 0.75F);
        const ma_result startResult = ma_sound_start(&active->sound);
        if (startResult != MA_SUCCESS) {
            std::cerr << "Unable to start sound asset " << entry.name << " (miniaudio result "
                      << startResult << ")\n";
            active->reset();
            freeSounds.push_back(std::move(active));
            return;
        }
        activeSounds.push_back(std::move(active));
    }

    // Reads the clip's bytes on first play and hands them to miniaudio's
    // resource manager under the asset's virtual name. Everything downstream —
    // streamed voices, the decoded prototype, the cheap copies of it — then
    // works exactly as it did against a file path, without one existing.
    //
    // `register_encoded_data` does not copy, so the vector that backs it has to
    // outlive every voice; it lives in the asset map, which outlives the engine
    // teardown below.
    [[nodiscard]] bool ensureRegistered(const std::string& name, CachedSoundAsset& asset) {
        if (asset.registered) {
            return true;
        }
        auto bytes = provider->readBytes(asset.location);
        if (bytes.empty()) {
            if (warnedMissingAssets.emplace(name).second) {
                std::cerr << "Unable to read sound asset: " << name << '\n';
            }
            asset.exists = false;
            return false;
        }
        asset.encoded = std::move(bytes);
        auto* manager = ma_engine_get_resource_manager(&engine);
        if (manager == nullptr) {
            return false;
        }
        const ma_result result = ma_resource_manager_register_encoded_data(
            manager, asset.virtualName.c_str(), asset.encoded.data(), asset.encoded.size());
        if (result != MA_SUCCESS) {
            if (warnedMissingAssets.emplace(name).second) {
                std::cerr << "Unable to register sound asset " << name << " (miniaudio result "
                          << result << ")\n";
            }
            return false;
        }
        asset.registered = true;
        return true;
    }

    void compileSoundAssets() {
        std::size_t entryCount = 0U;
        for (const auto& event : registry.events()) {
            entryCount += event.sounds.size();
        }
        soundAssets.reserve(entryCount);
        for (const auto& event : registry.events()) {
            for (const auto& entry : event.sounds) {
                if (entry.isEvent || soundAssets.contains(entry.name)) {
                    continue;
                }
                const auto parsed = assets::ResourceLocation::parse(entry.name);
                CachedSoundAsset asset;
                asset.location = assets::sounds(parsed.path + ".ogg", parsed.space);
                // `exists` is an index lookup on every provider, including the
                // zip one; `locate` is what materialises a file.
                asset.exists = provider->exists(asset.location);
                asset.virtualName = "rebedrock:" + entry.name;
                soundAssets.emplace(entry.name, std::move(asset));
            }
        }
    }

    void compileBlockEvents() {
        for (std::size_t family = 0; family < blockEvents.size(); ++family) {
            for (std::size_t action = 0; action < blockEvents[family].size(); ++action) {
                const std::string event =
                    "block." + std::string{blockSoundFamilyName(
                                    static_cast<BlockSoundFamily>(family))} +
                    "." + std::string{kBlockSoundActions[action]};
                blockEvents[family][action] = registry.idOf(event);
            }
        }
    }

    // A value in [0, 1) from the top 24 bits (an LCG's low bits are weak).
    [[nodiscard]] float randomUnit() {
        randomState = randomState * 1664525U + 1013904223U;
        return static_cast<float>(randomState >> 8) / static_cast<float>(1U << 24);
    }

    const assets::ResourceProvider* provider = nullptr;
    assets::SoundRegistry registry;
    // PX-6 Bug3: the subtitle of the most recently played event (empty if none).
    std::string lastSubtitle;
    float masterVolume = 1.0F;
    ma_engine engine{};
    bool initialized = false;
    std::uint32_t randomState = 0x4D43564BU;
    std::atomic<float> listenerPositionX{0.0F};
    std::atomic<float> listenerPositionY{0.0F};
    std::atomic<float> listenerPositionZ{0.0F};
    std::atomic<float> listenerDirectionX{0.0F};
    std::atomic<float> listenerDirectionY{0.0F};
    std::atomic<float> listenerDirectionZ{-1.0F};
    std::atomic<float> listenerUpX{0.0F};
    std::atomic<float> listenerUpY{1.0F};
    std::atomic<float> listenerUpZ{0.0F};
    std::unordered_set<std::string> warnedMissingEvents;
    std::unordered_set<std::string> warnedMissingAssets;
    std::unordered_map<std::string, CachedSoundAsset, assets::TransparentStringHash,
                       assets::TransparentStringEqual>
        soundAssets;
    std::array<std::array<assets::SoundEventId,
                          static_cast<std::size_t>(BlockSoundAction::Count)>,
               7>
        blockEvents{};
    std::vector<std::unique_ptr<ActiveSound>> activeSounds;
    std::vector<std::unique_ptr<ActiveSound>> freeSounds;
};

AudioSystem::AudioSystem(const assets::ResourceProvider& provider, float masterVolume)
    : implementation(std::make_unique<Impl>(provider, masterVolume)) {}

AudioSystem::~AudioSystem() = default;

bool AudioSystem::available() const { return implementation->initialized; }

std::string_view AudioSystem::lastSubtitle() const { return implementation->lastSubtitle; }

void AudioSystem::setMasterVolume(float volume) { implementation->setMasterVolume(volume); }

void AudioSystem::updateListener(const glm::vec3& position, const glm::vec3& direction,
                                 const glm::vec3& up) {
    implementation->updateListener(position, direction, up);
}

void AudioSystem::update() { implementation->update(); }

void AudioSystem::playBlockBreak(world::Block block, const glm::vec3& position) {
    implementation->playEvent(
        implementation->blockEventId(blockSoundFamily(block), BlockSoundAction::Break), position,
        1.0F, 0.8F);
}

void AudioSystem::playBlockHit(world::Block block, const glm::vec3& position) {
    // WorldRenderer.playEvent uses the block sound at roughly one quarter
    // volume for the four-tick mining cadence.
    implementation->playEvent(
        implementation->blockEventId(blockSoundFamily(block), BlockSoundAction::Hit), position,
        0.25F, 0.5F);
}

void AudioSystem::playBlockPlace(world::Block block, const glm::vec3& position) {
    implementation->playEvent(
        implementation->blockEventId(blockSoundFamily(block), BlockSoundAction::Place), position,
        0.8F, 0.8F);
}

void AudioSystem::playButtonClick(const glm::vec3& position) {
    implementation->playEvent("ui.button.click", position, 1.0F, 1.0F);
}

void AudioSystem::playFootstep(world::Block block, const glm::vec3& position, float volume) {
    implementation->playEvent(
        implementation->blockEventId(blockSoundFamily(block), BlockSoundAction::Step), position,
        volume, 1.0F);
}

void AudioSystem::playItemPickup(const glm::vec3& position) {
    implementation->playEvent("entity.item.pickup", position, 0.25F, 1.8F);
}

void AudioSystem::playSplash(const glm::vec3& position, float volume) {
    implementation->playEvent("entity.player.splash", position, volume, 1.0F);
}

void AudioSystem::playPlayerHurt(const glm::vec3& position) {
    implementation->playEvent("entity.player.hurt", position, 0.7F, 1.0F);
}

void AudioSystem::playPlayerFall(const glm::vec3& position, bool heavy) {
    implementation->playEvent(heavy ? "entity.player.big_fall" : "entity.player.small_fall",
                              position, 0.7F, 1.0F);
}

void AudioSystem::playEat(const glm::vec3& position) {
    // The three eating variants the way SoundEvents.ENTITY_GENERIC_EAT does.
    implementation->playEvent("entity.generic.eat", position, 1.0F, 1.0F);
}

void AudioSystem::playBurp(const glm::vec3& position) {
    implementation->playEvent("entity.player.burp", position, 1.0F, 1.0F);
}

void AudioSystem::playItemBreak(const glm::vec3& position) {
    // ENTITY_ITEM_BREAK: volume 0.8, and vanilla spreads the pitch a little.
    implementation->playEvent("entity.item.break", position, 0.8F, 0.9F);
}

void AudioSystem::playCreatureHurt(const MobSoundProfile& profile, const glm::vec3& position) {
    implementation->playEvent(profile.hurtEvent, position, profile.volume,
                              implementation->nextPitch());
}

void AudioSystem::playCreatureDeath(const MobSoundProfile& profile, const glm::vec3& position) {
    implementation->playEvent(profile.deathEvent, position, profile.volume,
                              implementation->nextPitch());
}

void AudioSystem::playCreatureAmbient(const MobSoundProfile& profile, const glm::vec3& position) {
    implementation->playEvent(profile.ambientEvent, position, profile.volume,
                              implementation->nextPitch());
}

void AudioSystem::playCreatureStep(const MobSoundProfile& profile, const glm::vec3& position) {
    implementation->playEvent(profile.stepEvent, position, profile.stepVolume, 1.0F);
}

void AudioSystem::playWeatherRain(const glm::vec3& position, float volume) {
    implementation->playEvent("weather.rain", position, volume, 1.0F);
}

void AudioSystem::playWeatherRainAbove(const glm::vec3& position, float volume) {
    implementation->playEvent("weather.rain.above", position, volume, 1.0F);
}

} // namespace mc::audio

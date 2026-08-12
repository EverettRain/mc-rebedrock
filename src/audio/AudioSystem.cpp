#include "audio/AudioSystem.hpp"

#include "assets/SoundRegistry.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
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
        if (initialized) {
            ma_sound_uninit(&sound);
        }
    }
};

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

[[nodiscard]] std::string blockEvent(BlockSoundFamily family, std::string_view action) {
    return "block." + std::string{blockSoundFamilyName(family)} + "." + std::string{action};
}

class AudioSystem::Impl final {
  public:
    Impl(const assets::ResourceProvider& resourceProvider, float initialMasterVolume)
        : provider(&resourceProvider), registry(assets::SoundRegistry::load(resourceProvider)),
          masterVolume(std::clamp(initialMasterVolume, 0.0F, 1.0F)) {
        ma_engine_config engineConfig = ma_engine_config_init();
        engineConfig.listenerCount = 1;
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
    }

    ~Impl() {
        activeSounds.clear();
        if (initialized) {
            ma_engine_uninit(&engine);
        }
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
        ma_engine_listener_set_position(&engine, 0, position.x, position.y, position.z);
        ma_engine_listener_set_direction(&engine, 0, direction.x, direction.y, direction.z);
        ma_engine_listener_set_world_up(&engine, 0, up.x, up.y, up.z);
    }

    void update() {
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(activeSounds, [now](const auto& sound) {
            // An asynchronously decoded sound initially reports both
            // "not playing" and "at end". Keep it alive long enough for the
            // resource-manager job to finish instead of deleting it on the
            // frame immediately after ma_sound_start().
            if (now - sound->createdAt < std::chrono::seconds{2}) {
                return false;
            }
            return ma_sound_at_end(&sound->sound) == MA_TRUE &&
                   ma_sound_is_playing(&sound->sound) == MA_FALSE;
        });
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
        play(resolveSound(entry->name), position, volume * entry->volume, pitch * entry->pitch);
    }

    // LivingEntity#getSoundPitch: (nextFloat() - nextFloat()) * 0.2 + 1.0, the
    // ±0.2 pitch roll every mob clip except footsteps uses.
    [[nodiscard]] float nextPitch() { return (randomUnit() - randomUnit()) * 0.2F + 1.0F; }

    // sounds.json names omit the extension and may carry a namespace. Resolve
    // the selected physical file through the layered provider, preserving
    // per-file overlay semantics after event selection.
    [[nodiscard]] std::filesystem::path resolveSound(std::string_view name) const {
        const auto parsed = assets::ResourceLocation::parse(name);
        return provider->locate(assets::sounds(parsed.path + ".ogg", parsed.space));
    }

    void play(const std::filesystem::path& path, const glm::vec3& position, float volume,
              float pitch) {
        if (!initialized || masterVolume <= 0.0F) {
            return;
        }
        if (!std::filesystem::exists(path)) {
            std::cerr << "Missing sound asset: " << path << '\n';
            return;
        }
        auto active = std::make_unique<ActiveSound>();
        if (ma_sound_init_from_file(&engine, path.string().c_str(),
                                    MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC, nullptr, nullptr,
                                    &active->sound) != MA_SUCCESS) {
            std::cerr << "Unable to decode sound asset: " << path << '\n';
            return;
        }
        active->initialized = true;
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
            std::cerr << "Unable to start sound asset " << path << " (miniaudio result "
                      << startResult << ")\n";
            return;
        }
        activeSounds.push_back(std::move(active));
    }

    // A value in [0, 1) from the top 24 bits (an LCG's low bits are weak).
    [[nodiscard]] float randomUnit() {
        randomState = randomState * 1664525U + 1013904223U;
        return static_cast<float>(randomState >> 8) / static_cast<float>(1U << 24);
    }

    const assets::ResourceProvider* provider = nullptr;
    assets::SoundRegistry registry;
    float masterVolume = 1.0F;
    ma_engine engine{};
    bool initialized = false;
    std::uint32_t randomState = 0x4D43564BU;
    std::unordered_set<std::string> warnedMissingEvents;
    std::vector<std::unique_ptr<ActiveSound>> activeSounds;
};

AudioSystem::AudioSystem(const assets::ResourceProvider& provider, float masterVolume)
    : implementation(std::make_unique<Impl>(provider, masterVolume)) {}

AudioSystem::~AudioSystem() = default;

bool AudioSystem::available() const { return implementation->initialized; }

void AudioSystem::setMasterVolume(float volume) { implementation->setMasterVolume(volume); }

void AudioSystem::updateListener(const glm::vec3& position, const glm::vec3& direction,
                                 const glm::vec3& up) {
    implementation->updateListener(position, direction, up);
}

void AudioSystem::update() { implementation->update(); }

void AudioSystem::playBlockBreak(world::Block block, const glm::vec3& position) {
    implementation->playEvent(blockEvent(blockSoundFamily(block), "break"), position, 1.0F, 0.8F);
}

void AudioSystem::playBlockHit(world::Block block, const glm::vec3& position) {
    // WorldRenderer.playEvent uses the block sound at roughly one quarter
    // volume for the four-tick mining cadence.
    implementation->playEvent(blockEvent(blockSoundFamily(block), "hit"), position, 0.25F, 0.5F);
}

void AudioSystem::playBlockPlace(world::Block block, const glm::vec3& position) {
    implementation->playEvent(blockEvent(blockSoundFamily(block), "place"), position, 0.8F, 0.8F);
}

void AudioSystem::playButtonClick(const glm::vec3& position) {
    implementation->playEvent("ui.button.click", position, 1.0F, 1.0F);
}

void AudioSystem::playFootstep(world::Block block, const glm::vec3& position, float volume) {
    implementation->playEvent(blockEvent(blockSoundFamily(block), "step"), position, volume, 1.0F);
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

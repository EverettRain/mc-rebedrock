#include "audio/AudioSystem.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
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
        return "cloth";
    case BlockSoundFamily::Glass:
        return "glass";
    }
    return "stone";
}

// The step sound directories are uneven in 1.16.1: stone, grass and wood have
// six clips each, sand five, and gravel and cloth four. Every dig family has
// four. Rolling a variation past the real count logs a missing asset on every
// play, which is exactly the sand6 spam seen when walking on sand.
[[nodiscard]] int stepVariationCount(BlockSoundFamily family) {
    switch (family) {
    case BlockSoundFamily::Sand:
        return 5;
    case BlockSoundFamily::Gravel:
        return 4;
    case BlockSoundFamily::Cloth:
        return 4;
    default:
        return 6;
    }
}

class AudioSystem::Impl final {
  public:
    Impl(std::filesystem::path initialSoundRoot, float initialMasterVolume)
        : soundRoot(std::move(initialSoundRoot)),
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

    void playFamily(const char* directory, BlockSoundFamily family, int variations,
                    const glm::vec3& position, float volume, float pitch) {
        const auto name = std::string{blockSoundFamilyName(family)};
        play(soundRoot / directory / (name + std::to_string(nextVariation(variations)) + ".ogg"),
             position, volume, pitch);
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

    [[nodiscard]] int nextVariation(int count) {
        randomState = randomState * 1664525U + 1013904223U;
        return static_cast<int>(randomState % static_cast<std::uint32_t>(count)) + 1;
    }

    std::filesystem::path soundRoot;
    float masterVolume = 1.0F;
    ma_engine engine{};
    bool initialized = false;
    std::uint32_t randomState = 0x4D43564BU;
    std::vector<std::unique_ptr<ActiveSound>> activeSounds;
};

AudioSystem::AudioSystem(std::filesystem::path soundRoot, float masterVolume)
    : implementation(std::make_unique<Impl>(std::move(soundRoot), masterVolume)) {}

AudioSystem::~AudioSystem() = default;

bool AudioSystem::available() const { return implementation->initialized; }

void AudioSystem::setMasterVolume(float volume) { implementation->setMasterVolume(volume); }

void AudioSystem::updateListener(const glm::vec3& position, const glm::vec3& direction,
                                 const glm::vec3& up) {
    implementation->updateListener(position, direction, up);
}

void AudioSystem::update() { implementation->update(); }

void AudioSystem::playBlockBreak(world::Block block, const glm::vec3& position) {
    const auto family = blockSoundFamily(block);
    if (family == BlockSoundFamily::Glass) {
        implementation->playFamily("random", family, 3, position, 1.0F, 1.0F);
        return;
    }
    implementation->playFamily("dig", family, 4, position, 1.0F, 0.8F);
}

void AudioSystem::playBlockHit(world::Block block, const glm::vec3& position) {
    // WorldRenderer.playEvent uses the block sound at roughly one quarter
    // volume for the four-tick mining cadence.
    const auto family = blockSoundFamily(block) == BlockSoundFamily::Glass
                            ? BlockSoundFamily::Stone
                            : blockSoundFamily(block);
    implementation->playFamily("dig", family, 4, position, 0.25F, 0.5F);
}

void AudioSystem::playBlockPlace(world::Block block, const glm::vec3& position) {
    const auto family = blockSoundFamily(block) == BlockSoundFamily::Glass
                            ? BlockSoundFamily::Stone
                            : blockSoundFamily(block);
    implementation->playFamily("dig", family, 4, position, 0.8F, 0.8F);
}

void AudioSystem::playButtonClick(const glm::vec3& position) {
    // SoundEvents.UI_BUTTON_CLICK, which this 1.16.1 build's sounds.json maps to
    // random/click_stereo, played the way AbstractButtonWidget#playDownSound
    // does: volume 1.0, pitch 1.0, master category.
    implementation->play(implementation->soundRoot / "random" / "click_stereo.ogg", position,
                         1.0F, 1.0F);
}

void AudioSystem::playFootstep(world::Block block, const glm::vec3& position, float volume) {
    const auto family = blockSoundFamily(block) == BlockSoundFamily::Glass
                            ? BlockSoundFamily::Stone
                            : blockSoundFamily(block);
    implementation->playFamily("step", family, stepVariationCount(family), position, volume,
                               1.0F);
}

void AudioSystem::playItemPickup(const glm::vec3& position) {
    implementation->play(implementation->soundRoot / "random" / "pop.ogg", position, 0.25F, 1.8F);
}

void AudioSystem::playSplash(const glm::vec3& position, float volume) {
    implementation->play(implementation->soundRoot / "liquid" /
                             (implementation->nextVariation(2) == 1 ? "splash.ogg" : "splash2.ogg"),
                         position, volume, 1.0F);
}

void AudioSystem::playPlayerHurt(const glm::vec3& position) {
    implementation->play(implementation->soundRoot / "damage" /
                             ("hit" + std::to_string(implementation->nextVariation(3)) + ".ogg"),
                         position, 0.7F, 1.0F);
}

void AudioSystem::playPlayerFall(const glm::vec3& position, bool heavy) {
    implementation->play(implementation->soundRoot / "damage" /
                             (heavy ? "fallbig.ogg" : "fallsmall.ogg"),
                         position, 0.7F, 1.0F);
}

void AudioSystem::playEat(const glm::vec3& position) {
    // The three eating variants the way SoundEvents.ENTITY_GENERIC_EAT does.
    implementation->play(implementation->soundRoot / "random" /
                             ("eat" + std::to_string(implementation->nextVariation(3)) + ".ogg"),
                         position, 1.0F, 1.0F);
}

void AudioSystem::playBurp(const glm::vec3& position) {
    implementation->play(implementation->soundRoot / "random" / "burp.ogg", position, 1.0F,
                         1.0F);
}

void AudioSystem::playItemBreak(const glm::vec3& position) {
    // ENTITY_ITEM_BREAK: volume 0.8, and vanilla spreads the pitch a little.
    implementation->play(implementation->soundRoot / "random" / "break.ogg", position, 0.8F,
                         0.9F);
}

void AudioSystem::playPigHurt(const glm::vec3& position) {
    implementation->play(implementation->soundRoot / "mob" / "pig" /
                             ("say" + std::to_string(implementation->nextVariation(3)) + ".ogg"),
                         position, 1.0F, 1.0F);
}

void AudioSystem::playPigDeath(const glm::vec3& position) {
    implementation->play(implementation->soundRoot / "mob" / "pig" / "death.ogg", position, 1.0F,
                         1.0F);
}

} // namespace mc::audio

#pragma once

#include "audio/MobSoundProfile.hpp"
#include "world/Block.hpp"

#include <filesystem>
#include <glm/vec3.hpp>
#include <memory>

namespace mc::audio {

enum class BlockSoundFamily {
    Stone,
    Grass,
    Wood,
    Sand,
    Gravel,
    Cloth,
    Glass,
};

[[nodiscard]] constexpr BlockSoundFamily blockSoundFamily(world::Block block) {
    using enum world::Block;
    switch (block) {
    case Grass:
    case Dirt:
    case CoarseDirt:
    case Podzol:
    case GrassPlant:
    case Dandelion:
    case OakSapling:
    case OakLeaves:
    case SpruceLeaves:
    case BirchLeaves:
    case JungleLeaves:
    case AcaciaLeaves:
    case DarkOakLeaves:
        return BlockSoundFamily::Grass;
    case OakPlanks:
    case SprucePlanks:
    case BirchPlanks:
    case JunglePlanks:
    case AcaciaPlanks:
    case DarkOakPlanks:
    case OakLog:
    case SpruceLog:
    case BirchLog:
    case JungleLog:
    case AcaciaLog:
    case DarkOakLog:
    case Bookshelf:
    case CraftingTable:
    case Pumpkin:
    case Melon:
    case Torch:
    case WallTorchNorth:
    case WallTorchEast:
    case WallTorchSouth:
    case WallTorchWest:
        return BlockSoundFamily::Wood;
    case Sand:
    case RedSand:
        return BlockSoundFamily::Sand;
    case Gravel:
        return BlockSoundFamily::Gravel;
    case WhiteWool:
    case RedWool:
    case BlackWool:
        return BlockSoundFamily::Cloth;
    case Glass:
        return BlockSoundFamily::Glass;
    default:
        return BlockSoundFamily::Stone;
    }
}

[[nodiscard]] const char* blockSoundFamilyName(BlockSoundFamily family);

class AudioSystem final {
  public:
    explicit AudioSystem(std::filesystem::path soundRoot, float masterVolume = 1.0F);
    ~AudioSystem();

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;
    AudioSystem(AudioSystem&&) = delete;
    AudioSystem& operator=(AudioSystem&&) = delete;

    [[nodiscard]] bool available() const;
    void setMasterVolume(float volume);
    void updateListener(const glm::vec3& position, const glm::vec3& direction, const glm::vec3& up);
    void update();

    void playBlockBreak(world::Block block, const glm::vec3& position);
    void playBlockHit(world::Block block, const glm::vec3& position);
    void playBlockPlace(world::Block block, const glm::vec3& position);
    // `ui.button.click`, the master-category sound every vanilla button plays
    // when pressed. Positioned at the listener so attenuation cannot hide it.
    void playButtonClick(const glm::vec3& position);
    void playFootstep(world::Block block, const glm::vec3& position, float volume = 0.5F);
    void playItemPickup(const glm::vec3& position);
    void playSplash(const glm::vec3& position, float volume = 0.7F);
    void playPlayerHurt(const glm::vec3& position);
    void playPlayerFall(const glm::vec3& position, bool heavy);
    void playEat(const glm::vec3& position);
    void playBurp(const glm::vec3& position);
    // `entity.item.break`, played when a tool runs out of durability.
    void playItemBreak(const glm::vec3& position);

    // ---- Per-species mob sounds (1.16.1 LivingEntity sound hooks) ----
    // Each plays the species' clip for that event at its getSoundVolume with
    // the vanilla ±0.2 randomised pitch. A profile with no clip for an event
    // stays silent, so species without an ambient sound are not forced to bark.
    void playCreatureHurt(const MobSoundProfile& profile, const glm::vec3& position);
    void playCreatureDeath(const MobSoundProfile& profile, const glm::vec3& position);
    void playCreatureAmbient(const MobSoundProfile& profile, const glm::vec3& position);
    // playStepSound: every 1.16.1 mob steps at 0.15 volume (profile.stepVolume),
    // fixed pitch 1.0 — the only one of the four that skips the pitch roll.
    void playCreatureStep(const MobSoundProfile& profile, const glm::vec3& position);

    // ---- Weather (1.16.1 WorldRenderer#tickRainSplashing) ----
    // WEATHER_RAIN / WEATHER_RAIN_ABOVE: the per-frame rain clip played at the
    // surface the drops hit, and its muffled under-roof variant. `volume` is the
    // caller's gradient-scaled value — 0.2 base for rain, 0.1 for rain-above.
    void playWeatherRain(const glm::vec3& position, float volume);
    void playWeatherRainAbove(const glm::vec3& position, float volume);

  private:
    class Impl;
    std::unique_ptr<Impl> implementation;
};

} // namespace mc::audio

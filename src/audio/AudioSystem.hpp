#pragma once

#include "assets/ResourceProvider.hpp"
#include "audio/MobSoundProfile.hpp"
#include "world/Block.hpp"

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
    // A block's step/break sound family, as grouped conditions rather than a
    // switch on identity: the leaves and logs come from their traits (isLeaves /
    // isLog are exactly the six leaf and six log blocks), the rest are the small
    // named lists the vanilla sound groups hold. Anything else falls to Stone.
    if (block == Grass || block == Dirt || block == CoarseDirt || block == Podzol ||
        block == GrassPlant || block == Dandelion || block == OakSapling ||
        world::isLeaves(block)) {
        return BlockSoundFamily::Grass;
    }
    if (world::isLog(block) || block == OakPlanks || block == SprucePlanks ||
        block == BirchPlanks || block == JunglePlanks || block == AcaciaPlanks ||
        block == DarkOakPlanks || block == Bookshelf || block == CraftingTable ||
        block == Pumpkin || block == Melon || block == Torch || block == WallTorch) {
        return BlockSoundFamily::Wood;
    }
    if (block == Sand || block == RedSand) {
        return BlockSoundFamily::Sand;
    }
    if (block == Gravel) {
        return BlockSoundFamily::Gravel;
    }
    if (block == WhiteWool || block == RedWool || block == BlackWool) {
        return BlockSoundFamily::Cloth;
    }
    if (block == Glass) {
        return BlockSoundFamily::Glass;
    }
    return BlockSoundFamily::Stone;
}

[[nodiscard]] const char* blockSoundFamilyName(BlockSoundFamily family);

class AudioSystem final {
  public:
    explicit AudioSystem(const assets::ResourceProvider& provider, float masterVolume = 1.0F);
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

    // ---- Per-species mob sound events ----
    // Each resolves the species event through sounds.json and applies its
    // getSoundVolume with the vanilla pitch roll. A profile with no event
    // stays silent, so species without an ambient sound are not forced to bark.
    void playCreatureHurt(const MobSoundProfile& profile, const glm::vec3& position);
    void playCreatureDeath(const MobSoundProfile& profile, const glm::vec3& position);
    void playCreatureAmbient(const MobSoundProfile& profile, const glm::vec3& position);
    // Steps use profile.stepVolume and fixed pitch 1.0.
    void playCreatureStep(const MobSoundProfile& profile, const glm::vec3& position);

    // ---- Weather ----
    // weather.rain / weather.rain.above: the per-frame rain clip played at the
    // surface the drops hit, and its muffled under-roof variant. `volume` is the
    // caller's gradient-scaled value — 0.2 base for rain, 0.1 for rain-above.
    void playWeatherRain(const glm::vec3& position, float volume);
    void playWeatherRainAbove(const glm::vec3& position, float volume);

  private:
    class Impl;
    std::unique_ptr<Impl> implementation;
};

} // namespace mc::audio

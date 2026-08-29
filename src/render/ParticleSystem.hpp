#pragma once

#include "world/Block.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc::world {
class World;
}

namespace mc::render {

enum class ParticleCategory : std::uint8_t {
    Gameplay,
    Weather,
};

struct BlockParticle final {
    glm::vec3 position{0.0F};
    glm::vec3 velocity{0.0F};
    float textureLayer = 0.0F;
    glm::vec2 uvOrigin{0.0F};
    float uvScale = 0.25F;
    float size = 0.10F;
    float ageSeconds = 0.0F;
    float lifetimeSeconds = 0.8F;
    float opacity = 1.0F;
    ParticleCategory category = ParticleCategory::Gameplay;
};

class ParticleSystem final {
public:
    // The 粒子效果 density knob: `scale` multiplies the per-event spawn counts
    // and the live-particle cap (1.0 = the current default, 2.0/3.0 for 高/疯狂,
    // 0.5 for 低). Applied when the option is loaded or cycled.
    void setLevelScale(float scale);

    void spawnBlockBreak(const glm::ivec3& blockPosition, world::Block block);
    void spawnWaterSplash(const glm::vec3& position);
    // One vanilla RainSplashParticle at a sampled solid/fluid impact point.
    // Water impacts are slightly broader so they remain legible on the moving
    // water texture; ground impacts keep the original compact sprite.
    void spawnRainImpact(const glm::vec3& position, bool onWater);
    // A rain drop landing on a water surface or solid ground: a short-lived
    // outward splash like vanilla's SplashParticle, kept small so the continuous
    // rain never floods the particle list. A non-zero `direction` (a wall's
    // outward normal) sprays the droplets in a half-circle fan facing away from
    // the wall instead of radially.
    void spawnRainSplash(const glm::vec3& position, const glm::vec2& direction = {0.0F, 0.0F});
    void update(float deltaSeconds, const world::World& world);

    [[nodiscard]] const std::vector<BlockParticle>& particles() const {
        return particles_;
    }
    [[nodiscard]] std::size_t particleLimit() const { return particleLimit_; }
    [[nodiscard]] std::size_t weatherParticleCount() const { return weatherParticleCount_; }
    [[nodiscard]] std::size_t weatherParticleLimit() const { return weatherParticleLimit_; }

private:
    // The number of particles one event spawns at the current density: the
    // exact `base * levelScale_` (ceil for the fractional part above one, a
    // probabilistic carry below one), never zero for a non-trivial base.
    [[nodiscard]] int scaledCount(int base);

    [[nodiscard]] float randomUnit();

    // Gameplay feedback has priority over ambient weather. Weather may use at
    // most 75% of the pool; if gameplay already occupies the reserve and the
    // pool is full, a new interaction evicts the oldest weather records in one
    // linear pass before it starts spawning.
    void reserveGameplayCapacity(std::size_t requested);
    [[nodiscard]] bool weatherCapacityAvailable() const;

    std::vector<BlockParticle> particles_;
    std::uint32_t randomState_ = 0x50415254U;
    // Live-particle ceiling at the current density; default 8000, scaled by
    // setLevelScale. Every spawn path skips once the list is full.
    std::size_t particleLimit_ = 8000U;
    std::size_t weatherParticleLimit_ = 6000U;
    std::size_t weatherParticleCount_ = 0U;
    float levelScale_ = 1.0F;
};

} // namespace mc::render

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
};

class ParticleSystem final {
public:
    void spawnBlockBreak(const glm::ivec3& blockPosition, world::Block block);
    void spawnWaterSplash(const glm::vec3& position);
    void update(float deltaSeconds, const world::World& world);

    [[nodiscard]] const std::vector<BlockParticle>& particles() const {
        return particles_;
    }

private:
    [[nodiscard]] float randomUnit();

    std::vector<BlockParticle> particles_;
    std::uint32_t randomState_ = 0x50415254U;
};

} // namespace mc::render

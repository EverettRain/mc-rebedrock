#include "render/RainSystem.hpp"

#include <algorithm>
#include <cmath>

namespace mc::render {

float RainSystem::randomUnit() {
    // xorshift32 — cheap, no STL RNG state, seeded from a constant like the
    // rest of the renderer's per-frame RNGs.
    std::uint32_t value = randomState_;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    randomState_ = value;
    return static_cast<float>(value & 0xFFFFU) / 65535.0F;
}

void RainSystem::update(float deltaSeconds, const glm::vec3& cameraPosition, float intensity,
                        std::size_t targetCount) {
    const std::size_t desired = intensity <= 0.02F
        ? 0U
        : static_cast<std::size_t>(static_cast<float>(targetCount) *
                                   std::min(intensity, 1.0F));
    while (drops_.size() < desired) {
        RainDrop drop;
        drop.position = {
            cameraPosition.x + (randomUnit() - 0.5F) * 32.0F,
            cameraPosition.y + 12.0F + randomUnit() * 8.0F,
            cameraPosition.z + (randomUnit() - 0.5F) * 32.0F,
        };
        drop.size = 0.03F + randomUnit() * 0.02F;
        drops_.push_back(drop);
    }
    if (drops_.size() > desired) {
        drops_.resize(desired);
    }
    // Rain falls fast; a drop below the camera's window respawns at the top of
    // the box so the population stays constant while raining.
    constexpr float kFallSpeed = 18.0F;
    const float fall = kFallSpeed * deltaSeconds;
    for (auto& drop : drops_) {
        drop.position.y -= fall;
        if (drop.position.y < cameraPosition.y - 4.0F) {
            drop.position.y = cameraPosition.y + 14.0F;
            drop.position.x = cameraPosition.x + (randomUnit() - 0.5F) * 32.0F;
            drop.position.z = cameraPosition.z + (randomUnit() - 0.5F) * 32.0F;
        }
    }
}

} // namespace mc::render

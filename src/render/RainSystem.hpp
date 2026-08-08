#pragma once

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc::render {

// A falling rain drop, CPU-simulated. The three render modes (贴图雨 texture
// sheets, 粒子雨 legacy per-particle draws, 异步粒子雨 instanced SSBO draws)
// consume the SAME drops so the paths can be compared with identical
// simulation: only the draw strategy differs.
struct RainDrop final {
    glm::vec3 position{0.0F};
    float size = 0.03F;
};

class RainSystem final {
  public:
    // Advances the rain for one frame: grows/shrinks the population toward
    // targetCount (scaled by rain intensity), then falls each drop and respawns
    // ones below the camera volume at the top.
    void update(float deltaSeconds, const glm::vec3& cameraPosition, float intensity,
                std::size_t targetCount);

    [[nodiscard]] const std::vector<RainDrop>& drops() const { return drops_; }

  private:
    [[nodiscard]] float randomUnit();

    std::vector<RainDrop> drops_;
    std::uint32_t randomState_ = 0x5EED41U;
};

} // namespace mc::render

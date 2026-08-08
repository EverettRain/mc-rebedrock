#pragma once

#include "render/MeshData.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>

namespace mc::render {

class Frustum final {
  public:
    explicit Frustum(const glm::mat4& viewProjection);

    [[nodiscard]] bool intersects(const Aabb& bounds) const;

  private:
    struct Plane final {
        glm::vec3 normal{};
        float distance = 0.0F;
    };

    std::array<Plane, 6> planes_{};
};

} // namespace mc::render

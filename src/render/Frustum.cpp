#include "render/Frustum.hpp"

#include <glm/geometric.hpp>
#include <glm/vec4.hpp>

#include <algorithm>

namespace mc::render {
namespace {

[[nodiscard]] glm::vec4 normalizedCoefficients(const glm::vec4& coefficients) {
    const glm::vec3 normal{coefficients};
    const float length = glm::length(normal);
    if (length <= 0.0F) {
        return {};
    }
    return coefficients / length;
}

} // namespace

Frustum::Frustum(const glm::mat4& matrix) {
    const glm::vec4 row0{matrix[0][0], matrix[1][0], matrix[2][0], matrix[3][0]};
    const glm::vec4 row1{matrix[0][1], matrix[1][1], matrix[2][1], matrix[3][1]};
    const glm::vec4 row2{matrix[0][2], matrix[1][2], matrix[2][2], matrix[3][2]};
    const glm::vec4 row3{matrix[0][3], matrix[1][3], matrix[2][3], matrix[3][3]};

    const auto setPlane = [this](std::size_t index, const glm::vec4& coefficients) {
        const glm::vec4 normalized = normalizedCoefficients(coefficients);
        planes_[index] = {glm::vec3{normalized}, normalized.w};
    };
    setPlane(0, row3 + row0);
    setPlane(1, row3 - row0);
    setPlane(2, row3 + row1);
    setPlane(3, row3 - row1);
    setPlane(4, row2);
    setPlane(5, row3 - row2);
}

bool Frustum::intersects(const Aabb& bounds) const {
    return std::ranges::all_of(planes_, [&bounds](const Plane& plane) {
        const glm::vec3 positive{
            plane.normal.x >= 0.0F ? bounds.maximum.x : bounds.minimum.x,
            plane.normal.y >= 0.0F ? bounds.maximum.y : bounds.minimum.y,
            plane.normal.z >= 0.0F ? bounds.maximum.z : bounds.minimum.z,
        };
        return glm::dot(plane.normal, positive) + plane.distance >= 0.0F;
    });
}

} // namespace mc::render

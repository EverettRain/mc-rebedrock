#include "render/PerspectiveCamera.hpp"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mc::render {

PerspectiveCamera::PerspectiveCamera(
    glm::vec3 position,
    glm::vec3 target,
    float verticalFieldOfViewDegrees)
    : position_(position),
      verticalFieldOfViewDegrees_(verticalFieldOfViewDegrees) {
    if (verticalFieldOfViewDegrees <= 1.0F || verticalFieldOfViewDegrees >= 179.0F) {
        throw std::invalid_argument("Perspective camera field of view is invalid");
    }
    const auto direction = glm::normalize(target - position);
    yawDegrees_ = glm::degrees(std::atan2(direction.z, direction.x));
    pitchDegrees_ = glm::degrees(std::asin(direction.y));
}

glm::mat4 PerspectiveCamera::viewMatrix() const {
    return glm::lookAt(position_, position_ + forward(), glm::vec3{0.0F, 1.0F, 0.0F});
}

glm::mat4 PerspectiveCamera::projectionMatrix(
    float aspectRatio,
    float farPlane) const {
    if (aspectRatio <= 0.0F) {
        throw std::invalid_argument("Perspective camera aspect ratio must be positive");
    }
    if (farPlane <= 0.1F) {
        throw std::invalid_argument("Perspective camera far plane must exceed near plane");
    }
    auto projection = glm::perspectiveRH_ZO(
        glm::radians(verticalFieldOfViewDegrees_), aspectRatio, 0.1F, farPlane);
    projection[1][1] *= -1.0F;
    return projection;
}

glm::vec3 PerspectiveCamera::position() const {
    return position_;
}

glm::vec3 PerspectiveCamera::direction() const {
    return forward();
}

void PerspectiveCamera::setPosition(glm::vec3 position) {
    position_ = position;
}

void PerspectiveCamera::setFieldOfViewDegrees(float degrees) {
    verticalFieldOfViewDegrees_ = std::clamp(degrees, 1.1F, 178.9F);
}

glm::vec3 PerspectiveCamera::forward() const {
    const float yaw = glm::radians(yawDegrees_);
    const float pitch = glm::radians(pitchDegrees_);
    return glm::normalize(glm::vec3{
        std::cos(yaw) * std::cos(pitch),
        std::sin(pitch),
        std::sin(yaw) * std::cos(pitch),
    });
}

void PerspectiveCamera::move(CameraMovement movement, float distance) {
    const auto front = forward();
    const auto right = glm::normalize(glm::cross(front, glm::vec3{0.0F, 1.0F, 0.0F}));
    switch (movement) {
    case CameraMovement::Forward:
        position_ += front * distance;
        break;
    case CameraMovement::Backward:
        position_ -= front * distance;
        break;
    case CameraMovement::Left:
        position_ -= right * distance;
        break;
    case CameraMovement::Right:
        position_ += right * distance;
        break;
    case CameraMovement::Up:
        position_.y += distance;
        break;
    case CameraMovement::Down:
        position_.y -= distance;
        break;
    }
}

void PerspectiveCamera::rotate(float yawDeltaDegrees, float pitchDeltaDegrees) {
    yawDegrees_ += yawDeltaDegrees;
    pitchDegrees_ = std::clamp(pitchDegrees_ + pitchDeltaDegrees, -89.0F, 89.0F);
}

void PerspectiveCamera::setRotation(float yawDegrees, float pitchDegrees) {
    yawDegrees_ = yawDegrees;
    pitchDegrees_ = std::clamp(pitchDegrees, -89.0F, 89.0F);
}

} // namespace mc::render

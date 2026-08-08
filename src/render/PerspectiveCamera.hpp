#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace mc::render {

enum class CameraMovement {
    Forward,
    Backward,
    Left,
    Right,
    Up,
    Down,
};

class PerspectiveCamera final {
  public:
    PerspectiveCamera(glm::vec3 position, glm::vec3 target, float verticalFieldOfViewDegrees);

    [[nodiscard]] glm::mat4 viewMatrix() const;
    [[nodiscard]] glm::mat4 projectionMatrix(
        float aspectRatio,
        float farPlane = 100.0F) const;
    [[nodiscard]] glm::vec3 position() const;
    [[nodiscard]] glm::vec3 direction() const;

    [[nodiscard]] float fieldOfViewDegrees() const { return verticalFieldOfViewDegrees_; }

    void setPosition(glm::vec3 position);
    // Vanilla scales the base FOV by the player's movement multiplier every
    // frame, which is what makes a sprint read as fast. Values outside the
    // constructor's accepted range are clamped rather than rejected.
    void setFieldOfViewDegrees(float degrees);
    void move(CameraMovement movement, float distance);
    void rotate(float yawDeltaDegrees, float pitchDeltaDegrees);
    // Absolute look, set by /tp's rotation argument. Pitch is clamped the same
    // way rotate() clamps it.
    void setRotation(float yawDegrees, float pitchDegrees);

  private:
    [[nodiscard]] glm::vec3 forward() const;

    glm::vec3 position_;
    float yawDegrees_ = -90.0F;
    float pitchDegrees_ = 0.0F;
    float verticalFieldOfViewDegrees_;
};

} // namespace mc::render

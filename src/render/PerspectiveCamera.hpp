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
    // vanilla 每帧用玩家的移动倍率去缩放基础 FOV，冲刺看起来快就是这么来的
    // 超出构造函数接受范围的取值一律夹紧，而不是拒绝
    void setFieldOfViewDegrees(float degrees);
    void move(CameraMovement movement, float distance);
    void rotate(float yawDeltaDegrees, float pitchDeltaDegrees);
    // 绝对视角，由 /tp 的旋转参数设置
    // 俯仰角的夹紧方式与 rotate() 完全一致
    void setRotation(float yawDegrees, float pitchDegrees);

  private:
    [[nodiscard]] glm::vec3 forward() const;

    glm::vec3 position_;
    float yawDegrees_ = -90.0F;
    float pitchDegrees_ = 0.0F;
    float verticalFieldOfViewDegrees_;
};

} // namespace mc::render

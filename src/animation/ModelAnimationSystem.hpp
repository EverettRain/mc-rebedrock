#pragma once

#include "animation/AnimationClip.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <filesystem>

namespace mc::animation {

enum class ModelAction {
    None,
    Break,
    Use,
    Eat,
};

struct ModelPose final {
    glm::vec3 translation{0.0F};
    // Euler rotation in degrees, applied Y then Z then X the way Minecraft's
    // matrix stack multiplies its per-axis quaternions.
    glm::vec3 rotationDegrees{0.0F};
    float scale = 1.0F;
    float swingProgress = 0.0F;
};

// Camera-space transforms matching vanilla's
// HeldItemRenderer right-hand matrix stack.
[[nodiscard]] glm::mat4 firstPersonArmTransform(const ModelPose& pose);
[[nodiscard]] glm::mat4 firstPersonItemTransform(const ModelPose& pose, bool cubeModel);
// The held item during eating: the eat clip's camera-space lift and tilt, then
// the resting hand placement (HeldItemRenderer's UseAction.EAT branch).
[[nodiscard]] glm::mat4 firstPersonEatTransform(const ModelPose& pose, bool cubeModel);

// Drives the first-person held-item pose from data-driven animation clips
// (the "break", "use" and "eat" actions). The keyframes live in JSON and are
// evaluated through the shared animation library; a compact built-in copy is
// compiled in so the swing works even before any resource pack is loaded, and
// `load` can override it from `resources/animation/held_item.animation.json`.
//
// The action timeline lives in gameplay (PlayerActionState, tick-driven), so
// this class is a pure function of (action, progress) each frame: the caller
// supplies the normalised clip progress and the pose is sampled from it. There
// is no frame-time clock here, which is what makes the swing consume the same
// ticks at any frame rate.
class ModelAnimationSystem final {
  public:
    ModelAnimationSystem();

    // Replaces the built-in clips with the ones under `animationDirectory` when
    // present. Failures leave the built-in clips in place.
    void load(const std::filesystem::path& animationDirectory);

    // Samples the action's clip at `progress` in [0, 1], or clears the pose for
    // None. The caller derives progress from the tick-owned action timeline.
    void setAction(ModelAction action, float progress);

    [[nodiscard]] bool active() const { return action_ != ModelAction::None; }
    [[nodiscard]] ModelAction action() const { return action_; }
    [[nodiscard]] const ModelPose& pose() const { return pose_; }

  private:
    [[nodiscard]] const AnimationClip* clipFor(ModelAction action) const;
    void sample(float progress);

    AnimationLibrary library_;
    ModelAction action_ = ModelAction::None;
    ModelPose pose_{};
};

} // namespace mc::animation

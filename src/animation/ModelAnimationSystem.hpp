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

// Camera-space transforms matching Minecraft Java 1.16.1's
// HeldItemRenderer right-hand matrix stack.
[[nodiscard]] glm::mat4 firstPersonArmTransform(const ModelPose& pose);
[[nodiscard]] glm::mat4 firstPersonItemTransform(const ModelPose& pose, bool cubeModel);
// The held item during eating: the eat clip's camera-space lift and tilt, then
// the resting hand placement (HeldItemRenderer's UseAction.EAT branch).
[[nodiscard]] glm::mat4 firstPersonEatTransform(const ModelPose& pose, bool cubeModel);

// Drives the first-person held-item swing from data-driven animation clips
// (the "break" and "use" actions). The keyframes live in JSON and are evaluated
// through the shared animation library; a compact built-in copy is compiled in
// so the swing works even before any resource pack is loaded, and `load` can
// override it from `resources/animation/held_item.animation.json`.
class ModelAnimationSystem final {
  public:
    ModelAnimationSystem();

    // Replaces the built-in clips with the ones under `animationDirectory` when
    // present. Failures leave the built-in clips in place.
    void load(const std::filesystem::path& animationDirectory);

    void trigger(ModelAction action);
    void update(float deltaSeconds);

    [[nodiscard]] bool active() const { return action_ != ModelAction::None; }
    [[nodiscard]] ModelAction action() const { return action_; }
    [[nodiscard]] const ModelPose& pose() const { return pose_; }

  private:
    // An action gameplay owns the lifetime of (eating runs for the vanilla
    // 32-tick meal, or until the button is released), as opposed to a one-shot
    // swing that ends with its clip.
    [[nodiscard]] static bool held(ModelAction action) { return action == ModelAction::Eat; }
    [[nodiscard]] const AnimationClip* clipFor(ModelAction action) const;
    void sample();

    AnimationLibrary library_;
    ModelAction action_ = ModelAction::None;
    float elapsedSeconds_ = 0.0F;
    ModelPose pose_{};
};

} // namespace mc::animation

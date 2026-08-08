#pragma once

#include "animation/AnimationClip.hpp"
#include "animation/Animator.hpp"
#include "animation/SkeletalModel.hpp"

#include <filesystem>

namespace mc::animation {

struct PlayerModelPose final {
    float bodyYaw = 0.0F;
    float headYaw = 0.0F;
    float headPitch = 0.0F;
    float rightArmPitch = 0.0F;
    float leftArmPitch = 0.0F;
    float rightLegPitch = 0.0F;
    float leftLegPitch = 0.0F;
    float idleBob = 0.0F;
};

// Drives the inventory/creative player preview through the unified animation
// library: a Bedrock player geometry plus walk/idle/look clips are blended by an
// Animator each frame, and the resulting bone rotations are projected into the
// flat PlayerModelPose the preview renderer already consumes. A compact built-in
// geometry/clip set is compiled in, and `load` can override it from
// resources/animation/player.{geo,animation}.json.
class PlayerModelAnimator final {
  public:
    PlayerModelAnimator();

    // Overrides the built-in preview assets from `animationDirectory` when the
    // files are present; failures keep the built-in assets.
    void load(const std::filesystem::path& animationDirectory);

    void setCursorLook(float normalizedX, float normalizedY);
    // Whether the body bone turns with the cursor look. The inventory/creative
    // preview enables this (the look clip rotates the body at half the head's
    // amplitude); the third-person world player keeps it off, because its body
    // already follows the look direction through the renderer's separate
    // "head leads, body follows" yaw.
    void setBodyFollowsLook(bool enabled) { bodyFollowsLook_ = enabled; }
    // `walking`/`sneaking` are targets; their influence eases in and out over a
    // few frames so state changes blend instead of snapping.
    void update(float deltaSeconds, bool walking, bool sneaking = false);
    [[nodiscard]] const PlayerModelPose& pose() const { return pose_; }

    // The full skeletal pose from the most recent update, plus the geometry it
    // belongs to. The world renderer uses these to draw the third-person player
    // as real bone-transformed cuboids (the flat PlayerModelPose is a projection
    // of the same pose for the 2D inventory preview).
    [[nodiscard]] const SkeletalModel& model() const { return model_; }
    [[nodiscard]] const SkeletonPose& skeletonPose() const { return skeletonPose_; }

  private:
    void rebindBones();

    SkeletalModel model_;
    AnimationLibrary library_;
    Animator animator_;
    SkeletonPose skeletonPose_;

    int bodyBone_ = -1;
    int headBone_ = -1;
    int rightArmBone_ = -1;
    int leftArmBone_ = -1;
    int rightLegBone_ = -1;
    int leftLegBone_ = -1;

    float lookX_ = 0.0F;
    float lookY_ = 0.0F;
    float elapsed_ = 0.0F;
    // Eased blend weights so walk/sneak transitions are smooth, not abrupt.
    float walkAmount_ = 0.0F;
    float sneakAmount_ = 0.0F;
    bool bodyFollowsLook_ = false;
    PlayerModelPose pose_{};
};

} // namespace mc::animation

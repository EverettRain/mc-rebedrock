#pragma once

#include "animation/AnimationClip.hpp"
#include "animation/AnimationController.hpp"
#include "animation/Animator.hpp"
#include "animation/BoneMask.hpp"
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
    // Holds an item pose on the upper body (arms) as an OVERRIDE layer masked to
    // the arms: while `holding` is true the arms take the item-hold pitch instead
    // of the locomotion arm swing (ANIM-1 mask + ANIM-2 override), so walking and
    // holding an item no longer bleed into each other. `pitchDegrees` is the arm
    // pitch of the held pose (0 = rest, negative = raised forward).
    void setItemHold(bool holding, float pitchDegrees = -55.0F);
    // `walking`/`sneaking` are targets; the ANIM-3 controller selects the
    // idle/walk/sneak state and an ANIM-2 crossfade eases the state blend, so
    // state changes fade instead of snapping (no hand-written eased weights).
    void update(float deltaSeconds, bool walking, bool sneaking = false);
    // World-player entry point: drive the same controller stack from the
    // AUTHORITATIVE walk drive quantities (walkAnimationSpeed = amplitude,
    // walkAnimationPosition = phase) and the real render age, rather than the
    // preview's clock-synthesized values. Look and item-hold are set via
    // setCursorLook / setItemHold first. This replaces the retired
    // HumanoidPoseSolver: the third-person world player now shares the preview's
    // Molang clips + ANIM controller + masks (one animation path, two feeds).
    void updateWorldPlayer(float deltaSeconds, float walkAmount, float walkPosition,
                           float ageInTicks, bool sneaking = false);
    [[nodiscard]] const PlayerModelPose& pose() const { return pose_; }

    // The controller state the locomotion machine settled on this frame
    // (idle/walk/sneak). Exposed for tests and mob reuse.
    [[nodiscard]] const std::string& locomotionState() const {
        return controllerInstance_.currentState();
    }
    // True while a state crossfade is in flight (ANIM-2 Transition ramp). Exposed
    // so tests can prove the state blend eases over time rather than snapping.
    [[nodiscard]] bool locomotionTransitioning() const {
        return controllerInstance_.transitioning();
    }

    // The full skeletal pose from the most recent update, plus the geometry it
    // belongs to. The world renderer uses these to draw the third-person player
    // as real bone-transformed cuboids (the flat PlayerModelPose is a projection
    // of the same pose for the 2D inventory preview).
    [[nodiscard]] const SkeletalModel& model() const { return model_; }
    [[nodiscard]] const SkeletonPose& skeletonPose() const { return skeletonPose_; }

  private:
    void rebindBones();
    // The shared per-frame evaluation both feeds call: sets the Molang drive
    // variables, runs the controller + masks + item-hold override, and projects
    // the flat PlayerModelPose. `walkAmount` is the gait amplitude, `walkPosition`
    // the phase, `idleAgeTicks` the idle-bob age.
    void evaluatePose(float dt, float walkAmount, float walkPosition, float idleAgeTicks,
                      bool sneaking);

    SkeletalModel model_;
    AnimationLibrary library_;
    Animator animator_;
    SkeletonPose skeletonPose_;

    // ANIM-4 layered animation: masks split the skeleton so locomotion drives the
    // legs, the item pose overrides the arms, and the look drives the head — each
    // without touching the others. The controller selects idle/walk/sneak.
    BoneGroups masks_;
    AnimationControllerSet controllers_;
    AnimationControllerInstance controllerInstance_;
    // The held-item override pose, authored as a clip once and masked to the arms.
    AnimationClip itemHoldClip_;
    bool holdingItem_ = false;
    float itemHoldPitch_ = -55.0F;

    int bodyBone_ = -1;
    int headBone_ = -1;
    int rightArmBone_ = -1;
    int leftArmBone_ = -1;
    int rightLegBone_ = -1;
    int leftLegBone_ = -1;

    float lookX_ = 0.0F;
    float lookY_ = 0.0F;
    float elapsed_ = 0.0F;
    // B2: the walk-clip phase accumulator (vanilla `walkAnimation.position`); for
    // the preview it advances from the clock at a walking cadence.
    float walkPosition_ = 0.0F;
    // The walk amplitude fed to the walk clip's Molang; the controller crossfade
    // owns the idle<->walk state blend, so this only scales the swing magnitude.
    float walkAmount_ = 0.0F;
    bool walking_ = false;
    bool sneaking_ = false;
    bool bodyFollowsLook_ = false;
    PlayerModelPose pose_{};
};

} // namespace mc::animation

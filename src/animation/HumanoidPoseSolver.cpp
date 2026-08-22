#include "animation/HumanoidPoseSolver.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace mc::animation {
namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kDegreesPerRadian = 180.0F / kPi;

// 26.1's HumanoidModel walk cadence: limbs swing as cos(pos * 0.6662) scaled by
// speed. rightArm/leftLeg are in phase; leftArm/rightLeg are a half period out.
constexpr float kWalkFrequency = 0.6662F;
// HumanoidModel's arm expression includes a final 0.5 factor: 2.0 * 0.5 = 1.0
// effective radian. Legs use 1.4 radians.
constexpr float kArmSwingRadians = 1.0F;
constexpr float kLegSwingRadians = 1.4F;

// The wrist offset from the arm pivot, in model units, where a held item hangs.
// The arm is 12 units long from the shoulder; the socket sits near the hand end.
constexpr float kWristDrop = 10.0F;

[[nodiscard]] float toDegrees(float radians) { return radians * kDegreesPerRadian; }

void setRotation(SkeletonPose& pose, int boneIndex, const glm::vec3& degrees) {
    if (boneIndex < 0) {
        return;
    }
    pose.bone(static_cast<std::size_t>(boneIndex)).rotation = degrees;
}

void addRotation(SkeletonPose& pose, int boneIndex, const glm::vec3& degrees) {
    if (boneIndex < 0) {
        return;
    }
    pose.bone(static_cast<std::size_t>(boneIndex)).rotation += degrees;
}

void setPosition(SkeletonPose& pose, int boneIndex, const glm::vec3& offset) {
    if (boneIndex < 0) {
        return;
    }
    pose.bone(static_cast<std::size_t>(boneIndex)).position = offset;
}

}  // namespace

HumanoidBoneBindings HumanoidBoneBindings::bind(const SkeletalModel& model) {
    HumanoidBoneBindings bindings;
    bindings.body = model.findBone("body");
    bindings.head = model.findBone("head");
    bindings.rightArm = model.findBone("rightArm");
    bindings.leftArm = model.findBone("leftArm");
    bindings.rightLeg = model.findBone("rightLeg");
    bindings.leftLeg = model.findBone("leftLeg");
    return bindings;
}

PlayerPoseFrame solveHumanoidPose(const SkeletalModel& model, const HumanoidBoneBindings& bones,
                                  const render::player::PlayerRenderState& state,
                                  float ageInTicks) {
    // 1. resetPose: start every bone at rest (zero deltas). SkeletonPose's
    //    constructor already zeroes the deltas, so this is the fresh pose.
    PlayerPoseFrame frame;
    frame.skeleton = SkeletonPose{model};
    SkeletonPose& pose = frame.skeleton;

    // 2. applyLook: the head turns by the head-relative yaw and pitches. bodyYaw
    //    is applied by the renderer at the world root, not baked into a bone.
    setRotation(pose, bones.head, glm::vec3{state.pitchDegrees, state.headYawDegrees, 0.0F});

    // 3. applyLocomotion: limbs swing along the walk phase, amplitude scaled by
    //    the walk speed. Arms and legs are the standard vanilla phase pairs.
    const float phase = state.walkStride * kWalkFrequency;
    // Sprinting keeps the same tick-owned phase but carries a visibly stronger
    // gait. Clamp after the multiplier so malformed snapshots cannot spin limbs
    // through multiple revolutions.
    constexpr float kSprintSwingMultiplier = 1.25F;
    const float amount = std::clamp(
        state.walkSpeed * (state.sprinting ? kSprintSwingMultiplier : 1.0F), 0.0F, 1.0F);
    const float rightArmX = toDegrees(std::cos(phase + kPi) * kArmSwingRadians * amount);
    const float leftArmX = toDegrees(std::cos(phase) * kArmSwingRadians * amount);
    const float rightLegX = toDegrees(std::cos(phase) * kLegSwingRadians * amount);
    const float leftLegX = toDegrees(std::cos(phase + kPi) * kLegSwingRadians * amount);
    addRotation(pose, bones.rightArm, glm::vec3{rightArmX, 0.0F, 0.0F});
    addRotation(pose, bones.leftArm, glm::vec3{leftArmX, 0.0F, 0.0F});
    addRotation(pose, bones.rightLeg, glm::vec3{rightLegX, 0.0F, 0.0F});
    addRotation(pose, bones.leftLeg, glm::vec3{leftLegX, 0.0F, 0.0F});

    // 4. applyArmPoses: an item is held slightly forward; a block a bit more;
    //    eating raises the arm toward the head. The main hand is the right arm.
    const auto applyArmPose = [&](int armBone, render::player::ArmPose armPose) {
        switch (armPose) {
            case render::player::ArmPose::Item:
                addRotation(pose, armBone, glm::vec3{-30.0F, 0.0F, 0.0F});
                break;
            case render::player::ArmPose::Block:
                addRotation(pose, armBone, glm::vec3{-45.0F, 0.0F, 0.0F});
                break;
            case render::player::ArmPose::Eat:
                // Raised to the mouth, wobbling with the use progress. This is an
                // OVERRIDE, not an add: the use pose owns the arm (vanilla sets
                // the arm's pitch absolutely for eating/drinking), so it replaces
                // the locomotion swing rather than summing with it — otherwise a
                // walk phase would tilt the food away from the mouth.
                setRotation(pose, armBone,
                            glm::vec3{-70.0F + std::sin(state.use.progress * kPi * 6.0F) * 5.0F,
                                      0.0F, 0.0F});
                break;
            case render::player::ArmPose::Empty:
            case render::player::ArmPose::Bow:
            case render::player::ArmPose::Spear:
            case render::player::ArmPose::Crossbow:
            case render::player::ArmPose::Spyglass:
            case render::player::ArmPose::Horn:
            case render::player::ArmPose::Brush:
                break;
        }
    };
    applyArmPose(bones.rightArm, state.rightArmPose);
    applyArmPose(bones.leftArm, state.leftArmPose);

    // 5. applyAttack: the swinging arm sweeps forward-and-up over the arc. sin(pi
    //    * progress) peaks mid-swing. The main-hand swing drives the right arm;
    //    the body twists slightly toward the swing (26.1's attack body rotation).
    if (state.swing.active) {
        const float swingLift = std::sin(state.swing.progress * kPi);
        addRotation(pose, bones.rightArm, glm::vec3{-swingLift * 60.0F, 0.0F, 0.0F});
        addRotation(pose, bones.body, glm::vec3{0.0F, swingLift * 8.0F, 0.0F});
    }

    // 6. applyCrouch: reproduce HumanoidModel's 0.5-radian body lean and model-
    //    part offsets. This geometry parents head/arms to body while JE keeps the
    //    parts independent, so their local transforms cancel the inherited body
    //    rotation/translation before adding JE's own head/arm offsets.
    if (state.sneaking) {
        constexpr float kBodyLeanRadians = 0.5F;
        constexpr float kArmCrouchRadians = 0.4F;
        const float bodyLeanDegrees = toDegrees(kBodyLeanRadians);
        addRotation(pose, bones.body, glm::vec3{bodyLeanDegrees, 0.0F, 0.0F});
        setPosition(pose, bones.body, glm::vec3{0.0F, -3.2F, 0.0F});

        addRotation(pose, bones.head, glm::vec3{-bodyLeanDegrees, 0.0F, 0.0F});
        setPosition(pose, bones.head,
                    glm::vec3{0.0F, -std::cos(kBodyLeanRadians),
                              std::sin(kBodyLeanRadians)});

        const glm::vec3 armParentCompensation{
            0.0F,
            2.0F - 2.0F * std::cos(kBodyLeanRadians),
            2.0F * std::sin(kBodyLeanRadians),
        };
        const float armRotationCompensation =
            toDegrees(kArmCrouchRadians - kBodyLeanRadians);
        addRotation(pose, bones.rightArm,
                    glm::vec3{armRotationCompensation, 0.0F, 0.0F});
        addRotation(pose, bones.leftArm,
                    glm::vec3{armRotationCompensation, 0.0F, 0.0F});
        setPosition(pose, bones.rightArm, armParentCompensation);
        setPosition(pose, bones.leftArm, armParentCompensation);

        // JE's model-space +Z points toward the crouching torso. This Bedrock
        // geometry faces -Z, so the equivalent leg offset must be negated or the
        // upper and lower body move in opposite directions and visibly split.
        setPosition(pose, bones.rightLeg, glm::vec3{0.0F, -0.2F, -4.0F});
        setPosition(pose, bones.leftLeg, glm::vec3{0.0F, -0.2F, -4.0F});
    }

    // 7. applyIdleBob: a small cosmetic arm sway from the render age. Skipped
    //    while an item is actively used (the eat/aim pose owns the arm), matching
    //    the vanilla suppression for spyglass/aim poses.
    const bool armBusy = state.use.active;
    if (!armBusy) {
        const float bob = std::sin(ageInTicks * 0.067F) * 2.0F;
        addRotation(pose, bones.rightArm, glm::vec3{0.0F, 0.0F, bob});
        addRotation(pose, bones.leftArm, glm::vec3{0.0F, 0.0F, -bob});
    }

    // 8. resolveSockets: the hand sockets follow the animated arm bones. The
    //    socket sits at the wrist, dropped from the arm pivot along the arm.
    const glm::mat4 wrist = glm::translate(glm::mat4{1.0F}, glm::vec3{0.0F, -kWristDrop, 0.0F});
    if (bones.rightArm >= 0) {
        frame.rightHandSocket = pose.worldMatrix(bones.rightArm) * wrist;
    }
    if (bones.leftArm >= 0) {
        frame.leftHandSocket = pose.worldMatrix(bones.leftArm) * wrist;
    }
    return frame;
}

}  // namespace mc::animation

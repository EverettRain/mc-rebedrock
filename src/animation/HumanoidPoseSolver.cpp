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
    // ANIM A2: no sprint multiplier — the amplitude s already saturates to 1.0 for
    // sprint and creative flight (§8.1: sprint does not change the pose formula).
    // A negative amplitude (§5 backpedal) reverses the phase, which cos handles.
    const float amount = std::clamp(state.walkSpeed, -1.0F, 1.0F);
    const float rightArmX = toDegrees(std::cos(phase + kPi) * kArmSwingRadians * amount);
    const float leftArmX = toDegrees(std::cos(phase) * kArmSwingRadians * amount);
    const float rightLegX = toDegrees(std::cos(phase) * kLegSwingRadians * amount);
    const float leftLegX = toDegrees(std::cos(phase + kPi) * kLegSwingRadians * amount);
    addRotation(pose, bones.rightArm, glm::vec3{rightArmX, 0.0F, 0.0F});
    addRotation(pose, bones.leftArm, glm::vec3{leftArmX, 0.0F, 0.0F});
    addRotation(pose, bones.rightLeg, glm::vec3{rightLegX, 0.0F, 0.0F});
    addRotation(pose, bones.leftLeg, glm::vec3{leftLegX, 0.0F, 0.0F});

    // 4. applyArmPoses (§14): the ArmPose OVERRIDES the held arm's xRot — it halves
    //    the walk swing and adds a forward lift, exactly `arm.xRot = arm.xRot*0.5 -
    //    offset` (not a plain add). `side` is +1 for the right arm, -1 for the left
    //    (the Y inward tuck mirrors). Eating/drinking use the ITEM pose in third
    //    person (§16.3: raising to the mouth + chewing is first-person only, A9).
    constexpr float kItemLiftDegrees = 18.0F;   // 0.3141593 rad
    constexpr float kBlockLiftDegrees = 54.0F;  // 0.9424779 rad
    constexpr float kBlockYawDegrees = 30.0F;   // 0.5235988 rad, inward tuck
    const auto applyArmPose = [&](int armBone, render::player::ArmPose armPose, float side) {
        if (armBone < 0) {
            return;
        }
        glm::vec3& rotation = pose.bone(static_cast<std::size_t>(armBone)).rotation;
        switch (armPose) {
            case render::player::ArmPose::Item:
            case render::player::ArmPose::Eat:  // A9: third person eats with ITEM pose
                rotation.x = rotation.x * 0.5F - kItemLiftDegrees;
                break;
            case render::player::ArmPose::Block:
                rotation.x = rotation.x * 0.5F - kBlockLiftDegrees;
                rotation.y = -side * kBlockYawDegrees;  // inward tuck 30 deg
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
    applyArmPose(bones.rightArm, state.rightArmPose, 1.0F);
    applyArmPose(bones.leftArm, state.leftArmPose, -1.0F);

    // 5. applyAttack (§15): the non-symmetric "whip" swing. The body twists on Y
    //    with a very fast sqrt ease; the swinging arm lifts on X with a quartic
    //    ease-out (fast up ~1 tick, slow down ~5), rolls out on Z, and yaws with
    //    twice the body twist. Amplitudes: body 11.46 deg, arm lift 68.75 deg, arm
    //    roll -22.9 deg. The pivot translation (±5px) and the head-pitch coupling
    //    (h term) are first-person-camera niceties left to the visual pass.
    if (state.swing.active) {
        const float t = std::clamp(state.swing.progress, 0.0F, 1.0F);
        const float bodyTwist = std::sin(std::sqrt(t) * 2.0F * kPi) * toDegrees(0.2F);  // 11.46
        float f = 1.0F - t;
        f *= f;
        f *= f;
        f = 1.0F - f;  // 1 - (1-t)^4, quartic ease-out
        const float lift = std::sin(f * kPi) * toDegrees(1.2F);       // 68.75 deg main lift
        const float roll = std::sin(t * kPi) * toDegrees(-0.4F);      // -22.9 deg outward
        addRotation(pose, bones.body, glm::vec3{0.0F, bodyTwist, 0.0F});
        // The main hand is the right arm; -lift raises it forward (matches the
        // walk swing's forward sign), yRot follows body twist doubled.
        addRotation(pose, bones.rightArm, glm::vec3{-lift, bodyTwist * 2.0F, roll});
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

    // 7. applyIdleBob (§4, A3): AnimationUtils.bobModelPart, the ONLY idle motion,
    //    on both arms (body/head/legs stay still). Z sways outward-only
    //    `cos(age*0.09)*2.86 + 2.86` (0..5.73 deg, mirrored: right +, left -), X
    //    front/back `sin(age*0.067)*2.86` (reversed between arms). It is additive
    //    and persists during walking; skipped while an item use owns the arm.
    const bool armBusy = state.use.active;
    if (!armBusy) {
        constexpr float kBobDegrees = 2.86F;  // 0.05 rad
        const float outward = std::cos(ageInTicks * 0.09F) * kBobDegrees + kBobDegrees;
        const float frontBack = std::sin(ageInTicks * 0.067F) * kBobDegrees;
        addRotation(pose, bones.rightArm, glm::vec3{frontBack, 0.0F, outward});
        addRotation(pose, bones.leftArm, glm::vec3{-frontBack, 0.0F, -outward});
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

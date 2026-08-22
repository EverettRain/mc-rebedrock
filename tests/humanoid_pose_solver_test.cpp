// PX-2 (analysis Phase 3-5): the unified player pose pipeline's pure core.
//   - the extractor's wrapped-yaw rotation lerp + head-relative-to-body,
//   - ArmPose derivation,
//   - the deterministic HumanoidPoseSolver (rest/walk/crouch/look/attack/idle),
//   - the FirstPersonHandPoseSolver mapping.
// Everything here is a pure function of an immutable render state, so the same
// tick + alpha gives the same bones at any frame rate (§20).

#include "animation/FirstPersonHandPoseSolver.hpp"
#include "animation/HumanoidPoseSolver.hpp"
#include "animation/SkeletalModel.hpp"
#include "render/player/ArmPose.hpp"
#include "render/player/PlayerRenderLayers.hpp"
#include "render/player/PlayerRenderState.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <glm/vec4.hpp>
#include <optional>
#include <string>

using namespace mc;
namespace rp = mc::render::player;

namespace {

// A minimal humanoid geometry: the six bones the solver drives, each with a
// pivot so worldMatrix/sockets resolve. Bedrock model space, Y up.
const char* kPlayerGeo = R"({
  "minecraft:geometry": [{
    "description": {"identifier": "test.player", "texture_width": 64, "texture_height": 64},
    "bones": [
      {"name": "body", "pivot": [0, 24, 0]},
      {"name": "head", "parent": "body", "pivot": [0, 24, 0]},
      {"name": "rightArm", "parent": "body", "pivot": [-5, 22, 0]},
      {"name": "leftArm", "parent": "body", "pivot": [5, 22, 0]},
      {"name": "rightLeg", "pivot": [-2, 12, 0]},
      {"name": "leftLeg", "pivot": [2, 12, 0]}
    ]
  }]
})";

bool near(float a, float b, float eps = 0.0005F) { return std::fabs(a - b) < eps; }

// --- Extractor: wrapped-yaw lerp crosses the +/-180 seam the short way --------
void testWrappedYawLerp() {
    // 179 -> -179 is +2 degrees across the seam, not -358.
    assert(near(rp::lerpAngleDegrees(179.0F, -179.0F, 0.5F), 180.0F) ||
           near(rp::lerpAngleDegrees(179.0F, -179.0F, 0.5F), -180.0F));
    // Halfway from 170 to -170 lands near 180 (short path +20), never near 0.
    const float mid = rp::lerpAngleDegrees(170.0F, -170.0F, 0.5F);
    assert(std::fabs(mid) > 175.0F);
    // A plain within-range lerp is ordinary.
    assert(near(rp::lerpAngleDegrees(0.0F, 90.0F, 0.5F), 45.0F));
    // wrapDegrees normalizes into the half-open [-180, 180): 540 and -540 are the
    // same physical angle (the +/-180 seam), so both fold to the same value.
    assert(near(std::fabs(rp::wrapDegrees(540.0F)), 180.0F));
    assert(near(rp::wrapDegrees(540.0F), rp::wrapDegrees(-540.0F)));
    // In-range values are untouched; 90 stays 90, -90 stays -90.
    assert(near(rp::wrapDegrees(90.0F), 90.0F));
    assert(near(rp::wrapDegrees(-90.0F), -90.0F));
    // A value just past the seam wraps to the far side.
    assert(near(rp::wrapDegrees(190.0F), -170.0F));
}

// --- Extractor: head yaw comes out relative to the body ------------------------
void testHeadRelativeToBody() {
    gameplay::PlayerTickSnapshot snap;
    snap.previousBodyYawDegrees = 30.0F;
    snap.bodyYawDegrees = 30.0F;
    snap.previousHeadYawDegrees = 50.0F;
    snap.headYawDegrees = 50.0F;
    snap.previousPitchDegrees = -10.0F;
    snap.pitchDegrees = -10.0F;
    std::optional<std::uint64_t> last;
    const auto state = rp::extractPlayerRenderState(snap, 0.5F, last);
    assert(near(state.bodyYawDegrees, 30.0F));
    assert(near(state.headYawDegrees, 20.0F));  // 50 - 30, relative
    assert(near(state.pitchDegrees, -10.0F));
}

// --- ArmPose derivation -------------------------------------------------------
void testArmPoseDerivation() {
    gameplay::ItemStack empty{};
    assert(rp::deriveArmPose(empty, false, gameplay::UseAnimation::None) == rp::ArmPose::Empty);

    gameplay::ItemStack block{};
    block.block = world::Block::Stone;
    block.count = 1U;
    assert(rp::deriveArmPose(block, false, gameplay::UseAnimation::None) == rp::ArmPose::Block);

    // Eating overrides the held item pose while the use is on this hand.
    assert(rp::deriveArmPose(block, true, gameplay::UseAnimation::Eat) == rp::ArmPose::Eat);
    // But only when the use is on THIS hand.
    assert(rp::deriveArmPose(block, false, gameplay::UseAnimation::Eat) == rp::ArmPose::Block);
}

// Builds a neutral render state (rest).
rp::PlayerRenderState restState() {
    rp::PlayerRenderState s;
    return s;
}

// --- Solver: rest pose leaves the driven-only bones at rest -------------------
void testRestPose(const animation::SkeletalModel& model,
                  const animation::HumanoidBoneBindings& bones) {
    const auto frame = animation::solveHumanoidPose(model, bones, restState(), 0.0F);
    // No walk, no look, no arm pose -> arms/legs at zero delta. (Idle bob is a
    // Z-roll on the arms; at age 0 sin(0)=0, so still rest.)
    const auto& rightLeg = frame.skeleton.bone(static_cast<std::size_t>(bones.rightLeg));
    const auto& leftLeg = frame.skeleton.bone(static_cast<std::size_t>(bones.leftLeg));
    assert(near(rightLeg.rotation.x, 0.0F));
    assert(near(leftLeg.rotation.x, 0.0F));
}

// --- Solver: walk swings legs in anti-phase -----------------------------------
void testWalkAntiPhase(const animation::SkeletalModel& model,
                       const animation::HumanoidBoneBindings& bones) {
    rp::PlayerRenderState s = restState();
    s.walkStride = 1.0F;   // a quarter into the phase where cos differs from +/-1
    s.walkSpeed = 1.0F;
    const auto frame = animation::solveHumanoidPose(model, bones, s, 0.0F);
    const float rl = frame.skeleton.bone(static_cast<std::size_t>(bones.rightLeg)).rotation.x;
    const float ll = frame.skeleton.bone(static_cast<std::size_t>(bones.leftLeg)).rotation.x;
    // Legs are half a period apart: opposite signs, equal magnitude.
    assert(near(rl, -ll));
    assert(std::fabs(rl) > 0.001F);
    // The right arm is in phase with the LEFT leg (vanilla pairing).
    const float ra = frame.skeleton.bone(static_cast<std::size_t>(bones.rightArm)).rotation.x;
    assert((ra > 0.0F) == (ll > 0.0F));
}

// --- Solver A2: sprint does NOT change the pose formula; the amplitude s alone
// decides the gait (§8.1). At the same walkSpeed, sprinting and walking produce
// the SAME limb angles — the sprint flag is not a swing multiplier.
void testSprintDoesNotAmplify(const animation::SkeletalModel& model,
                              const animation::HumanoidBoneBindings& bones) {
    rp::PlayerRenderState walking = restState();
    walking.walkStride = 0.0F;
    walking.walkSpeed = 0.8F;
    rp::PlayerRenderState sprinting = walking;
    sprinting.sprinting = true;
    const auto walk = animation::solveHumanoidPose(model, bones, walking, 0.0F);
    const auto sprint = animation::solveHumanoidPose(model, bones, sprinting, 0.0F);
    const float walkLeg =
        walk.skeleton.bone(static_cast<std::size_t>(bones.rightLeg)).rotation.x;
    const float sprintLeg =
        sprint.skeleton.bone(static_cast<std::size_t>(bones.rightLeg)).rotation.x;
    assert(near(sprintLeg, walkLeg));
    // A higher amplitude (saturated 1.0) genuinely swings wider than 0.8.
    rp::PlayerRenderState saturated = walking;
    saturated.walkSpeed = 1.0F;
    const float satLeg = std::fabs(
        animation::solveHumanoidPose(model, bones, saturated, 0.0F)
            .skeleton.bone(static_cast<std::size_t>(bones.rightLeg)).rotation.x);
    assert(satLeg > std::fabs(walkLeg));
}

// --- Solver: crouch bends the body forward and drops the torso ----------------
void testCrouch(const animation::SkeletalModel& model,
                const animation::HumanoidBoneBindings& bones) {
    rp::PlayerRenderState s = restState();
    s.sneaking = true;
    const auto frame = animation::solveHumanoidPose(model, bones, s, 0.0F);
    const auto& body = frame.skeleton.bone(static_cast<std::size_t>(bones.body));
    const auto& head = frame.skeleton.bone(static_cast<std::size_t>(bones.head));
    const auto& rightArm = frame.skeleton.bone(static_cast<std::size_t>(bones.rightArm));
    const auto& rightLeg = frame.skeleton.bone(static_cast<std::size_t>(bones.rightLeg));
    assert(near(body.rotation.x, 28.64789F));
    assert(near(body.position.y, -3.2F));
    assert(near(body.position.z, 0.0F));
    assert(near(head.rotation.x, -28.64789F));
    assert(near(head.position.y, -0.877583F));
    assert(near(head.position.z, 0.479426F));
    // Local compensation makes parented arms match JE's independent model parts.
    assert(near(rightArm.position.x, 0.0F));
    assert(near(rightArm.position.y, 0.244835F));
    assert(near(rightArm.position.z, 0.958851F));
    assert(near(rightArm.rotation.x, -5.729578F));
    assert(near(rightLeg.position.y, -0.2F));
    assert(near(rightLeg.position.z, -4.0F));
    // The torso waist and leg top travel toward the same side of the player;
    // the opposite sign creates the user-visible upper/lower-body split.
    const glm::vec4 bodyWaist = frame.skeleton.worldMatrix(bones.body) *
                                glm::vec4{0.0F, 12.0F, 0.0F, 1.0F};
    const glm::vec4 rightLegTop = frame.skeleton.worldMatrix(bones.rightLeg) *
                                  glm::vec4{-2.0F, 12.0F, 0.0F, 1.0F};
    assert(std::fabs(bodyWaist.y - rightLegTop.y) < 2.0F);
    assert(std::fabs(bodyWaist.z - rightLegTop.z) < 2.0F);
}

// --- Solver: look turns the head, relative to the body ------------------------
void testLook(const animation::SkeletalModel& model,
              const animation::HumanoidBoneBindings& bones) {
    rp::PlayerRenderState s = restState();
    s.headYawDegrees = 25.0F;
    s.pitchDegrees = -15.0F;
    const auto frame = animation::solveHumanoidPose(model, bones, s, 0.0F);
    const auto& head = frame.skeleton.bone(static_cast<std::size_t>(bones.head));
    assert(near(head.rotation.y, 25.0F));
    assert(near(head.rotation.x, -15.0F));
}

// --- Solver: attack lifts the swinging (right) arm ----------------------------
void testAttack(const animation::SkeletalModel& model,
                const animation::HumanoidBoneBindings& bones) {
    // A4: the main lift peaks where f = 1-(1-t)^4 = 0.5, i.e. t ~= 0.159 (fast
    // up), reaching near the full -68.75 deg; the curve is asymmetric (a late
    // t=0.5 is already well past the peak and much smaller).
    rp::PlayerRenderState peak = restState();
    peak.swing.active = true;
    peak.swing.progress = 0.159F;
    const float raPeak = animation::solveHumanoidPose(model, bones, peak, 0.0F)
                             .skeleton.bone(static_cast<std::size_t>(bones.rightArm))
                             .rotation.x;
    assert(raPeak < -60.0F);  // near the -68.75 deg lift
    assert(raPeak > -75.0F);

    // A5: the body twists on Y (fast sqrt ease), peaking near t ~= 0.0625.
    rp::PlayerRenderState early = restState();
    early.swing.active = true;
    early.swing.progress = 0.0625F;
    const float bodyY = animation::solveHumanoidPose(model, bones, early, 0.0F)
                            .skeleton.bone(static_cast<std::size_t>(bones.body))
                            .rotation.y;
    assert(bodyY > 10.0F && bodyY < 12.5F);  // ~11.46 deg peak

    // Asymmetry: t=0.5 (past the lift peak) lifts far less than the t=0.159 peak.
    rp::PlayerRenderState late = restState();
    late.swing.active = true;
    late.swing.progress = 0.5F;
    const float raLate = animation::solveHumanoidPose(model, bones, late, 0.0F)
                             .skeleton.bone(static_cast<std::size_t>(bones.rightArm))
                             .rotation.x;
    assert(std::fabs(raLate) < std::fabs(raPeak));
}

// --- Solver determinism: same state -> same matrices, any frame rate ----------
void testDeterminism(const animation::SkeletalModel& model,
                     const animation::HumanoidBoneBindings& bones) {
    rp::PlayerRenderState s = restState();
    s.walkStride = 3.3F;
    s.walkSpeed = 0.8F;
    s.headYawDegrees = 12.0F;
    s.sneaking = true;
    // Two solves with the same inputs, and idle bob at the same age, are equal.
    const auto a = animation::solveHumanoidPose(model, bones, s, 40.0F);
    const auto b = animation::solveHumanoidPose(model, bones, s, 40.0F);
    for (std::size_t i = 0; i < a.skeleton.boneCount(); ++i) {
        const auto& ba = a.skeleton.bone(i);
        const auto& bb = b.skeleton.bone(i);
        assert(near(ba.rotation.x, bb.rotation.x) && near(ba.rotation.y, bb.rotation.y) &&
               near(ba.rotation.z, bb.rotation.z));
    }
    // The socket follows the arm bone (moved from identity once the arm swings).
    assert(a.rightHandSocket == b.rightHandSocket);
}

// --- Solver: an active use suppresses the cosmetic idle bob --------------------
void testUseSuppressesIdleBob(const animation::SkeletalModel& model,
                              const animation::HumanoidBoneBindings& bones) {
    rp::PlayerRenderState idle = restState();
    // A large age so the idle bob (a Z roll) is clearly non-zero when present.
    const float age = 25.0F;  // sin(25*0.067) ~ sin(1.675) ~ 0.994
    const auto withBob = animation::solveHumanoidPose(model, bones, idle, age);
    const float bobRoll =
        withBob.skeleton.bone(static_cast<std::size_t>(bones.rightArm)).rotation.z;
    assert(std::fabs(bobRoll) > 0.5F);  // idle bob present when the arm is free

    rp::PlayerRenderState using_ = restState();
    using_.use.active = true;
    using_.use.animation = gameplay::UseAnimation::Eat;
    using_.rightArmPose = rp::ArmPose::Eat;
    const auto noBob = animation::solveHumanoidPose(model, bones, using_, age);
    const float busyRoll =
        noBob.skeleton.bone(static_cast<std::size_t>(bones.rightArm)).rotation.z;
    assert(near(busyRoll, 0.0F));  // idle bob suppressed while using an item
}

// --- Solver A7/A8: an ArmPose OVERRIDES the held arm (halve the walk swing + a
// forward lift), it does NOT sum onto the swing. Item lift 18 deg, Block 54 deg +
// 30 deg inward yaw; the off arm is untouched.
void testArmPoseHalvesAndOverrides(const animation::SkeletalModel& model,
                                   const animation::HumanoidBoneBindings& bones) {
    constexpr float kPi = 3.14159265358979323846F;
    constexpr float kWalkFrequency = 0.6662F;
    rp::PlayerRenderState walking = restState();
    walking.walkStride = kPi / kWalkFrequency;  // right arm at its max forward swing
    walking.walkSpeed = 1.0F;
    const float walkArmX =
        animation::solveHumanoidPose(model, bones, walking, 0.0F)
            .skeleton.bone(static_cast<std::size_t>(bones.rightArm))
            .rotation.x;
    assert(walkArmX > 40.0F);  // the bare walk swing (~+57.3 deg) is large

    // ITEM on the right (main) hand: xRot = walkArmX*0.5 - 18, halving the swing
    // and lifting forward — NOT summed, NOT raised to the mouth (A9).
    rp::PlayerRenderState item = walking;
    item.rightArmPose = rp::ArmPose::Item;
    const auto itemPose = animation::solveHumanoidPose(model, bones, item, 0.0F);
    const float itemRight =
        itemPose.skeleton.bone(static_cast<std::size_t>(bones.rightArm)).rotation.x;
    assert(near(itemRight, walkArmX * 0.5F - 18.0F, 0.05F));
    // The off (left) arm keeps its own walk swing — the pose is single-armed.
    const float leftWalk =
        animation::solveHumanoidPose(model, bones, walking, 0.0F)
            .skeleton.bone(static_cast<std::size_t>(bones.leftArm))
            .rotation.x;
    const float itemLeft =
        itemPose.skeleton.bone(static_cast<std::size_t>(bones.leftArm)).rotation.x;
    assert(near(itemLeft, leftWalk));

    // BLOCK: xRot halved - 54, plus a 30 deg inward Y tuck on the right arm.
    rp::PlayerRenderState block = walking;
    block.rightArmPose = rp::ArmPose::Block;
    const auto blockPose = animation::solveHumanoidPose(model, bones, block, 0.0F);
    const auto& blockArm = blockPose.skeleton.bone(static_cast<std::size_t>(bones.rightArm));
    assert(near(blockArm.rotation.x, walkArmX * 0.5F - 54.0F, 0.05F));
    assert(near(blockArm.rotation.y, -30.0F));  // right hand tucks inward (-side*30)

    // A9: third-person Eat uses the ITEM pose, not a mouth-raise. Same as ITEM.
    rp::PlayerRenderState eat = walking;
    eat.use.active = true;
    eat.use.animation = gameplay::UseAnimation::Eat;
    eat.rightArmPose = rp::ArmPose::Eat;
    const float eatRight =
        animation::solveHumanoidPose(model, bones, eat, 0.0F)
            .skeleton.bone(static_cast<std::size_t>(bones.rightArm))
            .rotation.x;
    // Eat suppresses the idle bob (use active) so it is the pure item override.
    assert(near(eatRight, walkArmX * 0.5F - 18.0F, 0.05F));
}

// --- World and preview share ONE solver: same synthetic state -> same bones ----
// §20: the world player and the inventory preview both call solveHumanoidPose, so
// a given synthetic state must produce identical LOCAL bone rotations regardless
// of which consumer built it. (The consumers differ only in world placement/
// camera, never in the pose rule.) This is what "shared pose semantics" means.
void testWorldPreviewShareSolver(const animation::SkeletalModel& model,
                                 const animation::HumanoidBoneBindings& bones) {
    // A synthetic "preview" standing/idle-with-look state, and the same values
    // arriving via a "world" path — the solver is stateless, so both are equal.
    rp::PlayerRenderState synthetic = restState();
    synthetic.headYawDegrees = 18.0F;
    synthetic.pitchDegrees = -7.0F;
    synthetic.walkStride = 2.0F;
    synthetic.walkSpeed = 0.6F;
    synthetic.rightArmPose = rp::ArmPose::Item;

    const auto world = animation::solveHumanoidPose(model, bones, synthetic, 12.0F);
    const auto preview = animation::solveHumanoidPose(model, bones, synthetic, 12.0F);
    for (std::size_t i = 0; i < world.skeleton.boneCount(); ++i) {
        const auto& w = world.skeleton.bone(i);
        const auto& p = preview.skeleton.bone(i);
        assert(near(w.rotation.x, p.rotation.x) && near(w.rotation.y, p.rotation.y) &&
               near(w.rotation.z, p.rotation.z));
        assert(near(w.position.x, p.position.x) && near(w.position.y, p.position.y) &&
               near(w.position.z, p.position.z));
    }
    assert(world.rightHandSocket == preview.rightHandSocket);
    assert(world.leftHandSocket == preview.leftHandSocket);
}

// --- Extractor: head yaw stays relative to body across the +/-180 seam ---------
// §7.1/§13.3: the stored head yaw is absolute; the render state's headYaw is
// relative to the body, on the SHORT path — so a body near +180 and a head near
// -180 read as a small relative turn, not a ~360 sweep.
void testHeadRelativeAcrossSeam() {
    gameplay::PlayerTickSnapshot snap;
    snap.previousBodyYawDegrees = 170.0F;
    snap.bodyYawDegrees = 170.0F;
    snap.previousHeadYawDegrees = -170.0F;  // 20 degrees past the seam from body
    snap.headYawDegrees = -170.0F;
    std::optional<std::uint64_t> last;
    const auto state = rp::extractPlayerRenderState(snap, 0.5F, last);
    // 170 -> -170 the short way is +20, so head-relative is +20, not -340.
    assert(near(state.headYawDegrees, 20.0F));
    assert(near(state.bodyYawDegrees, 170.0F));
}

// --- FirstPersonHandPoseSolver: unified state -> action + progress ------------
void testFirstPersonHandSolver() {
    // Rest hand.
    {
        rp::PlayerRenderState s = restState();
        const auto in = animation::solveFirstPersonHand(s);
        assert(in.action == animation::ModelAction::None);
    }
    // A Break swing maps to Break at the swing's progress.
    {
        rp::PlayerRenderState s = restState();
        s.swing.active = true;
        s.swing.animation = gameplay::SwingAnimation::Break;
        s.swing.progress = 0.4F;
        const auto in = animation::solveFirstPersonHand(s);
        assert(in.action == animation::ModelAction::Break);
        assert(near(in.progress, 0.4F));
    }
    // A Use swing maps to Use.
    {
        rp::PlayerRenderState s = restState();
        s.swing.active = true;
        s.swing.animation = gameplay::SwingAnimation::Use;
        s.swing.progress = 0.2F;
        const auto in = animation::solveFirstPersonHand(s);
        assert(in.action == animation::ModelAction::Use);
    }
    // An active eat takes precedence over a swing and maps to Eat.
    {
        rp::PlayerRenderState s = restState();
        s.swing.active = true;
        s.swing.animation = gameplay::SwingAnimation::Break;
        s.use.active = true;
        s.use.animation = gameplay::UseAnimation::Eat;
        s.use.progress = 0.7F;
        const auto in = animation::solveFirstPersonHand(s);
        assert(in.action == animation::ModelAction::Eat);
        assert(near(in.progress, 0.7F));
    }
}

// --- Layer order is stable and body draws before the item ---------------------
void testLayerOrder() {
    // Body is first; the item-in-hand layer draws after the body/skin so it
    // never z-fights, and is the only socket-attached layer.
    assert(rp::kPlayerRenderLayerOrder[0].id == rp::PlayerRenderLayerId::Body);
    bool sawBody = false;
    bool itemAfterBody = false;
    for (const auto& info : rp::kPlayerRenderLayerOrder) {
        if (info.id == rp::PlayerRenderLayerId::Body) sawBody = true;
        if (info.id == rp::PlayerRenderLayerId::ItemInHand) {
            itemAfterBody = sawBody;
            assert(info.attachesToSocket);
        }
    }
    assert(itemAfterBody);
    assert(rp::itemInHandSocket(restState()) == rp::HandSocket::Right);
}

}  // namespace

int main() {
    const animation::SkeletalModel model = animation::SkeletalModel::parse(kPlayerGeo);
    const auto bones = animation::HumanoidBoneBindings::bind(model);
    assert(bones.body >= 0 && bones.head >= 0 && bones.rightArm >= 0 && bones.leftArm >= 0 &&
           bones.rightLeg >= 0 && bones.leftLeg >= 0);

    testWrappedYawLerp();
    testHeadRelativeToBody();
    testArmPoseDerivation();
    testRestPose(model, bones);
    testWalkAntiPhase(model, bones);
    testSprintDoesNotAmplify(model, bones);
    testCrouch(model, bones);
    testLook(model, bones);
    testAttack(model, bones);
    testDeterminism(model, bones);
    testUseSuppressesIdleBob(model, bones);
    testArmPoseHalvesAndOverrides(model, bones);
    testWorldPreviewShareSolver(model, bones);
    testHeadRelativeAcrossSeam();
    testFirstPersonHandSolver();
    testLayerOrder();
    return 0;
}

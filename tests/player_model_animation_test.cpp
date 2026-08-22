#include "animation/PlayerModelAnimator.hpp"

#include <cassert>
#include <cmath>
#include <glm/geometric.hpp>

int main() {
    mc::animation::PlayerModelAnimator animator;
    animator.setCursorLook(2.0F, -2.0F);
    animator.update(0.1F, false);
    assert(animator.pose().headYaw > 0.0F);
    assert(animator.pose().headPitch < 0.0F);
    // Body-follow is off by default (the world player's body turns through the
    // renderer's separate "head leads, body follows" yaw, not the look clip), so
    // the body bone must stay put while the head alone looks at the cursor.
    assert(std::abs(animator.pose().bodyYaw) < 1e-5F);

    // The inventory preview enables body-follow: the look clip then rotates the
    // body bone at half the head's yaw amplitude (vanilla EntityRenderer#drawEntity
    // uses bodyYaw = f*20 vs yaw = f*40), driven by the animation library rather
    // than per-part poses. The preview renderer applies bodyYaw to the body/arms/
    // legs and bodyYaw + headYaw to the head, so the body follows while the head
    // keeps its full look amplitude.
    animator.setBodyFollowsLook(true);
    animator.update(0.1F, false);
    assert(animator.pose().bodyYaw > 0.0F);
    assert(animator.pose().bodyYaw <= animator.pose().headYaw);
    const float headTotal = animator.pose().bodyYaw + animator.pose().headYaw;
    assert(headTotal > animator.pose().bodyYaw);
    assert(std::abs(headTotal - 2.0F * animator.pose().bodyYaw) < 1e-4F);
    const float idleArm = animator.pose().rightArmPitch;
    animator.update(0.1F, true);
    assert(animator.pose().rightArmPitch != idleArm);
    // B1/B2 diagonal sync: rightArm and leftLeg share the same walk phase (both
    // cos(p*0.6662 + PI)), so they swing in the SAME direction. The leg amplitude
    // is 1.4x the arm's (80.2 vs 57.3 deg), so the pitches match in sign and in
    // the ~1.4 ratio, not in exact value.
    const float arm = animator.pose().rightArmPitch;
    const float leg = animator.pose().leftLegPitch;
    if (std::abs(arm) > 1e-4F) {
        assert((arm > 0.0F) == (leg > 0.0F));            // same direction
        assert(std::abs(leg / arm - 1.4F) < 0.05F);      // leg = arm * 1.4
    }

    // The skeletal pose (used by the third-person world renderer) is exposed and
    // consistent with the flat preview pose.
    const auto& model = animator.model();
    const auto& skeleton = animator.skeletonPose();
    const int head = model.findBone("head");
    const int rightLeg = model.findBone("rightLeg");
    assert(head >= 0 && rightLeg >= 0);
    assert(skeleton.boneCount() == model.boneCount());
    // Walking swings the leg, so its world transform differs from rest.
    const glm::vec4 knee{0.0F, 6.0F, 2.0F, 1.0F};
    mc::animation::PlayerModelAnimator resting;
    resting.update(0.0F, false);
    const glm::vec4 restKnee = resting.skeletonPose().worldMatrix(rightLeg) * knee;
    const glm::vec4 walkKnee = skeleton.worldMatrix(rightLeg) * knee;
    assert(glm::length(glm::vec3(restKnee - walkKnee)) > 0.5F);

    // Crouching (Shift) must not separate the upper and lower body: the torso
    // hinges at the waist and its base stays seated on the planted legs. Before
    // the fix the body pivoted at its neck and its base swung ~5.6 units off the
    // hips, leaving a visible gap in third person.
    mc::animation::PlayerModelAnimator sneaker;
    for (int i = 0; i < 40; ++i) {
        sneaker.update(0.05F, false, /*sneaking=*/true);
    }
    const int body = sneaker.model().findBone("body");
    assert(body >= 0);
    const glm::vec4 waist{0.0F, 12.0F, 0.0F, 1.0F};
    const glm::vec4 worldWaist = sneaker.skeletonPose().worldMatrix(body) * waist;
    // The waist joint (top of the legs) must stay put; only the torso above it
    // leans. Tolerate the small idle-bob bob on Y.
    assert(std::abs(worldWaist.x) < 0.5F);
    assert(std::abs(worldWaist.z) < 0.5F);
    assert(std::abs(worldWaist.y - 12.0F) < 1.0F);

    // --- ANIM task2: the world-player feed (updateWorldPlayer) drives the SAME
    // controller stack from the authoritative WalkAnimationState. These migrate
    // the spec behaviours the retired HumanoidPoseSolver's test guarded: idle ->
    // no limb swing, walk -> anti-phase swing with amplitude, and determinism.
    const int rArm = model.findBone("rightArm");
    const int lArm = model.findBone("leftArm");
    const int lLeg = model.findBone("leftLeg");
    assert(rArm >= 0 && lArm >= 0 && lLeg >= 0 && rightLeg >= 0);

    // Idle: amplitude 0, phase irrelevant -> legs do NOT swing (the "stops but
    // keeps swinging" regression the solver fix targeted). Only the idle arm bob
    // (Z/X on the arms) moves; legs stay at rest.
    {
        mc::animation::PlayerModelAnimator wp;
        // A steady age so the idle bob is well into its cycle, proving legs are
        // still untouched by it.
        for (int i = 0; i < 10; ++i) {
            wp.updateWorldPlayer(0.05F, /*walkAmount=*/0.0F, /*walkPosition=*/0.0F,
                                 /*ageInTicks=*/static_cast<float>(i), /*sneaking=*/false);
        }
        const auto& p = wp.skeletonPose();
        assert(std::abs(p.bone(static_cast<std::size_t>(rightLeg)).rotation.x) < 0.01F);
        assert(std::abs(p.bone(static_cast<std::size_t>(lLeg)).rotation.x) < 0.01F);
    }

    // Walk: amplitude 0.86 at a phase where cos differs from +/-1 -> legs swing in
    // anti-phase (opposite signs), and rightArm shares leftLeg's phase.
    {
        mc::animation::PlayerModelAnimator wp;
        // Feed several ticks so the idle->walk controller crossfade completes (the
        // state blend eases in over a few frames), holding the phase fixed so the
        // final swing angle is deterministic.
        for (int i = 0; i < 30; ++i) {
            // walkPosition fixed at 1.5 so 0.6662*p ~ 1 rad (cos ~ 0.54), a clear
            // swing, once the walk state has fully faded in.
            wp.updateWorldPlayer(0.05F, /*walkAmount=*/0.863F, /*walkPosition=*/1.5F,
                                 /*ageInTicks=*/0.0F, /*sneaking=*/false);
        }
        const auto& p = wp.skeletonPose();
        const float rl = p.bone(static_cast<std::size_t>(rightLeg)).rotation.x;
        const float ll = p.bone(static_cast<std::size_t>(lLeg)).rotation.x;
        assert(std::abs(rl) > 1.0F);              // legs actually swing
        assert((rl > 0.0F) != (ll > 0.0F));       // anti-phase (opposite signs)
        const float ra = p.bone(static_cast<std::size_t>(rArm)).rotation.x;
        assert((ra > 0.0F) == (ll > 0.0F));       // rightArm in phase with leftLeg
    }

    // Determinism: identical world-player inputs -> identical pose (no frame-rate
    // or hidden-state dependence), the property the solver test asserted.
    {
        mc::animation::PlayerModelAnimator a;
        mc::animation::PlayerModelAnimator b;
        a.updateWorldPlayer(0.05F, 0.7F, 3.3F, 12.0F, true);
        b.updateWorldPlayer(0.05F, 0.7F, 3.3F, 12.0F, true);
        const auto& pa = a.skeletonPose();
        const auto& pb = b.skeletonPose();
        for (std::size_t i = 0; i < pa.boneCount(); ++i) {
            const auto& ba = pa.bone(i);
            const auto& bb = pb.bone(i);
            assert(std::abs(ba.rotation.x - bb.rotation.x) < 1e-4F);
            assert(std::abs(ba.rotation.y - bb.rotation.y) < 1e-4F);
            assert(std::abs(ba.rotation.z - bb.rotation.z) < 1e-4F);
        }
    }
    return 0;
}

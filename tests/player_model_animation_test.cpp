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
    assert(animator.pose().leftLegPitch == animator.pose().rightArmPitch);

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
    return 0;
}

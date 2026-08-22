#include "animation/AnimationClip.hpp"
#include "animation/Animator.hpp"
#include "animation/BoneMask.hpp"
#include "animation/SkeletalModel.hpp"

#include <cassert>
#include <cmath>

using namespace mc::animation;

namespace {

// A player-like skeleton: torso ("body") with head + two arms as children, and
// two root-level legs (matching PlayerModelAnimator's geometry, where legs are
// their own roots so they stay planted).
constexpr const char* kPlayerGeometry = R"({
  "format_version": "1.12.0",
  "minecraft:geometry": [
    { "description": {"identifier":"geometry.player","texture_width":64,"texture_height":64},
      "bones": [
        {"name":"body","pivot":[0,24,0],"cubes":[{"origin":[-4,12,-2],"size":[8,12,4],"uv":[16,16]}]},
        {"name":"head","parent":"body","pivot":[0,24,0],"cubes":[{"origin":[-4,24,-4],"size":[8,8,8],"uv":[0,0]}]},
        {"name":"hat","parent":"head","pivot":[0,24,0],"cubes":[{"origin":[-4,24,-4],"size":[8,8,8],"uv":[32,0]}]},
        {"name":"rightArm","parent":"body","pivot":[-5,22,0],"cubes":[{"origin":[-8,12,-2],"size":[4,12,4],"uv":[40,16]}]},
        {"name":"leftArm","parent":"body","pivot":[5,22,0],"cubes":[{"origin":[4,12,-2],"size":[4,12,4],"uv":[32,48]}]},
        {"name":"rightLeg","pivot":[-1.9,12,0],"cubes":[{"origin":[-3.9,0,-2],"size":[4,12,4],"uv":[0,16]}]},
        {"name":"leftLeg","pivot":[1.9,12,0],"cubes":[{"origin":[-0.1,0,-2],"size":[4,12,4],"uv":[16,48]}]}
      ]
    }
  ]
})";

// A full-body walk: arms swing and legs stride. Constant literals (no time
// dependence) keep the assertions exact.
constexpr const char* kWalk = R"({
  "format_version":"1.8.0",
  "animations": {
    "walk": {
      "loop": true, "animation_length": 1.0,
      "bones": {
        "rightLeg": {"rotation":[30,0,0]},
        "leftLeg":  {"rotation":[-30,0,0]},
        "rightArm": {"rotation":[-30,0,0]},
        "leftArm":  {"rotation":[30,0,0]},
        "body":     {"rotation":[5,0,0]},
        "head":     {"rotation":[2,0,0]}
      }
    }
  }
})";

// An item-hold pose that touches every bone the walk does, so a mask is the only
// thing that can keep them apart.
constexpr const char* kItemPose = R"({
  "format_version":"1.8.0",
  "animations": {
    "item": {
      "loop": true, "animation_length": 1.0,
      "bones": {
        "rightArm": {"rotation":[-90,0,0]},
        "leftArm":  {"rotation":[-90,0,0]},
        "head":     {"rotation":[10,0,0]},
        "body":     {"rotation":[1,0,0]},
        "rightLeg": {"rotation":[100,0,0]},
        "leftLeg":  {"rotation":[100,0,0]}
      }
    }
  }
})";

float rot(const SkeletonPose& pose, const SkeletalModel& model, const char* bone) {
    return pose.bone(static_cast<std::size_t>(model.findBone(bone))).rotation.x;
}

} // namespace

int main() {
    const SkeletalModel model = SkeletalModel::parse(kPlayerGeometry);
    assert(model.boneCount() == 7U);

    const int body = model.findBone("body");
    const int head = model.findBone("head");
    const int hat = model.findBone("hat");
    const int rightArm = model.findBone("rightArm");
    const int leftArm = model.findBone("leftArm");
    const int rightLeg = model.findBone("rightLeg");
    const int leftLeg = model.findBone("leftLeg");

    // --- Named groups ---------------------------------------------------------
    const BoneGroups groups = buildBoneGroups(model);

    // upper contains torso, head, hat (child of head), and both arms; never legs.
    assert(groups.upperBody.test(body));
    assert(groups.upperBody.test(head));
    assert(groups.upperBody.test(hat)); // subtree: hat rides the head
    assert(groups.upperBody.test(rightArm));
    assert(groups.upperBody.test(leftArm));
    assert(!groups.upperBody.test(rightLeg));
    assert(!groups.upperBody.test(leftLeg));

    // lower is exactly the legs; never arms/head/torso.
    assert(groups.lowerBody.test(rightLeg));
    assert(groups.lowerBody.test(leftLeg));
    assert(!groups.lowerBody.test(rightArm));
    assert(!groups.lowerBody.test(leftArm));
    assert(!groups.lowerBody.test(head));
    assert(!groups.lowerBody.test(body));
    assert(groups.lowerBody.count() == 2U);

    // head group = head + hat only; arms group = the two arms only.
    assert(groups.head.test(head) && groups.head.test(hat));
    assert(!groups.head.test(body) && !groups.head.test(rightArm));
    assert(groups.arms.count() == 2U);
    assert(groups.arms.test(rightArm) && groups.arms.test(leftArm));

    // Free-function builders agree with the struct.
    assert(upperBodyMask(model).count() == groups.upperBody.count());
    assert(lowerBodyMask(model).count() == groups.lowerBody.count());

    // --- Mask gating: an upper-body layer must not touch legs -----------------
    const AnimationLibrary walkLib = AnimationLibrary::parse(kWalk);
    const AnimationLibrary itemLib = AnimationLibrary::parse(kItemPose);
    const AnimationClip* walk = walkLib.find("walk");
    const AnimationClip* item = itemLib.find("item");
    assert(walk != nullptr && item != nullptr);

    const BoneMask upper = groups.upperBody;
    const BoneMask lower = groups.lowerBody;

    {
        Animator a;
        a.setModel(&model);
        a.addLayer(*item, 0.0F, 1.0F, &upper); // item pose, upper only
        const SkeletonPose pose = a.evaluate();
        // Legs are outside the upper mask -> untouched (rest = 0), even though the
        // clip authored 100 degrees on them.
        assert(std::abs(rot(pose, model, "rightLeg")) < 1e-4F);
        assert(std::abs(rot(pose, model, "leftLeg")) < 1e-4F);
        // Arms/head are inside the mask -> written.
        assert(std::abs(rot(pose, model, "rightArm") - (-90.0F)) < 1e-3F);
        assert(std::abs(rot(pose, model, "head") - 10.0F) < 1e-3F);
    }

    {
        Animator a;
        a.setModel(&model);
        a.addLayer(*item, 0.0F, 1.0F, &lower); // item pose, lower only
        const SkeletonPose pose = a.evaluate();
        // Reverse: only legs move; arms/head stay at rest.
        assert(std::abs(rot(pose, model, "rightLeg") - 100.0F) < 1e-3F);
        assert(std::abs(rot(pose, model, "rightArm")) < 1e-4F);
        assert(std::abs(rot(pose, model, "head")) < 1e-4F);
    }

    // --- Separation (the core use case): base walk + upper item override ------
    {
        Animator a;
        a.setModel(&model);
        a.addLayer(*walk, 0.0F, 1.0F);            // full-body walk
        a.addLayer(*item, 0.0F, 1.0F, &upper);    // item pose masked to upper
        const SkeletonPose pose = a.evaluate();

        // Legs: only the walk reached them (upper layer gated out) -> pure stride.
        assert(std::abs(rot(pose, model, "rightLeg") - 30.0F) < 1e-3F);
        assert(std::abs(rot(pose, model, "leftLeg") - (-30.0F)) < 1e-3F);
        // Arms: both layers reached them and additively stack (walk -30 + item -90).
        assert(std::abs(rot(pose, model, "rightArm") - (-120.0F)) < 1e-3F);
        // Head: walk 2 + item 10 = 12.
        assert(std::abs(rot(pose, model, "head") - 12.0F) < 1e-3F);
    }

    // --- Null-mask regression: masked-null == the pre-mask path ---------------
    {
        Animator masked;
        masked.setModel(&model);
        masked.addLayer(*walk, 0.0F, 1.0F, nullptr);
        masked.addLayer(*item, 0.0F, 0.5F, nullptr);
        const SkeletonPose maskedPose = masked.evaluate();

        Animator plain;
        plain.setModel(&model);
        plain.addLayer(*walk, 0.0F, 1.0F); // default mask arg (also null)
        plain.addLayer(*item, 0.0F, 0.5F);
        const SkeletonPose plainPose = plain.evaluate();

        for (std::size_t i = 0U; i < model.boneCount(); ++i) {
            const glm::vec3 m = maskedPose.bone(i).rotation;
            const glm::vec3 p = plainPose.bone(i).rotation;
            assert(std::abs(m.x - p.x) < 1e-6F);
            assert(std::abs(m.y - p.y) < 1e-6F);
            assert(std::abs(m.z - p.z) < 1e-6F);
        }
        // And it must equal every bone getting both layers unconditionally.
        assert(std::abs(plainPose.bone(static_cast<std::size_t>(rightLeg)).rotation.x -
                        (30.0F + 100.0F * 0.5F)) < 1e-3F);
    }

    return 0;
}

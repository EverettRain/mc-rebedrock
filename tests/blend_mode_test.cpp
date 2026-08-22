#include "animation/AnimationClip.hpp"
#include "animation/Animator.hpp"
#include "animation/BoneMask.hpp"
#include "animation/SkeletalModel.hpp"

#include <cassert>
#include <cmath>

using namespace mc::animation;

namespace {

constexpr const char* kPlayerGeometry = R"({
  "format_version": "1.12.0",
  "minecraft:geometry": [
    { "description": {"identifier":"geometry.player","texture_width":64,"texture_height":64},
      "bones": [
        {"name":"body","pivot":[0,24,0],"cubes":[{"origin":[-4,12,-2],"size":[8,12,4],"uv":[16,16]}]},
        {"name":"head","parent":"body","pivot":[0,24,0],"cubes":[{"origin":[-4,24,-4],"size":[8,8,8],"uv":[0,0]}]},
        {"name":"rightArm","parent":"body","pivot":[-5,22,0],"cubes":[{"origin":[-8,12,-2],"size":[4,12,4],"uv":[40,16]}]},
        {"name":"leftArm","parent":"body","pivot":[5,22,0],"cubes":[{"origin":[4,12,-2],"size":[4,12,4],"uv":[32,48]}]},
        {"name":"rightLeg","pivot":[-1.9,12,0],"cubes":[{"origin":[-3.9,0,-2],"size":[4,12,4],"uv":[0,16]}]},
        {"name":"leftLeg","pivot":[1.9,12,0],"cubes":[{"origin":[-0.1,0,-2],"size":[4,12,4],"uv":[16,48]}]}
      ]
    }
  ]
})";

// Full-body walk (constant literals for exact assertions).
constexpr const char* kWalk = R"({
  "format_version":"1.8.0",
  "animations": { "walk": { "loop": true, "animation_length": 1.0, "bones": {
    "rightArm": {"rotation":[-30,0,0]},
    "leftArm":  {"rotation":[30,0,0]},
    "rightLeg": {"rotation":[30,0,0]},
    "leftLeg":  {"rotation":[-30,0,0]},
    "head":     {"rotation":[2,0,0]},
    "body":     {"rotation":[0,0,0], "scale":[1.5,1.5,1.5]}
  }}}
})";

// An item-hold pose to override the arms/head with.
constexpr const char* kItem = R"({
  "format_version":"1.8.0",
  "animations": { "item": { "loop": true, "animation_length": 1.0, "bones": {
    "rightArm": {"rotation":[-90,0,0], "position":[0,1,0]},
    "leftArm":  {"rotation":[-90,0,0]},
    "head":     {"rotation":[10,0,0]},
    "rightLeg": {"rotation":[100,0,0]}
  }}}
})";

float rotX(const SkeletonPose& pose, const SkeletalModel& model, const char* bone) {
    return pose.bone(static_cast<std::size_t>(model.findBone(bone))).rotation.x;
}
float posY(const SkeletonPose& pose, const SkeletalModel& model, const char* bone) {
    return pose.bone(static_cast<std::size_t>(model.findBone(bone))).position.y;
}
float scaleX(const SkeletonPose& pose, const SkeletalModel& model, const char* bone) {
    return pose.bone(static_cast<std::size_t>(model.findBone(bone))).scale.x;
}

} // namespace

int main() {
    const SkeletalModel model = SkeletalModel::parse(kPlayerGeometry);
    const AnimationClip* walk = AnimationLibrary::parse(kWalk).find("walk");
    // Own the libraries so the clip pointers stay valid.
    const AnimationLibrary walkLib = AnimationLibrary::parse(kWalk);
    const AnimationLibrary itemLib = AnimationLibrary::parse(kItem);
    walk = walkLib.find("walk");
    const AnimationClip* item = itemLib.find("item");
    assert(walk != nullptr && item != nullptr);

    // --- Override, weight 1: replaces the bone outright (no additive leak) -----
    {
        Animator a;
        a.setModel(&model);
        a.addLayer(*walk, 0.0F, 1.0F);                                        // arm = -30
        a.addLayer(*item, 0.0F, 1.0F, nullptr, BlendMode::Override);          // arm := -90
        const SkeletonPose pose = a.evaluate();
        // Override wins outright: exactly the item value, not walk(-30)+item(-90).
        assert(std::abs(rotX(pose, model, "rightArm") - (-90.0F)) < 1e-4F);
        assert(std::abs(posY(pose, model, "rightArm") - 1.0F) < 1e-4F);
        // A bone the override does not author (leftLeg) still carries the walk.
        assert(std::abs(rotX(pose, model, "leftLeg") - (-30.0F)) < 1e-4F);
    }

    // --- Override, weight 0.5: halfway lerp between walk and item -------------
    {
        Animator a;
        a.setModel(&model);
        a.addLayer(*walk, 0.0F, 1.0F);                                        // arm = -30
        a.addLayer(*item, 0.0F, 0.5F, nullptr, BlendMode::Override);          // lerp 50%
        const SkeletonPose pose = a.evaluate();
        // lerp(-30, -90, 0.5) = -60.
        assert(std::abs(rotX(pose, model, "rightArm") - (-60.0F)) < 1e-3F);
        // Override scale lerp: item has no scale channel on body, walk set 1.5.
        assert(std::abs(scaleX(pose, model, "body") - 1.5F) < 1e-4F);
    }

    // --- Override + mask (★): upper item pose replaces arms/head only --------
    {
        const BoneMask upper = upperBodyMask(model);
        Animator a;
        a.setModel(&model);
        a.addLayer(*walk, 0.0F, 1.0F);                                        // full-body walk
        a.addLayer(*item, 0.0F, 1.0F, &upper, BlendMode::Override);          // upper override
        const SkeletonPose pose = a.evaluate();
        // Arms replaced (not summed): exactly the item value.
        assert(std::abs(rotX(pose, model, "rightArm") - (-90.0F)) < 1e-3F);
        assert(std::abs(rotX(pose, model, "head") - 10.0F) < 1e-3F);
        // Legs are outside the mask -> pure walk, untouched by the override.
        assert(std::abs(rotX(pose, model, "rightLeg") - 30.0F) < 1e-3F);
        assert(std::abs(rotX(pose, model, "leftLeg") - (-30.0F)) < 1e-3F);
    }

    // --- Composition order: additives sum first, overrides apply after -------
    // Queue the override BEFORE a second additive; the additive must still be
    // summed into the base the override lerps against (two-pass order).
    {
        Animator a;
        a.setModel(&model);
        a.addLayer(*item, 0.0F, 1.0F, nullptr, BlendMode::Override);          // queued first
        a.addLayer(*walk, 0.0F, 1.0F);                                        // additive after
        const SkeletonPose pose = a.evaluate();
        // rightArm: additive walk(-30) is the base, override lerps to item(-90)
        // at weight 1 -> -90, regardless of queue order.
        assert(std::abs(rotX(pose, model, "rightArm") - (-90.0F)) < 1e-3F);
    }

    // --- Additive regression: mode defaults + explicit Additive == pre-ANIM-2 -
    {
        Animator def;
        def.setModel(&model);
        def.addLayer(*walk, 0.0F, 1.0F);            // default mode
        def.addLayer(*item, 0.0F, 0.5F);            // default mode
        const SkeletonPose defPose = def.evaluate();

        Animator expl;
        expl.setModel(&model);
        expl.addLayer(*walk, 0.0F, 1.0F, nullptr, BlendMode::Additive);
        expl.addLayer(*item, 0.0F, 0.5F, nullptr, BlendMode::Additive);
        const SkeletonPose explPose = expl.evaluate();

        for (std::size_t i = 0U; i < model.boneCount(); ++i) {
            const glm::vec3 d = defPose.bone(i).rotation;
            const glm::vec3 e = explPose.bone(i).rotation;
            assert(std::abs(d.x - e.x) < 1e-6F && std::abs(d.y - e.y) < 1e-6F &&
                   std::abs(d.z - e.z) < 1e-6F);
            const glm::vec3 ds = defPose.bone(i).scale;
            const glm::vec3 es = explPose.bone(i).scale;
            assert(std::abs(ds.x - es.x) < 1e-6F);
        }
        // Additive still sums: rightArm = walk(-30) + item(-90)*0.5 = -75.
        assert(std::abs(rotX(defPose, model, "rightArm") - (-75.0F)) < 1e-3F);
        // Additive scale composes multiplicatively (walk 1.5, item none) -> 1.5.
        assert(std::abs(scaleX(defPose, model, "body") - 1.5F) < 1e-4F);
    }

    // --- Transition (crossfade) ----------------------------------------------
    {
        Transition t{0.0F, 1.0F, 1.0F}; // 0 -> 1 over 1 second, linear
        assert(std::abs(t.value() - 0.0F) < 1e-6F);
        const float mid = t.advance(0.25F);
        assert(std::abs(mid - 0.25F) < 1e-6F);      // mid-transition, in (0,1)
        assert(mid > 0.0F && mid < 1.0F);
        t.advance(0.25F);
        assert(std::abs(t.value() - 0.5F) < 1e-6F);
        assert(!t.finished());
        const float end = t.advance(10.0F);         // overshoot clamps
        assert(std::abs(end - 1.0F) < 1e-6F);
        assert(t.finished());
    }

    // Zero-duration snaps straight to the target (no ramp).
    {
        Transition snap{0.0F, 1.0F, 0.0F};
        assert(std::abs(snap.value() - 1.0F) < 1e-6F);
    }

    // Eased ramp stays within (0,1) mid-way and hits both endpoints exactly.
    {
        Transition e{0.0F, 1.0F, 1.0F, /*eased=*/true};
        assert(std::abs(e.value() - 0.0F) < 1e-6F);
        const float half = e.advance(0.5F);
        assert(std::abs(half - 0.5F) < 1e-6F);      // smoothstep(0.5) = 0.5
        const float quarter = Transition{0.0F, 1.0F, 1.0F, true}.value(); // t=0 -> 0
        assert(std::abs(quarter) < 1e-6F);
    }

    // Retarget mid-ramp restarts the clock from the current value.
    {
        Transition t{0.0F, 1.0F, 1.0F};
        t.advance(0.5F);                            // now 0.5, heading to 1
        t.retarget(0.0F, 1.0F);                     // reverse toward 0 from 0.5
        assert(std::abs(t.value() - 0.5F) < 1e-6F); // starts at current value
        assert(std::abs(t.target() - 0.0F) < 1e-6F);
        t.advance(0.5F);
        assert(std::abs(t.value() - 0.25F) < 1e-6F); // halfway from 0.5 -> 0
    }

    return 0;
}

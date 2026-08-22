#include "animation/AnimationClip.hpp"
#include "animation/AnimationController.hpp"
#include "animation/Animator.hpp"
#include "animation/BoneMask.hpp"
#include "animation/Molang.hpp"
#include "animation/SkeletalModel.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace mc::animation;

namespace {

constexpr const char* kGeometry = R"({
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

// Distinctive constant poses so a state is identifiable from a single bone.
constexpr const char* kAnimations = R"({
  "format_version":"1.8.0",
  "animations": {
    "animation.player.idle":  { "loop": true, "animation_length": 1.0, "bones": { "body": {"rotation":[1,0,0]} } },
    "animation.player.walk":  { "loop": true, "animation_length": 1.0, "bones": { "rightLeg": {"rotation":[30,0,0]} } },
    "animation.player.sneak": { "loop": true, "animation_length": 1.0, "bones": { "body": {"rotation":[28,0,0]} } },
    "animation.player.look":  { "loop": true, "animation_length": 1.0, "bones": { "head": {"rotation":[5,0,0]} } }
  }
})";

float boneRotX(const SkeletonPose& pose, const SkeletalModel& model, const char* bone) {
    return pose.bone(static_cast<std::size_t>(model.findBone(bone))).rotation.x;
}

} // namespace

int main() {
    const SkeletalModel model = SkeletalModel::parse(kGeometry);
    const AnimationLibrary library = AnimationLibrary::parse(kAnimations);
    const BoneGroups groups = buildBoneGroups(model);
    const MaskResolver resolveMask = [&](std::string_view name) -> const BoneMask* {
        if (name == "upper_body") return &groups.upperBody;
        if (name == "lower_body") return &groups.lowerBody;
        if (name == "head") return &groups.head;
        if (name == "arms") return &groups.arms;
        return nullptr;
    };

    // ---- Loader: schema parse -----------------------------------------------
    const AnimationControllerSet builtins = builtinPlayerControllers();
    const AnimationController* loco = builtins.find("controller.player.locomotion");
    assert(loco != nullptr);
    assert(loco->initialState() == "idle");
    assert(loco->stateCount() == 3U);
    const ControllerState* idle = loco->findState("idle");
    const ControllerState* walk = loco->findState("walk");
    assert(idle != nullptr && walk != nullptr);
    assert(idle->animations.size() == 1U); // idle clip (look is a consumer layer)
    assert(!idle->transitions.empty());
    assert(std::abs(idle->blendTransition - 0.15F) < 1e-6F);
    assert(std::abs(walk->blendTransition - 0.1F) < 1e-6F);

    // ---- State machine: idle -> walk when walk_amount crosses threshold ------
    {
        AnimationControllerInstance inst{*loco};
        assert(inst.currentState() == "idle");

        MolangContext ctx;
        ctx.setVariable("walk_amount", 0.0F);
        ctx.setVariable("sneaking", 0.0F);
        inst.update(0.016F, ctx); // below threshold: stays idle
        assert(inst.currentState() == "idle");

        ctx.setVariable("walk_amount", 0.8F); // crosses > 0.1
        inst.update(0.016F, ctx);
        assert(inst.currentState() == "walk");
        // A crossfade is now in flight (walk blend_transition = 0.1s).
        assert(inst.transitioning());
        assert(inst.previousState() == "idle");
        const float mid = inst.blendProgress();
        assert(mid > 0.0F && mid < 1.0F); // crossfading, not snapped

        // Settle the crossfade.
        inst.update(0.2F, ctx);
        assert(!inst.transitioning());
        assert(std::abs(inst.blendProgress() - 1.0F) < 1e-6F);
        assert(inst.currentState() == "walk");
    }

    // ---- sneaking drives the sneak state, then releasing returns to idle -----
    {
        AnimationControllerInstance inst{*loco};
        MolangContext ctx;
        ctx.setVariable("walk_amount", 0.0F);
        ctx.setVariable("sneaking", 1.0F);
        inst.update(0.016F, ctx);
        assert(inst.currentState() == "sneak");
        inst.update(0.2F, ctx); // settle
        ctx.setVariable("sneaking", 0.0F);
        inst.update(0.016F, ctx);
        assert(inst.currentState() == "idle");
    }

    // ---- apply(): the active state's clip actually poses the skeleton --------
    {
        AnimationControllerInstance inst{*loco};
        MolangContext ctx;
        ctx.setVariable("walk_amount", 1.0F);
        ctx.setQuery("anim_time", 0.0F);
        inst.update(1.0F, ctx); // go to walk and settle
        assert(inst.currentState() == "walk");

        Animator animator;
        animator.setModel(&model);
        animator.clearLayers();
        inst.apply(animator, library, resolveMask, ctx);
        const SkeletonPose pose = animator.evaluate();
        // Walk state poses rightLeg (30); idle body(1) must NOT contribute (we are
        // fully in walk), and head look is a consumer layer, not a controller
        // animation, so the controller alone leaves the head at rest.
        assert(std::abs(boneRotX(pose, model, "rightLeg") - 30.0F) < 1e-3F);
        assert(std::abs(boneRotX(pose, model, "head")) < 1e-3F);
        assert(std::abs(boneRotX(pose, model, "body")) < 1e-3F);
    }

    // ---- Data drives behavior: a different controller => different transition -
    // Same variables, but the data says walk triggers at a *different* threshold
    // and idle plays a different clip. Behavior must follow the data, not code.
    {
        const char* kCustom = R"({
          "animation_controllers": { "controller.custom": {
            "initial_state": "a",
            "states": {
              "a": { "animations": [{"animation.player.sneak": 1.0}],
                     "transitions": [{"b": "variable.go > 5"}], "blend_transition": 0.0 },
              "b": { "animations": [{"animation.player.walk": 1.0}], "transitions": [], "blend_transition": 0.0 }
            }
          }}
        })";
        const AnimationControllerSet custom = AnimationControllerSet::parse(kCustom);
        const AnimationController* c = custom.find("controller.custom");
        assert(c != nullptr && c->initialState() == "a");

        AnimationControllerInstance inst{*c};
        MolangContext ctx;
        ctx.setVariable("go", 3.0F); // below 5: stays in a
        inst.update(0.1F, ctx);
        assert(inst.currentState() == "a");
        ctx.setVariable("go", 9.0F); // above 5: moves to b (data-defined threshold)
        inst.update(0.1F, ctx);
        assert(inst.currentState() == "b");
        // blend_transition 0.0 snaps: no crossfade in flight.
        assert(!inst.transitioning());
    }

    // ---- Molang condition: a missing variable resolves to 0 (no transition) --
    {
        const char* kMissing = R"({
          "animation_controllers": { "controller.m": {
            "states": {
              "s0": { "transitions": [{"s1": "variable.never_set"}], "blend_transition": 0.0 },
              "s1": { "transitions": [] }
            }
          }}
        })";
        const AnimationControllerSet set = AnimationControllerSet::parse(kMissing);
        AnimationControllerInstance inst{*set.find("controller.m")};
        MolangContext ctx; // never_set is absent -> 0 -> falsy
        inst.update(0.1F, ctx);
        assert(inst.currentState() == "s0");
    }

    // ---- Bad-file fallback: malformed override reverts to built-in, no crash -
    {
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "anim_ctrl_test";
        std::filesystem::create_directories(dir);

        // (a) missing file -> built-in
        std::filesystem::remove(dir / "player.animation_controllers.json");
        AnimationControllerSet a = loadPlayerControllers(dir);
        assert(a.find("controller.player.locomotion") != nullptr);

        // (b) malformed JSON -> built-in (must not throw)
        {
            std::ofstream f(dir / "player.animation_controllers.json", std::ios::binary);
            f << "{ this is not valid json ]";
        }
        AnimationControllerSet b = loadPlayerControllers(dir);
        assert(b.find("controller.player.locomotion") != nullptr);
        assert(b.find("controller.player.locomotion")->stateCount() == 3U);

        // (c) valid override -> the override wins (proves the path is live)
        {
            std::ofstream f(dir / "player.animation_controllers.json", std::ios::binary);
            f << R"({"animation_controllers":{"controller.player.locomotion":{
                 "initial_state":"only",
                 "states":{"only":{"animations":[{"animation.player.idle":1.0}],"transitions":[]}}}}})";
        }
        AnimationControllerSet c = loadPlayerControllers(dir);
        const AnimationController* overridden = c.find("controller.player.locomotion");
        assert(overridden != nullptr);
        assert(overridden->initialState() == "only");
        assert(overridden->stateCount() == 1U);

        std::filesystem::remove_all(dir);
    }

    return 0;
}

#include "animation/AnimationClip.hpp"
#include "animation/Molang.hpp"

#include <cassert>
#include <cmath>

using namespace mc::animation;

int main() {
    const MolangContext ctx;

    // --- Programmatic keyframe API: eased (flat-tangent Bezier) ---
    AnimationChannel eased;
    eased.addEased(0.0F, {0.0F, 0.0F, 0.0F});
    eased.addEased(0.5F, {90.0F, 0.0F, 0.0F});
    // Midpoint of a symmetric ease is the value midpoint.
    assert(std::abs(eased.sample(0.25F, ctx, {}).x - 45.0F) < 1e-3F);
    // Ease-in: early in the segment it lags a linear ramp.
    const float earlyEase = eased.sample(0.05F, ctx, {}).x;      // t=0.1
    const float earlyLinear = 0.1F * 90.0F;                      // 9.0
    assert(earlyEase < earlyLinear);
    // Ease-out: late in the segment it leads a linear ramp (symmetry).
    const float lateEase = eased.sample(0.45F, ctx, {}).x;       // t=0.9
    assert(lateEase > 0.9F * 90.0F);
    // Endpoints are exact.
    assert(std::abs(eased.sample(0.0F, ctx, {}).x) < 1e-4F);
    assert(std::abs(eased.sample(0.5F, ctx, {}).x - 90.0F) < 1e-4F);

    // --- Explicit tangents produce an overshoot bump ---
    AnimationChannel bump;
    // Leave the first key heading up (+slope), arrive at the second heading
    // down (-slope): both endpoints are 0 yet the curve bulges above 0.
    bump.addBezier(0.0F, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, {400.0F, 0.0F, 0.0F});
    bump.addBezier(1.0F, {0.0F, 0.0F, 0.0F}, {-400.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F});
    assert(bump.sample(0.5F, ctx, {}).x > 5.0F);

    // --- Linear vs Bezier differ mid-segment ---
    AnimationChannel linear;
    linear.addLinear(0.0F, {0.0F, 0.0F, 0.0F});
    linear.addLinear(0.5F, {90.0F, 0.0F, 0.0F});
    assert(std::abs(linear.sample(0.25F, ctx, {}).x - 45.0F) < 1e-3F);
    assert(std::abs(linear.sample(0.05F, ctx, {}).x - 9.0F) < 1e-3F); // truly linear

    // --- JSON authoring: lerp_mode "bezier" ---
    const AnimationLibrary lib = AnimationLibrary::parse(R"({
      "animations": {
        "a": {
          "animation_length": 0.5,
          "bones": {
            "lid": {
              "rotation": {
                "0.0": [0, 0, 0],
                "0.5": {"post": [-90, 0, 0], "lerp_mode": "bezier"}
              }
            }
          }
        }
      }
    })");
    const AnimationClip* clip = lib.find("a");
    assert(clip != nullptr);
    const BoneAnimation* lid = clip->findBone("lid");
    assert(lid != nullptr);
    // Symmetric ease -> exactly half-open at the midpoint.
    assert(std::abs(lid->rotation.sample(0.25F, ctx, {}).x + 45.0F) < 1e-3F);
    // Ease-in: quarter of the way through (t=0.25) the lid has opened less than
    // the linear amount (0.25 * 90 = 22.5 degrees).
    assert(std::abs(lid->rotation.sample(0.125F, ctx, {}).x) < 22.5F);

    // --- Programmatic clip building ---
    AnimationClip built;
    built.setLength(1.0F);
    built.setLoop(true);
    built.bone("lid").rotation.addEased(0.0F, {0.0F, 0.0F, 0.0F});
    built.bone("lid").rotation.addEased(1.0F, {-90.0F, 0.0F, 0.0F});
    assert(built.loops());
    assert(std::abs(built.length() - 1.0F) < 1e-5F);
    assert(std::abs(built.findBone("lid")->rotation.sample(0.5F, ctx, {}).x + 45.0F) < 1e-3F);

    return 0;
}

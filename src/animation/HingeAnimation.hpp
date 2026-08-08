#pragma once

#include "animation/AnimationClip.hpp"
#include "animation/Molang.hpp"

namespace mc::animation {

// A one-degree-of-freedom hinge (a chest/door lid) driven by the data-driven
// keyframe system. The open curve is a Bezier segment whose tangents reproduce
// a cubic ease-out (fast open, gentle settle), so the motion matches the classic
// hand-tuned `1 - (1 - t)^3` easing exactly while now living in the animation
// library. Reusable for any lid-like part.
class HingeAnimation final {
  public:
    explicit HingeAnimation(float maxAngleRadians = 1.57079632679F);

    // Maps an open progress in [0, 1] to the hinge lift angle in radians.
    [[nodiscard]] float liftRadians(float openProgress) const;

  private:
    AnimationChannel channel_;
    MolangContext context_;
};

} // namespace mc::animation

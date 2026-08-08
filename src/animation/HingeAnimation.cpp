#include "animation/HingeAnimation.hpp"

#include <algorithm>

namespace mc::animation {

HingeAnimation::HingeAnimation(float maxAngleRadians) {
    // A cubic ease-out value(t) = A * (1 - (1 - t)^3) over t in [0, 1] has
    // derivative 3A at t=0 and 0 at t=1. A Hermite/Bezier segment with those
    // endpoint slopes reproduces it exactly, so the lid keeps its familiar feel.
    channel_.addBezier(0.0F, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F},
                       {3.0F * maxAngleRadians, 0.0F, 0.0F});
    channel_.addBezier(1.0F, {maxAngleRadians, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F},
                       {0.0F, 0.0F, 0.0F});
}

float HingeAnimation::liftRadians(float openProgress) const {
    const float clamped = std::clamp(openProgress, 0.0F, 1.0F);
    return channel_.sample(clamped, context_, glm::vec3{0.0F}).x;
}

} // namespace mc::animation

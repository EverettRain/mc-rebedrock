#include "animation/DisplayEntityAnimation.hpp"

namespace mc::animation {
namespace {
// Molang's math.sin takes degrees, so the radian argument is scaled by 180/pi.
constexpr const char* kBob =
    "math.sin((query.age * 0.1 + variable.phase) * 57.2957795) * 0.1 + 0.1";
constexpr const char* kYaw = "query.age * 0.05 + variable.phase";
} // namespace

DisplayEntityAnimation::DisplayEntityAnimation()
    : bob_(MolangExpression::compile(kBob)), yaw_(MolangExpression::compile(kYaw)) {}

DisplayEntityAnimation::Sample DisplayEntityAnimation::at(float ageTicks, float phase) const {
    MolangContext context;
    context.setQuery("age", ageTicks);
    context.setVariable("phase", phase);
    return {bob_.evaluate(context), yaw_.evaluate(context)};
}

} // namespace mc::animation

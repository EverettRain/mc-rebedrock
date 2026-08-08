#include "animation/DisplayEntityAnimation.hpp"
#include "animation/HingeAnimation.hpp"

#include <cassert>
#include <cmath>

using namespace mc::animation;

int main() {
    // --- Hinge reproduces the classic cubic ease-out 1 - (1 - t)^3 ---
    const float pi = 3.14159265358979323846F;
    const HingeAnimation hinge{pi * 0.5F};
    for (float open = 0.0F; open <= 1.0F; open += 0.05F) {
        const float expected = (1.0F - std::pow(1.0F - open, 3.0F)) * (pi * 0.5F);
        assert(std::abs(hinge.liftRadians(open) - expected) < 1e-4F);
    }
    // Fully closed / open endpoints.
    assert(std::abs(hinge.liftRadians(0.0F)) < 1e-5F);
    assert(std::abs(hinge.liftRadians(1.0F) - pi * 0.5F) < 1e-4F);
    // Clamps out-of-range input.
    assert(std::abs(hinge.liftRadians(2.0F) - pi * 0.5F) < 1e-4F);
    assert(std::abs(hinge.liftRadians(-1.0F)) < 1e-5F);

    // --- Display entity reproduces the legacy bob/spin formula ---
    const DisplayEntityAnimation display;
    for (float age = 0.0F; age < 40.0F; age += 3.3F) {
        const float phase = 1.7F;
        const float expectedBob = std::sin(age / 10.0F + phase) * 0.1F + 0.1F;
        const float expectedYaw = age * 0.05F + phase;
        const auto sample = display.at(age, phase);
        assert(std::abs(sample.bobHeight - expectedBob) < 1e-4F);
        assert(std::abs(sample.yawRadians - expectedYaw) < 1e-4F);
    }
    // Distinct phases desynchronise entities.
    assert(display.at(10.0F, 0.0F).yawRadians != display.at(10.0F, 1.0F).yawRadians);

    return 0;
}

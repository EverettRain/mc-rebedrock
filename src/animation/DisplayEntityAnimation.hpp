#pragma once

#include "animation/Molang.hpp"

namespace mc::animation {

// The idle "float and spin" motion of a dropped-item display entity, expressed
// as Molang so the curve is data, not hardcoded C++. `age` is measured in ticks
// and `phase` offsets each entity so a pile of drops does not move in lockstep.
class DisplayEntityAnimation final {
  public:
    DisplayEntityAnimation();

    struct Sample final {
        float bobHeight = 0.0F;  // vertical offset in blocks
        float yawRadians = 0.0F; // spin about the vertical axis
    };

    [[nodiscard]] Sample at(float ageTicks, float phase) const;

  private:
    MolangExpression bob_;
    MolangExpression yaw_;
};

} // namespace mc::animation

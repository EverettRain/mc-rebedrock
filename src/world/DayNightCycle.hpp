#pragma once

#include <glm/vec3.hpp>

namespace mc::world {

struct DayNightState final {
    float dayFraction = 0.25F;
    float skyBrightness = 1.0F;
    glm::vec3 sunDirection{0.0F, 1.0F, 0.0F};
    glm::vec3 horizonColor{0.68F, 0.78F, 0.90F};
};

class DayNightCycle final {
  public:
    static constexpr double kTicksPerSecond = 20.0;
    static constexpr double kTicksPerDay = 24'000.0;
    static constexpr double kSecondsPerDay = kTicksPerDay / kTicksPerSecond;
    static constexpr double kNewWorldTick = 6'000.0;

    [[nodiscard]] static double worldTick(double elapsedSeconds);
    [[nodiscard]] static DayNightState state(double elapsedSeconds);
    [[nodiscard]] static DayNightState stateAtTick(double tick);
};

} // namespace mc::world

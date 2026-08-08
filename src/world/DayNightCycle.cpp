#include "world/DayNightCycle.hpp"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>

namespace mc::world {
namespace {

constexpr float kPi = 3.14159265358979323846F;

[[nodiscard]] float smoothstep(float edge0, float edge1, float value) {
    const float amount = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return amount * amount * (3.0F - 2.0F * amount);
}

[[nodiscard]] glm::vec3 mix(
    const glm::vec3& first,
    const glm::vec3& second,
    float amount) {
    return first + (second - first) * amount;
}

} // namespace

double DayNightCycle::worldTick(double elapsedSeconds) {
    double tick = std::fmod(
        kNewWorldTick + std::max(elapsedSeconds, 0.0) * kTicksPerSecond,
        kTicksPerDay);
    if (tick < 0.0) tick += kTicksPerDay;
    return tick;
}

DayNightState DayNightCycle::state(double elapsedSeconds) {
    return stateAtTick(worldTick(elapsedSeconds));
}

DayNightState DayNightCycle::stateAtTick(double tick) {
    double wrappedTick = std::fmod(tick, kTicksPerDay);
    if (wrappedTick < 0.0) wrappedTick += kTicksPerDay;
    const float dayFraction = static_cast<float>(wrappedTick / kTicksPerDay);
    // Tick 0/12000 are the horizon crossings, 6000 is noon and 18000 midnight.
    const float orbit = (dayFraction - 0.25F) * 2.0F * kPi;
    const float elevation = std::cos(orbit);
    const glm::vec3 sunDirection = glm::normalize(glm::vec3{
        -std::sin(orbit), elevation, 0.28F * std::cos(orbit)});

    constexpr float minimumBrightness = 0.05F;
    const float daylight = smoothstep(-0.20F, 0.25F, elevation);
    const float skyBrightness = minimumBrightness +
        (1.0F - minimumBrightness) * daylight;
    const glm::vec3 nightHorizon{0.012F, 0.020F, 0.055F};
    const glm::vec3 dayHorizon{0.68F, 0.78F, 0.90F};
    glm::vec3 horizon = mix(nightHorizon, dayHorizon, daylight);
    const float twilight = (1.0F - smoothstep(0.0F, 0.32F, std::abs(elevation))) *
        smoothstep(-0.22F, -0.02F, elevation);
    horizon = mix(horizon, {0.86F, 0.39F, 0.18F}, twilight * 0.55F);

    return {dayFraction, skyBrightness, sunDirection, horizon};
}

} // namespace mc::world

#pragma once

#include "world/World.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace mc::gameplay {

// The resolved environment for one tick.
//
// 26.1 keeps this in `world/attribute/EnvironmentAttributeSystem`: a map of
// named attributes, each built from a stack of layers (constant, time-based,
// positional), read through `getValue(attribute, pos, interpolator)`. That
// shape is the right *concept* — one named source per environmental fact,
// instead of every system deriving its own — but it is the one place in the
// 26.1 architecture that cannot be copied literally. A map lookup, a layer
// walk and a boxed `Value` per read would land on the per-block, per-tick
// growth and spawn paths, where the switch it replaces was a field read.
//
// So the layers are resolved once per tick into this POD, and everything hot
// reads a field. Vanilla leans on exactly the same trick from the other side:
// `invalidateTickCache` means its layer walk also runs about once per tick.
// The cache is the real hot path there too.
struct EnvironmentSnapshot final {
    // EnvironmentAttributes::SKY_LIGHT_LEVEL, the dimension-wide value: 15 in
    // full day, 4 at night, blended down by rain and thunder.
    float skyLightLevel = 15.0F;
    // Level::skyDarken, `(int)(15 - SKY_LIGHT_LEVEL)` (Level.java:738). This is
    // what every gameplay light check subtracts from stored sky light, and the
    // reason grass stops spreading at night without any system asking the clock
    // what time it is.
    int ambientDarkness = 0;
    // Level::isRaining / isThundering, off the smoothed gradients.
    bool raining = false;
    bool thundering = false;

    [[nodiscard]] static EnvironmentSnapshot resolve(
        double dayTimeTicks,
        float rainGradient,
        float thunderGradient);
};

namespace environment {

// Timelines::NIGHT_SKY_LIGHT_LEVEL / DAY_SKY_LIGHT_LEVEL.
inline constexpr float kDaySkyLightLevel = 15.0F;
inline constexpr float kNightSkyLightLevel = 4.0F;
inline constexpr int kTicksPerDay = 24000;

// The SKY_LIGHT_LEVEL multiplier track of Timelines::OVERWORLD_DAY
// (`world/timeline/Timelines.java:78-82`): a linear keyframe track, no easing,
// multiplying the 15.0 base. 15 * 0.26666668 == 4.0, the night level.
struct SkyLightKeyframe final {
    int tick;
    float multiplier;
};
inline constexpr std::array<SkyLightKeyframe, 4> kSkyLightTrack{{
    {133, 1.0F},
    {11867, 1.0F},
    {13670, 0.26666668F},
    {22330, 0.26666668F},
}};

// WeatherAttributes::RAIN / THUNDER blend SKY_LIGHT_LEVEL toward 4.0 with a
// fixed alpha (`world/attribute/WeatherAttributes.java:17, 28`).
inline constexpr float kRainAlpha = 0.3125F;
inline constexpr float kThunderAlpha = 0.52734375F;

[[nodiscard]] inline float lerp(float from, float to, float amount) {
    return from + (to - from) * amount;
}

// The day track sampled at a wrapped tick. The last keyframe wraps around to
// the first across midnight-to-dawn, which is where the curve climbs back to
// full daylight.
[[nodiscard]] inline float skyLightMultiplier(double dayTimeTicks) {
    double tick = std::fmod(dayTimeTicks, static_cast<double>(kTicksPerDay));
    if (tick < 0.0) {
        tick += static_cast<double>(kTicksPerDay);
    }
    const auto& first = kSkyLightTrack.front();
    const auto& last = kSkyLightTrack.back();
    if (tick < static_cast<double>(first.tick) || tick >= static_cast<double>(last.tick)) {
        // The wrap segment: last keyframe -> first keyframe one day later.
        const double start = static_cast<double>(last.tick);
        const double end = static_cast<double>(first.tick + kTicksPerDay);
        const double position = tick < static_cast<double>(first.tick)
                                    ? tick + static_cast<double>(kTicksPerDay)
                                    : tick;
        const auto amount = static_cast<float>((position - start) / (end - start));
        return lerp(last.multiplier, first.multiplier, amount);
    }
    for (std::size_t index = 1U; index < kSkyLightTrack.size(); ++index) {
        const auto& previous = kSkyLightTrack[index - 1U];
        const auto& next = kSkyLightTrack[index];
        if (tick < static_cast<double>(next.tick)) {
            const auto amount = static_cast<float>(
                (tick - static_cast<double>(previous.tick)) /
                static_cast<double>(next.tick - previous.tick));
            return lerp(previous.multiplier, next.multiplier, amount);
        }
    }
    return last.multiplier;
}

// LevelLightEngine#getRawBrightness: the brighter of the block channel and the
// sky channel after the day's darkening is taken off it. Vanilla does not clamp
// the sky term — the block channel is never negative, so the max cannot be.
[[nodiscard]] inline int rawBrightness(
    const world::World& world,
    int x,
    int y,
    int z,
    int skyDarkening) {
    const int sky = static_cast<int>(world.skyLight(x, y, z)) - skyDarkening;
    return std::max(static_cast<int>(world.blockLight(x, y, z)), sky);
}

// LevelReader#getRawBrightness(pos, 0): the light a block sees regardless of
// the time of day. Crops grow by this one — a wheat field does not stall at
// dusk (`CropBlock.randomTick` -> `getRawBrightness(pos, 0)`).
[[nodiscard]] inline int rawBrightness(const world::World& world, int x, int y, int z) {
    return rawBrightness(world, x, y, z, 0);
}

// LevelReader#getMaxLocalRawBrightness(pos): the same reading with the current
// ambient darkness applied. Spreading and sapling growth use this one, which is
// why they stop at night.
[[nodiscard]] inline int maxLocalRawBrightness(
    const world::World& world,
    int x,
    int y,
    int z,
    const EnvironmentSnapshot& environment) {
    return rawBrightness(world, x, y, z, environment.ambientDarkness);
}

// DimensionType#method_28515: the light-level -> brightness curve. For a light
// level L in [0, 15], `g = L/15`, `h = g / (4 - 3g)`, and the dimension lerps
// from its ambientLight floor toward h. The overworld's ambientLight is 0, so
// the curve is exactly h — the same value LivingEntity#isInDaylight reads as
// `getBrightnessAtEyes()`. Kept float-exact to Java's arithmetic so the
// `f > 0.5` band and the `(f - 0.4) * 2` roll below cross at the same light
// levels vanilla does (L == 12 is the first level with f > 0.5).
[[nodiscard]] inline float lightLevelToBrightness(int lightLevel) {
    const float g = static_cast<float>(lightLevel) / 15.0F;
    return g / (4.0F - 3.0F * g);
}

// LivingEntity#getBrightnessAtEyes -> WorldView#getBrightness(pos): the
// dimension brightness curve applied to the cell's max-local light with the
// tick's ambient darkness taken off, so rain and thunder (which raise
// ambientDarkness) fold straight into the value without a separate weather
// branch — exactly how vanilla suppresses daylight burning in the rain.
[[nodiscard]] inline float eyeBrightness(
    const world::World& world,
    int x,
    int y,
    int z,
    const EnvironmentSnapshot& environment) {
    return lightLevelToBrightness(maxLocalRawBrightness(world, x, y, z, environment));
}

// BlockAndLightGetter#canSeeSky: `getBrightness(SKY, pos) >= 15`. The stored
// sky light is static full sun, so a cell that still reads 15 is one nothing
// stands between and the sky.
[[nodiscard]] inline bool canSeeSky(const world::World& world, int x, int y, int z) {
    return world.skyLight(x, y, z) >= 15U;
}

} // namespace environment

inline EnvironmentSnapshot EnvironmentSnapshot::resolve(
    double dayTimeTicks,
    float rainGradient,
    float thunderGradient) {
    EnvironmentSnapshot snapshot{};
    float level = environment::kDaySkyLightLevel * environment::skyLightMultiplier(dayTimeTicks);

    // WeatherAttributes::addLayer: the rain layer covers only the part of the
    // rain gradient the thunder gradient has not already claimed, then the
    // thunder layer blends on top. Each layer lerps between the current value
    // and its own alpha-blended version by that gradient.
    const float thunder = std::clamp(thunderGradient, 0.0F, 1.0F);
    const float rain = std::clamp(rainGradient, 0.0F, 1.0F) - thunder;
    if (rain > 0.0F) {
        const float rained =
            environment::lerp(level, environment::kNightSkyLightLevel, environment::kRainAlpha);
        level = environment::lerp(level, rained, rain);
    }
    if (thunder > 0.0F) {
        const float thundered =
            environment::lerp(level, environment::kNightSkyLightLevel, environment::kThunderAlpha);
        level = environment::lerp(level, thundered, thunder);
    }

    snapshot.skyLightLevel = level;
    // Level#updateSkyBrightness truncates, so a partly darkened sky only costs a
    // whole light level once the float crosses it.
    snapshot.ambientDarkness = static_cast<int>(environment::kDaySkyLightLevel - level);
    // Level#isRaining / #isThundering read the gradients, not the flags.
    snapshot.raining = rainGradient > 0.2F;
    snapshot.thundering = thunderGradient > 0.9F;
    return snapshot;
}

// Level#isRainingAt: rain reaches a cell only where it is actually raining and
// the sky is open above it. Vanilla also rejects positions under the
// motion-blocking heightmap and biomes that get snow instead; the heightmap
// test is what canSeeSky stands in for here, and every biome in the game so far
// takes rain.
[[nodiscard]] inline bool isRainingAt(
    const world::World& world,
    int x,
    int y,
    int z,
    const EnvironmentSnapshot& environment) {
    return environment.raining && environment::canSeeSky(world, x, y, z);
}

} // namespace mc::gameplay

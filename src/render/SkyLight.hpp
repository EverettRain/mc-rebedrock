#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace mc::render {

// 26.1 drives the world's sky lighting from two SEPARATE day-cycle tracks, and
// conflating them is a visible bug in both directions. `Timelines.OVERWORLD_DAY`
// declares them as keyframe tracks over the 24000-tick day:
//
//   SKY_LIGHT_LEVEL  (base 15) x 1.0 @133 .. @11867, x 0.2666667 @13670 .. @22330
//   SKY_LIGHT_FACTOR (base 1)  x 1.0 @730 .. @11270, x 0.24      @13140 .. @22860
//
// SKY_LIGHT_LEVEL feeds the integer `skyDarken` that gameplay and the vignette
// read: `Level.updateSkyBrightness` is `(int)(15 - SKY_LIGHT_LEVEL)`, and
// `LevelLightEngine.getRawBrightness` is `max(blockLight, skyLight - skyDarken)`.
// It is 0 for the whole day and 11 at night, and it steps, because it is an int.
//
// SKY_LIGHT_FACTOR scales the sky half of the lightmap the terrain samples. It
// is a float, its ramps are at different ticks, and it bottoms out at 0.24, not
// at 4/15.
//
// Neither is the smooth `DayNightState::skyBrightness` curve, which is a
// sun-elevation cosine this renderer uses for sky/fog colour. Using that for the
// vignette is what made the corners crush in broad daylight: it is fractional
// most of the day, so `1 - brightness(sky * thatFactor)` stayed well above zero
// where vanilla is flat 0.
//
// These belong to BM-3's EnvironmentAttribute layer once it carries the visual
// tracks — this header is the render-side transcription until it does, and the
// keyframes are the numbers to move, not to re-derive.
class SkyLight final {
  public:
    static constexpr double kTicksPerDay = 24'000.0;

    // The multiplier the SKY_LIGHT_LEVEL track applies to the base 15.
    [[nodiscard]] static float skyLightLevelMultiplier(double dayTick) {
        static constexpr std::array<Keyframe, 4> kTrack{{
            {133.0, 1.0F}, {11867.0, 1.0F}, {13670.0, 0.26666668F}, {22330.0, 0.26666668F},
        }};
        return sample(kTrack, dayTick);
    }

    // `Level.updateSkyBrightness`: (int)(15 - SKY_LIGHT_LEVEL). 0 all day, 11 at
    // night. Integer on purpose — vanilla's sky light is an integer level and
    // the darkening is subtracted from it, not multiplied into it.
    [[nodiscard]] static int skyDarken(double dayTick) {
        const float level = 15.0F * skyLightLevelMultiplier(dayTick);
        return static_cast<int>(15.0F - level);
    }

    // The multiplier the SKY_LIGHT_FACTOR track applies to the base 1.0 — the
    // lightmap's `SkyFactor`.
    [[nodiscard]] static float skyLightFactor(double dayTick) {
        static constexpr std::array<Keyframe, 4> kTrack{{
            {730.0, 1.0F}, {11270.0, 1.0F}, {13140.0, 0.24F}, {22860.0, 0.24F},
        }};
        return sample(kTrack, dayTick);
    }

  private:
    struct Keyframe final {
        double tick = 0.0;
        float value = 0.0F;
    };

    // Linear between keyframes and wrapping across midnight. Vanilla's Timeline
    // eases a track only where one is declared (SUN_ANGLE sets a cubic bezier);
    // these light tracks declare none, so they interpolate straight.
    [[nodiscard]] static float sample(const std::array<Keyframe, 4>& track, double dayTick) {
        double tick = std::fmod(dayTick, kTicksPerDay);
        if (tick < 0.0) {
            tick += kTicksPerDay;
        }
        for (std::size_t index = 0; index + 1 < track.size(); ++index) {
            const auto& from = track[index];
            const auto& to = track[index + 1];
            if (tick >= from.tick && tick <= to.tick) {
                return lerp(from, to, tick);
            }
        }
        // The wrap segment: the last keyframe round to the first one, through
        // tick 0. Every tick outside the declared span lands here.
        const auto& last = track.back();
        const auto& first = track.front();
        const double span = (kTicksPerDay - last.tick) + first.tick;
        const double travelled = tick > last.tick ? tick - last.tick
                                                  : (kTicksPerDay - last.tick) + tick;
        const auto amount = static_cast<float>(span > 0.0 ? travelled / span : 0.0);
        return last.value + (first.value - last.value) * std::clamp(amount, 0.0F, 1.0F);
    }

    [[nodiscard]] static float lerp(const Keyframe& from, const Keyframe& to, double tick) {
        if (to.tick <= from.tick) {
            return to.value;
        }
        const auto amount = static_cast<float>((tick - from.tick) / (to.tick - from.tick));
        return from.value + (to.value - from.value) * std::clamp(amount, 0.0F, 1.0F);
    }
};

} // namespace mc::render

// 26.1 runs the sky on two separate day tracks, and this pins both.
//
// Timelines.OVERWORLD_DAY declares them over the 24000-tick day:
//   SKY_LIGHT_LEVEL  x1.0 @133..@11867, x0.2666667 @13670..@22330
//   SKY_LIGHT_FACTOR x1.0 @730..@11270, x0.24      @13140..@22860
//
// SKY_LIGHT_LEVEL becomes the INTEGER `skyDarken` that `Level.updateSkyBrightness`
// computes as (int)(15 - level), and `LevelLightEngine.getRawBrightness`
// SUBTRACTS from the sky light level: max(blockLight, skyLight - skyDarken).
// SKY_LIGHT_FACTOR is a float that SCALES the sky half of the lightmap.
//
// The defect these exist to prevent: reading one smooth sun-elevation curve for
// both. That curve is fractional through most of the day, so the vignette —
// which is 1 - brightness(rawBrightness) — never reached zero outdoors and the
// screen corners stayed crushed in broad daylight.

#include "render/SkyLight.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message, int line) {
    if (!condition) {
        throw std::runtime_error{"sky_light_test line " + std::to_string(line) + ": " + message};
    }
}

#define REQUIRE(condition, message) require(condition, message, __LINE__)

using mc::render::SkyLight;

[[nodiscard]] bool near(float value, float expected, float tolerance = 1.0e-4F) {
    return std::abs(value - expected) <= tolerance;
}

// The whole point: outdoors in daylight the darkening is exactly 0, so a player
// standing in the open reads light level 15 and the vignette switches off.
void checkSkyDarkenIsZeroAllDay() {
    for (double tick = 133.0; tick <= 11867.0; tick += 37.0) {
        REQUIRE(SkyLight::skyDarken(tick) == 0,
                "skyDarken must be 0 for the whole day span; tick " + std::to_string(tick) +
                    " gave " + std::to_string(SkyLight::skyDarken(tick)) +
                    ". A non-zero daytime darkening is what crushes the vignette outdoors.");
    }
}

// Night bottoms out at sky light level 4 (Timelines.NIGHT_SKY_LIGHT_LEVEL), so
// the darkening is 15 - 4 = 11.
void checkSkyDarkenAtNight() {
    for (double tick = 13670.0; tick <= 22330.0; tick += 41.0) {
        const int darken = SkyLight::skyDarken(tick);
        REQUIRE(darken == 11, "skyDarken must be 11 through the night; tick " +
                                  std::to_string(tick) + " gave " + std::to_string(darken));
    }
    REQUIRE(near(SkyLight::skyLightLevelMultiplier(18000.0), 0.26666668F),
            "midnight must sit on the night keyframe 4/15");
}

// It has to move between the two, and monotonically — a track that jumps would
// pop the vignette on at dusk instead of fading it.
void checkSkyDarkenRampsAtDusk() {
    int previous = SkyLight::skyDarken(11867.0);
    REQUIRE(previous == 0, "the day span must end at 0");
    bool sawIntermediate = false;
    for (double tick = 11867.0; tick <= 13670.0; tick += 1.0) {
        const int darken = SkyLight::skyDarken(tick);
        REQUIRE(darken >= previous,
                "the dusk ramp must be monotonic; it fell at tick " + std::to_string(tick));
        if (darken > 0 && darken < 11) {
            sawIntermediate = true;
        }
        previous = darken;
    }
    REQUIRE(previous == 11, "the dusk ramp must arrive at 11");
    REQUIRE(sawIntermediate, "the dusk ramp must pass through intermediate levels, not jump");
}

// The lightmap's SkyFactor: 1.0 by day, 0.24 at night, on its own keyframes —
// note they are NOT the SKY_LIGHT_LEVEL ticks, which is exactly why one shared
// curve cannot serve both.
void checkSkyLightFactor() {
    for (double tick = 730.0; tick <= 11270.0; tick += 53.0) {
        REQUIRE(near(SkyLight::skyLightFactor(tick), 1.0F),
                "skyLightFactor must be 1.0 through the day; tick " + std::to_string(tick) +
                    " gave " + std::to_string(SkyLight::skyLightFactor(tick)));
    }
    for (double tick = 13140.0; tick <= 22860.0; tick += 53.0) {
        REQUIRE(near(SkyLight::skyLightFactor(tick), 0.24F),
                "skyLightFactor must be 0.24 through the night (Timelines"
                ".NIGHT_SKY_LIGHT_FACTOR); tick " +
                    std::to_string(tick) + " gave " +
                    std::to_string(SkyLight::skyLightFactor(tick)));
    }
    // The two tracks have different keyframes: at tick 12000 the level track has
    // begun ramping (its day span ended at 11867) while the factor track is
    // already past its own day span too — but they are not equal, and asserting
    // that keeps anyone from collapsing them into one curve.
    REQUIRE(!near(SkyLight::skyLightFactor(13000.0),
                  SkyLight::skyLightLevelMultiplier(13000.0), 0.02F),
            "SKY_LIGHT_FACTOR and SKY_LIGHT_LEVEL are separate tracks with separate keyframes; "
            "if they agree everywhere, one has been made to follow the other");
}

// The day is a loop: the tracks have to be defined outside their declared spans
// and rejoin across midnight without a discontinuity.
void checkWrapsAcrossMidnight() {
    REQUIRE(near(SkyLight::skyLightFactor(0.0), SkyLight::skyLightFactor(24000.0)),
            "tick 0 and tick 24000 are the same instant");
    REQUIRE(SkyLight::skyDarken(0.0) == SkyLight::skyDarken(24000.0),
            "tick 0 and tick 24000 are the same instant");
    // Just after the night span ends the darkening must be heading back to 0,
    // and it must actually get there before the day span begins.
    REQUIRE(SkyLight::skyDarken(22330.0) == 11, "the night span must end at 11");
    REQUIRE(SkyLight::skyDarken(133.0) == 0, "the day span must begin at 0");
    // Negative and multi-day ticks resolve, so a long-running world does not
    // fall off the end of the track.
    REQUIRE(SkyLight::skyDarken(-1000.0) == SkyLight::skyDarken(23000.0),
            "negative ticks must wrap");
    REQUIRE(SkyLight::skyDarken(6000.0 + 24000.0 * 7.0) == 0, "multi-day ticks must wrap");
}

} // namespace

int main() {
    checkSkyDarkenIsZeroAllDay();
    checkSkyDarkenAtNight();
    checkSkyDarkenRampsAtDusk();
    checkSkyLightFactor();
    checkWrapsAcrossMidnight();
    return 0;
}

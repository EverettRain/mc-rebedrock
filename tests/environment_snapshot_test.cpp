#include "gameplay/EnvironmentSnapshot.hpp"

#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <cassert>
#include <cmath>

namespace {

using mc::gameplay::EnvironmentSnapshot;
namespace environment = mc::gameplay::environment;

// 1.16.1's closed form for the same quantity, kept here as an independent
// oracle: Level#updateSkyBrightness computed
//   d = 1 - rainLevel * 5/16
//   e = 1 - thunderLevel * 5/16
//   f = 0.5 + 2 * clamp(cos(celestialAngle * 2pi), -0.25, 0.25)
//   skyDarken = (int)((1 - f * d * e) * 11)
// The celestial angle is 0 at noon and 0.5 at midnight — Dimension#getSkyAngle
// shifts the raw time of day by a quarter turn.
// 26.1 expresses it as a keyframe track plus two alpha-blended weather layers.
// The two agree exactly on the plateaus and on every weather combination; they
// differ only in the shape of the dawn/dusk ramp (a clamped cosine there, a
// straight line here), which is why this oracle is applied at the plateaus.
[[nodiscard]] int legacySkyDarken(double celestialAngle, float rainLevel, float thunderLevel) {
    const double angle = celestialAngle * 2.0 * 3.14159265358979323846;
    const double cosine = std::cos(angle);
    const double clamped = cosine < -0.25 ? -0.25 : (cosine > 0.25 ? 0.25 : cosine);
    const double factor = 0.5 + 2.0 * clamped;
    const double rain = 1.0 - static_cast<double>(rainLevel) * 5.0 / 16.0;
    const double thunder = 1.0 - static_cast<double>(thunderLevel) * 5.0 / 16.0;
    return static_cast<int>((1.0 - factor * rain * thunder) * 11.0);
}

[[nodiscard]] bool nearly(float value, float expected) {
    return std::fabs(value - expected) < 0.0005F;
}

void testSkyLightTrack() {
    // The plateaus of Timelines::OVERWORLD_DAY's SKY_LIGHT_LEVEL track. Noon is
    // full daylight; midnight sits at 15 * 0.26666668 == NIGHT_SKY_LIGHT_LEVEL.
    assert(nearly(environment::skyLightMultiplier(6000.0), 1.0F));
    assert(nearly(environment::skyLightMultiplier(133.0), 1.0F));
    assert(nearly(environment::skyLightMultiplier(11867.0), 1.0F));
    assert(nearly(environment::skyLightMultiplier(13670.0), 0.26666668F));
    assert(nearly(environment::skyLightMultiplier(18000.0), 0.26666668F));
    assert(nearly(environment::skyLightMultiplier(22330.0), 0.26666668F));

    // Dusk ramps down and dawn ramps back up, both strictly monotone. A track
    // that failed to wrap past 24000 would read as a jump here.
    for (double tick = 11867.0; tick < 13670.0; tick += 100.0) {
        assert(environment::skyLightMultiplier(tick) >=
               environment::skyLightMultiplier(tick + 100.0));
    }
    for (double tick = 22330.0; tick < 24133.0; tick += 100.0) {
        assert(environment::skyLightMultiplier(tick) <=
               environment::skyLightMultiplier(tick + 100.0));
    }
    // Wrapping is by day length, so the same phase resolves the same however
    // many days have passed — the clock counts total ticks, not time of day.
    assert(nearly(environment::skyLightMultiplier(18000.0),
                  environment::skyLightMultiplier(18000.0 + 24000.0 * 7.0)));
}

void testAmbientDarkness() {
    const auto noon = EnvironmentSnapshot::resolve(6000.0, 0.0F, 0.0F);
    const auto midnight = EnvironmentSnapshot::resolve(18000.0, 0.0F, 0.0F);
    assert(nearly(noon.skyLightLevel, 15.0F));
    assert(noon.ambientDarkness == 0);
    assert(nearly(midnight.skyLightLevel, 4.0F));
    assert(midnight.ambientDarkness == 11);
    // Both agree with the 1.16.1 closed form on the plateaus.
    assert(noon.ambientDarkness == legacySkyDarken(0.0, 0.0F, 0.0F));
    assert(midnight.ambientDarkness == legacySkyDarken(0.5, 0.0F, 0.0F));

    // Open ground at midnight reads sky 15 - 11 == 4, which is exactly the
    // threshold grass survives on and one short of the 9 it spreads on. The two
    // vanilla constants only line up because the darkening is subtracted rather
    // than scaled, so this pins the whole reason C1 exists.
    assert(15 - midnight.ambientDarkness == 4);

    // The darkening only ever costs whole levels, and never more than 11.
    for (double tick = 0.0; tick < 24000.0; tick += 37.0) {
        const auto snapshot = EnvironmentSnapshot::resolve(tick, 0.0F, 0.0F);
        assert(snapshot.ambientDarkness >= 0);
        assert(snapshot.ambientDarkness <= 11);
    }
}

void testWeatherLayers() {
    // Rain and thunder blend SKY_LIGHT_LEVEL toward 4.0 with their own alphas.
    // Both land on the same integer the 1.16.1 product form produced.
    const auto rainyNoon = EnvironmentSnapshot::resolve(6000.0, 1.0F, 0.0F);
    assert(rainyNoon.ambientDarkness == 3);
    assert(rainyNoon.ambientDarkness == legacySkyDarken(0.0, 1.0F, 0.0F));

    const auto stormyNoon = EnvironmentSnapshot::resolve(6000.0, 1.0F, 1.0F);
    assert(stormyNoon.ambientDarkness == 5);
    assert(stormyNoon.ambientDarkness == legacySkyDarken(0.0, 1.0F, 1.0F));

    // WeatherAttributes::addLayer gives the rain layer only the part of the rain
    // gradient thunder has not claimed. Passing the full gradient to both layers
    // instead would over-darken this case.
    const auto halfStorm = EnvironmentSnapshot::resolve(6000.0, 1.0F, 0.5F);
    assert(halfStorm.ambientDarkness < stormyNoon.ambientDarkness);
    assert(halfStorm.ambientDarkness > rainyNoon.ambientDarkness);

    // Night is already at the 4.0 the weather blends toward, so a storm cannot
    // make it darker.
    const auto stormyMidnight = EnvironmentSnapshot::resolve(18000.0, 1.0F, 1.0F);
    assert(stormyMidnight.ambientDarkness == 11);

    // Level#isRaining / #isThundering read the gradients, not the flags.
    assert(!EnvironmentSnapshot::resolve(6000.0, 0.2F, 0.0F).raining);
    assert(EnvironmentSnapshot::resolve(6000.0, 0.21F, 0.0F).raining);
    assert(!EnvironmentSnapshot::resolve(6000.0, 1.0F, 0.9F).thundering);
    assert(EnvironmentSnapshot::resolve(6000.0, 1.0F, 0.91F).thundering);
}

[[nodiscard]] mc::world::World makeLitWorld() {
    mc::world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    mc::world::World world;
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

void testLightReadings() {
    auto world = makeLitWorld();
    world.setSkyLight(4, 1, 4, 15U);
    world.setBlockLight(4, 1, 4, 0U);
    // A torch-lit cell that the sky cannot reach.
    world.setSkyLight(6, 1, 6, 0U);
    world.setBlockLight(6, 1, 6, 14U);

    const auto noon = EnvironmentSnapshot::resolve(6000.0, 0.0F, 0.0F);
    const auto midnight = EnvironmentSnapshot::resolve(18000.0, 0.0F, 0.0F);

    // getRawBrightness(pos, 0) ignores the time of day entirely, which is why a
    // wheat field keeps growing overnight.
    assert(environment::rawBrightness(world, 4, 1, 4) == 15);
    assert(environment::rawBrightness(world, 4, 1, 4) ==
           environment::rawBrightness(world, 4, 1, 4, 0));

    // getMaxLocalRawBrightness subtracts the darkening from the sky channel.
    assert(environment::maxLocalRawBrightness(world, 4, 1, 4, noon) == 15);
    assert(environment::maxLocalRawBrightness(world, 4, 1, 4, midnight) == 4);

    // The block channel is untouched by it: a torch keeps its cell lit at
    // midnight. Scaling the reading instead of subtracting from the sky channel
    // would dim this cell too.
    assert(environment::maxLocalRawBrightness(world, 6, 1, 6, midnight) == 14);

    // canSeeSky is "the sky channel still reads full", the definition
    // BlockAndLightGetter uses.
    assert(environment::canSeeSky(world, 4, 1, 4));
    assert(!environment::canSeeSky(world, 6, 1, 6));

    // Rain reaches the open cell and not the sheltered one; a clear sky reaches
    // neither.
    const auto rainyNoon = EnvironmentSnapshot::resolve(6000.0, 1.0F, 0.0F);
    assert(mc::gameplay::isRainingAt(world, 4, 1, 4, rainyNoon));
    assert(!mc::gameplay::isRainingAt(world, 6, 1, 6, rainyNoon));
    assert(!mc::gameplay::isRainingAt(world, 4, 1, 4, noon));
}

} // namespace

int main() {
    testSkyLightTrack();
    testAmbientDarkness();
    testWeatherLayers();
    testLightReadings();
    return 0;
}

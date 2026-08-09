#include "gameplay/WeatherSystem.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

// Exercises the weather system as a headless unit against the exact 1.16.1
// semantics the port mirrors: WeatherCommand's setWeather calls, the
// ServerWorld auto-cycle (spell expiry flips the flags), and the gradient
// smoothing that isRaining() thresholds at 0.2.

namespace {

// Nearly-equal float comparison for the gradient assertions.
bool nearlyEqual(float left, float right) {
    return std::abs(left - right) < 0.0005F;
}

} // namespace

int main() {
    using namespace mc::gameplay;

    // --- A fresh system is clear, with no spell pending. ---
    {
        WeatherSystem weather;
        assert(!weather.isRaining());
        assert(!weather.isThundering());
        assert(weather.state().clearWeatherTime == 0);
        assert(weather.state().rainTime == 0);
        assert(nearlyEqual(weather.rainGradient(), 0.0F));
        assert(nearlyEqual(weather.visualSkyLightFactorAt(1.0F), 1.0F));
    }

    // --- /weather clear N: a forced clear spell that expires into the auto-cycle. ---
    // setWeather(5, 0, false, false) is WeatherCommand's clear with a 5-tick
    // spell. The clear branch idles the rain/thunder timers at 1, so the tick
    // after the spell expires flips both flags on — the exact 1.16.1 behaviour.
    {
        WeatherSystem weather;
        weather.setWeather(5, 0, false, false);
        assert(weather.state().clearWeatherTime == 5);
        assert(!weather.state().raining);
        for (int tick = 0; tick < 5; ++tick) {
            weather.tick(true);
        }
        assert(weather.state().clearWeatherTime == 0);
        assert(!weather.state().raining);
        // The spell has ended: the auto-cycle resumes and the idled timers hit
        // zero in one tick, so rain (and thunder) start.
        weather.tick(true);
        assert(weather.state().raining);
        assert(weather.state().thundering);
    }

    // --- /weather rain N: a rain spell that expires back to a clear interlude. ---
    // setWeather(0, 5, true, false) is WeatherCommand's rain. thunderTime is
    // set to the rain duration (ServerWorld#setWeather), so the rain spell and
    // the idled thunder timer expire together — the spell ends as thunder
    // starts, exactly like vanilla.
    {
        WeatherSystem weather;
        weather.setWeather(0, 5, true, false);
        assert(weather.state().raining);
        assert(weather.state().rainTime == 5);
        assert(!weather.isRaining());  // gradient still 0, needs 20 ticks to cross 0.2
        for (int tick = 0; tick < 5; ++tick) {
            weather.tick(true);
        }
        assert(!weather.state().raining);
        assert(weather.state().thundering);
        // The flags flipped this tick; the next tick rolls a long clear
        // interlude (12000..180000 ticks), so it stays non-raining for a while.
        weather.tick(true);
        assert(weather.state().rainTime >= 12'000);
        assert(weather.state().rainTime < 180'000);
    }

    // --- The rain gradient ramps 0.01/tick and crosses the 0.2 threshold. ---
    {
        WeatherSystem weather;
        weather.setWeather(0, 1000, true, false);
        for (int tick = 0; tick < 10; ++tick) {
            weather.tick(true);
        }
        assert(nearlyEqual(weather.rainGradient(), 0.10F));
        assert(!weather.isRaining());  // 0.10 < 0.2
        for (int tick = 0; tick < 11; ++tick) {
            weather.tick(true);
        }
        assert(nearlyEqual(weather.rainGradient(), 0.21F));
        assert(weather.isRaining());  // crossed 0.2
        for (int tick = 0; tick < 79; ++tick) {
            weather.tick(true);
        }
        assert(nearlyEqual(weather.rainGradient(), 1.0F));
        assert(weather.isRaining());
    }

    // --- The gradient lerps between the previous/current pair for frames. ---
    {
        WeatherSystem weather;
        weather.setWeather(0, 1000, true, false);
        weather.tick(true);
        assert(nearlyEqual(weather.rainGradientAt(0.0F), 0.0F));  // prev
        assert(nearlyEqual(weather.rainGradientAt(1.0F), 0.01F)); // current
        assert(nearlyEqual(weather.rainGradientAt(0.5F), 0.005F));
        assert(nearlyEqual(weather.visualSkyLightFactorAt(0.0F), 1.0F));
        assert(nearlyEqual(weather.visualSkyLightFactorAt(1.0F),
                           1.0F - 0.01F * 5.0F / 16.0F));
    }

    // Rain and thunder independently reduce the rendered sky contribution;
    // neither operation changes the persistent weather or world light shape.
    {
        WeatherSystem weather;
        weather.forceRainGradient(1.0F);
        weather.forceThunderGradient(1.0F);
        assert(nearlyEqual(weather.visualSkyLightFactorAt(1.0F),
                           (11.0F / 16.0F) * (11.0F / 16.0F)));
    }

    // --- The gradient decays after a rain spell ends. ---
    {
        WeatherSystem weather;
        weather.setWeather(0, 1000, true, false);
        for (int tick = 0; tick < 100; ++tick) {
            weather.tick(true);
        }
        assert(nearlyEqual(weather.rainGradient(), 1.0F));
        weather.resetWeather();  // ServerWorld.resetWeather ends the storm
        assert(!weather.state().raining);
        for (int tick = 0; tick < 100; ++tick) {
            weather.tick(true);
        }
        assert(nearlyEqual(weather.rainGradient(), 0.0F));
        assert(!weather.isRaining());
    }

    // --- doWeatherCycle=false freezes the auto-cycle but not the smoothing. ---
    {
        WeatherSystem weather;
        weather.setWeather(0, 100, true, false);
        for (int tick = 0; tick < 5; ++tick) {
            weather.tick(false);
        }
        // The spell is untouched ...
        assert(weather.state().rainTime == 100);
        assert(weather.state().raining);
        // ... but the gradient still chases the flag, like vanilla.
        assert(nearlyEqual(weather.rainGradient(), 0.05F));
    }

    // --- restore() fades the gradient straight to the saved flag. ---
    {
        WeatherSystem weather;
        WeatherState saved;
        saved.raining = true;
        saved.thundering = false;
        saved.rainTime = 1000;
        saved.thunderTime = 1000;
        saved.clearWeatherTime = 0;
        weather.restore(saved);
        assert(weather.state().raining);
        assert(nearlyEqual(weather.rainGradient(), 1.0F));  // initWeatherGradients
        assert(weather.isRaining());
        assert(nearlyEqual(weather.visualSkyLightFactorAt(1.0F), 11.0F / 16.0F));
    }

    // --- state() round-trips setWeather through the save shape. ---
    {
        WeatherSystem weather;
        weather.setWeather(300, 6000, true, false);
        const WeatherState captured = weather.state();
        WeatherSystem restored;
        restored.restore(captured);
        assert(restored.state().clearWeatherTime == 300);
        assert(restored.state().rainTime == 6000);
        assert(restored.state().thunderTime == 6000);
        assert(restored.state().raining);
        assert(!restored.state().thundering);
    }

    // --- The same seed replays the same auto-cycle history. ---
    {
        WeatherSystem first;
        WeatherSystem second;
        first.seedRandom(0x12345678U);
        second.seedRandom(0x12345678U);
        // A clear interlude is at most 180000 ticks and a rain spell 24000, so
        // 250000 ticks definitely span a weather flip.
        int firstFlipsToRainAt = -1;
        int secondFlipsToRainAt = -1;
        for (int tick = 1; tick <= 250'000; ++tick) {
            first.tick(true);
            second.tick(true);
            if (firstFlipsToRainAt < 0 && first.state().raining) firstFlipsToRainAt = tick;
            if (secondFlipsToRainAt < 0 && second.state().raining) secondFlipsToRainAt = tick;
        }
        assert(firstFlipsToRainAt >= 0);   // the auto-cycle actually rains
        assert(firstFlipsToRainAt == secondFlipsToRainAt);
        assert(first.state().raining == second.state().raining);
    }

    std::cout << "weather system: OK\n";
    return 0;
}

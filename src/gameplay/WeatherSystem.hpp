#pragma once

#include <cstdint>

namespace mc::gameplay {

// The persistent weather state, the C++ mirror of 1.16.1's LevelProperties
// weather fields (clearWeatherTime / rainTime / thunderTime / raining /
// thundering). It travels in the save; the transient rain/thunder gradients do
// not.
struct WeatherState final {
    int clearWeatherTime = 0;
    int rainTime = 0;
    int thunderTime = 0;
    bool raining = false;
    bool thundering = false;
};

// The 20 TPS weather system, a faithful C++ port of 1.16.1's split between
// ServerWorld (the timers and flags) and World (the smoothed gradients).
// setWeather() installs a spell exactly like WeatherCommand; tick() runs the
// auto-cycle from ServerWorld.tick's "weather" profiler section, then ramps
// the gradients 0.01 per tick toward the flags — full rain in five seconds.
//
// Performance: the whole tick is a handful of integer/float field writes. The
// allocation and network-packet churn vanilla pays whenever a gradient moves
// (a GameStateChangeS2CPacket per changed value per tick) has no analogue here
// — the render thread just reads the two floats.
class WeatherSystem final {
  public:
    // ServerWorld#setWeather: installs the four timers/flags at once. /weather
    // calls it with the same arguments WeatherCommand uses — clear is
    // setWeather(duration, 0, false, false), rain is setWeather(0, duration,
    // true, false), and thunder is setWeather(0, duration, true, true).
    void setWeather(int clearDuration, int rainDuration, bool raining, bool thundering);

    // ServerWorld.tick's weather section: the auto-cycle (gated on the
    // doWeatherCycle gamerule) then the gradient smoothing toward the flags.
    void tick(bool doWeatherCycle);

    // ServerWorld#resetWeather: the all-players-sleeping skip path that ends
    // any storm. Nothing in the session sleeps yet, but this is the canonical
    // "end the weather" API the sleeping feature will call.
    void resetWeather();

    // Restores persisted state (World#initWeatherGradients: a world that saves
    // mid-rain reopens with the rain fully faded in instead of fading up from
    // nothing over five seconds).
    void restore(const WeatherState& state);

    // Seeds the auto-cycle RNG from the world seed, the way the session seeds
    // its loot RNG. The RNG only feeds the length rolls of the auto-cycle; the
    // result is not persisted (vanilla does not persist its Random either).
    void seedRandom(std::uint32_t seed);

    // Test/perf harness helper: snaps the smoothed rain gradient (and the
    // raining flag) to a target in [0, 1] immediately, bypassing the 0.01/tick
    // ramp. Lets a smoke run exercise full-intensity rain without waiting the
    // five-second fade-in.
    void forceRainGradient(float gradient);

    // The thunder side of forceRainGradient: snaps the thunder gradient (and the
    // thundering flag) so a smoke run can exercise the storm's heavier rain and
    // wind without waiting for the ramp.
    void forceThunderGradient(float gradient);

    [[nodiscard]] WeatherState state() const;

    // Gradient-derived accessors, mirroring World#isRaining / #isThundering.
    // isRaining() answers the same question a render-side rain reads:
    // rainGradient(1.0F) > 0.2. The raw flags behind them live in state().
    [[nodiscard]] bool isRaining() const;
    [[nodiscard]] bool isThundering() const;

    // The smoothed rain intensity in [0, 1], ramping at 0.01 per tick toward
    // the raining flag. rainGradientAt(delta) lerps the previous/current pair
    // for frame interpolation, the way the client's particle rain samples
    // World#getRainGradient(delta) every frame. This is the surface a future
    // particle-rain test drives.
    [[nodiscard]] float rainGradient() const { return rainGradient_; }
    [[nodiscard]] float rainGradientAt(float delta) const;
    [[nodiscard]] float thunderGradient() const { return thunderGradient_; }
    [[nodiscard]] float thunderGradientAt(float delta) const;

    // ClientWorld#getSkyBrightness applies weather after sampling the logical
    // sky-light level: rain and thunder each remove at most 5/16 of the visual
    // sky contribution. Keeping this as a render multiplier means /weather can
    // make the scene overcast without changing World::skyLight() or mob-spawn
    // light checks.
    [[nodiscard]] float visualSkyLightFactorAt(float delta) const;

  private:
    // LevelProperties mirrors — the five fields setWeather and the auto-cycle
    // write, and the save serialises.
    int clearWeatherTime_ = 0;
    int rainTime_ = 0;
    int thunderTime_ = 0;
    bool raining_ = false;
    bool thundering_ = false;

    // World mirrors — transient, recomputed every tick, never saved.
    float rainGradient_ = 0.0F;
    float rainGradientPrev_ = 0.0F;
    float thunderGradient_ = 0.0F;
    float thunderGradientPrev_ = 0.0F;

    std::uint32_t randomState_ = 0x57E4F10AU;
};

} // namespace mc::gameplay

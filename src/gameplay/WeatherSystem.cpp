#include "gameplay/WeatherSystem.hpp"

#include <algorithm>
#include <cstdint>

namespace mc::gameplay {
namespace {

// The gradient step per 20 TPS tick (ServerWorld.tick's weather section):
// 0.01 per tick reaches full rain in 100 ticks, i.e. five seconds.
constexpr float kGradientStepPerTick = 0.01F;

// World#isRaining: the smoothed gradient must cross 0.2 before the world counts
// as raining. World#isThundering crosses at 0.9.
constexpr float kRainingThreshold = 0.2F;
constexpr float kThunderingThreshold = 0.9F;

// The auto-cycle's length rolls (ServerWorld.tick). A rain spell lasts
// 12000-24000 ticks (10-20 minutes) and a clear spell 12000-180000; the thunder
// side of a storm runs 3600-15600 while sharing the clear spell. All four draw
// from the same 168000 span the else branch uses for a clear interlude.
constexpr int kThunderToClearMinTicks = 3'600;
constexpr int kThunderToClearSpan = 12'000;
constexpr int kRainToClearMinTicks = 12'000;
constexpr int kRainToClearSpan = 12'000;
constexpr int kClearToRainMinTicks = 12'000;
constexpr int kClearToRainSpan = 168'000;

// The same xorshift WorldSimulation uses, so the codebase has one RNG shape.
[[nodiscard]] std::uint32_t nextRandom(std::uint32_t& state) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

} // namespace

void WeatherSystem::setWeather(int clearDuration, int rainDuration, bool raining,
                               bool thundering) {
    clearWeatherTime_ = clearDuration;
    rainTime_ = rainDuration;
    // ServerWorld#setWeather sets thunderTime to the rain duration too, so a
    // thunderless rain spell still expires the way /weather rain expects.
    thunderTime_ = rainDuration;
    raining_ = raining;
    thundering_ = thundering;
}

void WeatherSystem::tick(bool doWeatherCycle) {
    if (doWeatherCycle) {
        int clear = clearWeatherTime_;
        int thunder = thunderTime_;
        int rain = rainTime_;
        bool thundering = thundering_;
        bool raining = raining_;
        if (clear > 0) {
            // A clear spell counts down while forcing the weather off; the
            // rain/thunder timers idle at 0 or 1 so the moment it ends the
            // auto-cycle resumes rolling their lengths.
            --clear;
            thunder = thundering ? 0 : 1;
            rain = raining ? 0 : 1;
            thundering = false;
            raining = false;
        } else {
            if (thunder > 0) {
                if (--thunder == 0) thundering = !thundering;
            } else if (thundering) {
                thunder = static_cast<int>(nextRandom(randomState_) % kThunderToClearSpan) +
                    kThunderToClearMinTicks;
            } else {
                thunder = static_cast<int>(nextRandom(randomState_) % kClearToRainSpan) +
                    kClearToRainMinTicks;
            }
            if (rain > 0) {
                if (--rain == 0) raining = !raining;
            } else if (raining) {
                rain = static_cast<int>(nextRandom(randomState_) % kRainToClearSpan) +
                    kRainToClearMinTicks;
            } else {
                rain = static_cast<int>(nextRandom(randomState_) % kClearToRainSpan) +
                    kClearToRainMinTicks;
            }
        }
        thunderTime_ = thunder;
        rainTime_ = rain;
        clearWeatherTime_ = clear;
        thundering_ = thundering;
        raining_ = raining;
    }

    // The gradient chases its flag: +0.01 per tick while active, -0.01
    // otherwise, clamped to [0, 1]. The previous/current pair feeds the
    // frame-interpolated accessor.
    thunderGradientPrev_ = thunderGradient_;
    thunderGradient_ = std::clamp(
        thunderGradient_ + (thundering_ ? kGradientStepPerTick : -kGradientStepPerTick),
        0.0F, 1.0F);
    rainGradientPrev_ = rainGradient_;
    rainGradient_ = std::clamp(
        rainGradient_ + (raining_ ? kGradientStepPerTick : -kGradientStepPerTick),
        0.0F, 1.0F);
}

void WeatherSystem::resetWeather() {
    rainTime_ = 0;
    raining_ = false;
    thunderTime_ = 0;
    thundering_ = false;
}

void WeatherSystem::restore(const WeatherState& state) {
    clearWeatherTime_ = state.clearWeatherTime;
    rainTime_ = state.rainTime;
    thunderTime_ = state.thunderTime;
    raining_ = state.raining;
    thundering_ = state.thundering;
    // World#initWeatherGradients: a save captured mid-rain reopens with the
    // rain already at full intensity rather than fading up from nothing.
    rainGradient_ = rainGradientPrev_ = raining_ ? 1.0F : 0.0F;
    thunderGradient_ = thunderGradientPrev_ = thundering_ ? 1.0F : 0.0F;
}

void WeatherSystem::seedRandom(std::uint32_t seed) {
    randomState_ = seed != 0U ? seed : 0x57E4F10AU;
}

WeatherState WeatherSystem::state() const {
    return {clearWeatherTime_, rainTime_, thunderTime_, raining_, thundering_};
}

bool WeatherSystem::isRaining() const {
    return rainGradientAt(1.0F) > kRainingThreshold;
}

bool WeatherSystem::isThundering() const {
    return thunderGradientAt(1.0F) > kThunderingThreshold;
}

float WeatherSystem::rainGradientAt(float delta) const {
    return rainGradientPrev_ + (rainGradient_ - rainGradientPrev_) * delta;
}

float WeatherSystem::thunderGradientAt(float delta) const {
    return thunderGradientPrev_ + (thunderGradient_ - thunderGradientPrev_) * delta;
}

} // namespace mc::gameplay

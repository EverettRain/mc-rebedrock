#include "world/WorldClock.hpp"

#include "world/DayNightCycle.hpp"

#include <algorithm>
#include <cmath>

namespace mc::world {
namespace {

constexpr std::uint64_t kDayPeriodTicks =
    static_cast<std::uint64_t>(DayNightCycle::kTicksPerDay);

// ClockTimeMarker#durationToNext: the distance from `from` to `to` inside one
// period, wrapping to the next period when the marker has already passed. A
// marker that sits exactly on the current tick resolves to a whole period
// ahead rather than to zero, matching vanilla's `duration > 0` test.
[[nodiscard]] std::uint64_t durationToNext(std::uint64_t period, std::uint64_t from,
                                           std::uint64_t to) {
    return to > from ? to - from : period + to - from;
}

} // namespace

std::uint64_t timeMarkerTicks(ClockTimeMarker marker) {
    switch (marker) {
    case ClockTimeMarker::Day:
        return 1'000U;
    case ClockTimeMarker::Noon:
        return 6'000U;
    case ClockTimeMarker::Night:
        return 13'000U;
    case ClockTimeMarker::Midnight:
        return 18'000U;
    }
    return 0U;
}

void ClockManager::tick() {
    for (auto& clock : clocks_) {
        if (clock.paused) continue;
        // ServerClockManager.ClockInstance#tick: accumulate the rate, take the
        // whole ticks out of it and keep the remainder. A rate of 1 is the
        // ordinary "one clock tick per server tick".
        clock.partialTick += clock.rate;
        const float fullTicks = std::floor(clock.partialTick);
        clock.partialTick -= fullTicks;
        if (fullTicks > 0.0F) {
            clock.totalTicks += static_cast<std::uint64_t>(fullTicks);
        }
    }
}

void ClockManager::setTotalTicks(ClockId clock, std::uint64_t ticks) {
    auto& state = mutableState(clock);
    state.totalTicks = ticks;
    state.partialTick = 0.0F;
}

void ClockManager::addTicks(ClockId clock, std::int64_t ticks) {
    auto& state = mutableState(clock);
    if (ticks < 0) {
        const auto magnitude = static_cast<std::uint64_t>(-ticks);
        state.totalTicks = magnitude > state.totalTicks ? 0U : state.totalTicks - magnitude;
    } else {
        state.totalTicks += static_cast<std::uint64_t>(ticks);
    }
}

void ClockManager::moveToTimeMarker(ClockId clock, ClockTimeMarker marker) {
    auto& state = mutableState(clock);
    // ClockTimeMarker#resolveTimeToMoveTo: forward to the next occurrence, so a
    // clock is monotonic no matter which /time form was used.
    state.totalTicks += durationToNext(kDayPeriodTicks, state.totalTicks % kDayPeriodTicks,
                                       timeMarkerTicks(marker));
    state.partialTick = 0.0F;
}

void ClockManager::setPaused(ClockId clock, bool paused) {
    mutableState(clock).paused = paused;
}

void ClockManager::setRate(ClockId clock, float rate) {
    mutableState(clock).rate = std::max(0.0F, rate);
}

} // namespace mc::world

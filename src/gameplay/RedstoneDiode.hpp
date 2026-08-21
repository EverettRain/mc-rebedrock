#pragma once

// DiodeBlock's driving decisions (W-4), source-derived from Java 26.1's
// DiodeBlock/RepeaterBlock. Repeater and comparator share this skeleton: the
// block-update decision (checkTickOnNeighbor -> whether/when/at-what-priority to
// schedule) and the scheduled-tick decision (tick -> flip POWERED, and for a
// pulse shorter than the delay, re-arm the turn-off). Kept as pure functions so
// the priority ladder and the pulse extension can be pinned in isolation from
// the scheduler/world plumbing. Deterministic — no random source.

#include "gameplay/ChunkTickScheduler.hpp" // TickPriority
#include "world/BlockState.hpp"

#include <optional>

namespace mc::gameplay::redstone {

// RepeaterBlock.getDelay = DELAY * 2 gameticks (2/4/6/8 for a 1-4 tick delay).
[[nodiscard]] inline int repeaterDelayGameticks(world::BlockState state) {
    return state.repeaterDelay() * 2;
}

// A scheduled diode tick: when (relative delay) and at what priority.
struct DiodeSchedule final {
    int delayGameticks = 0;
    TickPriority priority = TickPriority::High;
};

// checkTickOnNeighbor: a diode schedules its flip when its output no longer
// matches its input and no tick is already pending, at HIGH (turning on),
// VERY_HIGH (turning off), or EXTREMELY_HIGH (a diode faces its output). A
// locked diode does nothing. DiodeBlock.checkTickOnNeighbor:100-113.
[[nodiscard]] inline std::optional<DiodeSchedule> diodeCheckTick(world::BlockState state,
                                                                 bool shouldTurnOn, bool isLocked,
                                                                 bool alreadyScheduled,
                                                                 bool shouldPrioritize,
                                                                 int delayGameticks) {
    if (isLocked) {
        return std::nullopt;
    }
    const bool on = state.powered();
    if (on == shouldTurnOn || alreadyScheduled) {
        return std::nullopt;
    }
    TickPriority priority = TickPriority::High;
    if (shouldPrioritize) {
        priority = TickPriority::ExtremelyHigh;
    } else if (on) {
        priority = TickPriority::VeryHigh;
    }
    return DiodeSchedule{delayGameticks, priority};
}

struct DiodeTickResult final {
    bool changed = false;
    world::BlockState newState{};
    // A turn-on whose input has already dropped re-arms a turn-off delay ticks
    // later, so a pulse shorter than the delay still comes out full length.
    std::optional<DiodeSchedule> pulseReschedule;
};

// tick: apply the delayed flip. DiodeBlock.tick:53-67.
[[nodiscard]] inline DiodeTickResult diodeTick(world::BlockState state, bool shouldTurnOn,
                                               bool isLocked, int delayGameticks) {
    DiodeTickResult result;
    if (isLocked) {
        return result;
    }
    const bool on = state.powered();
    if (on && !shouldTurnOn) {
        result.changed = true;
        result.newState = state.withPowered(false);
    } else if (!on) {
        result.changed = true;
        result.newState = state.withPowered(true);
        if (!shouldTurnOn) {
            result.pulseReschedule = DiodeSchedule{delayGameticks, TickPriority::VeryHigh};
        }
    }
    return result;
}

} // namespace mc::gameplay::redstone

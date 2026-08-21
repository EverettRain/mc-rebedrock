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

// --- Comparator (ComparatorBlock, a diode with an analog output) ---

// ComparatorBlock.calculateOutputSignal + shouldTurnOn. `input` is the back
// signal, `alt` the strongest side signal, `subtract` the mode.
struct ComparatorEval final {
    int output = 0;
    bool shouldTurnOn = false;
};
[[nodiscard]] inline ComparatorEval comparatorEvaluate(int input, int alt, bool subtract) {
    if (input == 0) {
        return {0, false};
    }
    const int output = alt > input ? 0 : (subtract ? input - alt : input);
    // input > alt turns on; input == alt turns on only in COMPARE mode.
    const bool shouldTurnOn = input > alt || (input == alt && !subtract);
    return {output, shouldTurnOn};
}

// ComparatorBlock.checkTickOnNeighbor: schedule (delay 2) when the analog output
// or the POWERED boolean would change. NORMAL priority, or HIGH facing a diode.
// A comparator never locks.
[[nodiscard]] inline std::optional<DiodeSchedule> comparatorCheckTick(world::BlockState state,
                                                                      ComparatorEval eval,
                                                                      bool alreadyScheduled,
                                                                      bool shouldPrioritize) {
    if (alreadyScheduled) {
        return std::nullopt;
    }
    if (eval.output == state.analogSignal() && state.powered() == eval.shouldTurnOn) {
        return std::nullopt;
    }
    return DiodeSchedule{2, shouldPrioritize ? TickPriority::High : TickPriority::Normal};
}

struct ComparatorTickResult final {
    bool changed = false;
    world::BlockState newState{};
    bool notifyFront = false;
};

// ComparatorBlock.refreshOutputState: always store the fresh analog output; when
// it changed (or in COMPARE mode) set POWERED to shouldTurnOn and notify the
// block in front.
[[nodiscard]] inline ComparatorTickResult comparatorTick(world::BlockState state,
                                                         ComparatorEval eval) {
    ComparatorTickResult result;
    world::BlockState next = state.withAnalogSignal(eval.output);
    const bool doUpdate = state.analogSignal() != eval.output || !state.comparatorSubtract();
    if (doUpdate) {
        const bool isOn = state.powered();
        if (isOn && !eval.shouldTurnOn) {
            next = next.withPowered(false);
        } else if (!isOn && eval.shouldTurnOn) {
            next = next.withPowered(true);
        }
        result.notifyFront = true;
    }
    result.newState = next;
    result.changed = next != state;
    return result;
}

} // namespace mc::gameplay::redstone

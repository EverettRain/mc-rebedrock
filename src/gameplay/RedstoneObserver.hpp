#pragma once

// The observer's scheduled-tick decision (W-4), source-derived from
// ObserverBlock. Detection (a block state change on the FACING side while not
// already pulsing) is handled where the updateShape pass runs; this is the tick
// that raises and then, two gameticks later, drops the pulse. Deterministic,
// fixed 2gt, edge-triggered — the pulse length does not depend on the input.

#include "world/BlockState.hpp"

namespace mc::gameplay::redstone {

inline constexpr int kObserverDelay = 2; // gt, ObserverBlock

struct ObserverTickResult final {
    bool changed = false;
    world::BlockState newState{};
    bool reschedule = false; // re-arm the turn-off tick after turning on
};

// ObserverBlock.tick:50-60: POWERED goes out; unpowered goes on and re-arms its
// own turn-off. Either way the front (its back side) is notified by the caller.
[[nodiscard]] inline ObserverTickResult observerTick(world::BlockState state) {
    if (state.powered()) {
        return {true, state.withPowered(false), false};
    }
    return {true, state.withPowered(true), true};
}

} // namespace mc::gameplay::redstone

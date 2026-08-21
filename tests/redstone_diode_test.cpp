#include "gameplay/RedstoneDiode.hpp"

#include "world/Block.hpp"
#include "world/BlockState.hpp"

#include <cstdio>
#include <cstdlib>

// W-4 slice 4: DiodeBlock's driving decisions, pinned against
// DiodeBlock/RepeaterBlock — the priority ladder (HIGH on, VERY_HIGH off,
// EXTREMELY_HIGH diode-facing-diode), the lock/dedup guards, and the pulse
// extension that re-arms a turn-off after a short input.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "redstone_diode_test line %d failed: %s\n", line, expression);
        std::abort();
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using namespace mc::gameplay;
using mc::world::Block;
using mc::world::BlockOrientation;
using mc::world::BlockState;

[[nodiscard]] BlockState repeater(int delay, bool powered) {
    return BlockState{Block::Repeater, BlockOrientation::North}
        .withRepeaterDelay(delay)
        .withPowered(powered);
}

} // namespace

int main() {
    // --- getDelay = DELAY * 2. ---
    REQUIRE(redstone::repeaterDelayGameticks(repeater(1, false)) == 2);
    REQUIRE(redstone::repeaterDelayGameticks(repeater(4, false)) == 8);

    // --- checkTickOnNeighbor: schedule with the right priority. ---
    {
        // Off, should turn on -> HIGH.
        const auto on = redstone::diodeCheckTick(repeater(1, false), true, false, false, false, 2);
        REQUIRE(on.has_value());
        REQUIRE(on->delayGameticks == 2);
        REQUIRE(on->priority == TickPriority::High);
        // On, should turn off -> VERY_HIGH.
        const auto off = redstone::diodeCheckTick(repeater(1, true), false, false, false, false, 2);
        REQUIRE(off.has_value() && off->priority == TickPriority::VeryHigh);
        // Diode faces the output (shouldPrioritize) -> EXTREMELY_HIGH.
        const auto pri = redstone::diodeCheckTick(repeater(1, true), false, false, false, true, 2);
        REQUIRE(pri.has_value() && pri->priority == TickPriority::ExtremelyHigh);
        // Locked: nothing.
        REQUIRE(!redstone::diodeCheckTick(repeater(1, false), true, true, false, false, 2));
        // Already pending (dedup): nothing.
        REQUIRE(!redstone::diodeCheckTick(repeater(1, false), true, false, true, false, 2));
        // Already in the wanted state: nothing.
        REQUIRE(!redstone::diodeCheckTick(repeater(1, true), true, false, false, false, 2));
    }

    // --- tick: apply the flip, with the pulse re-arm. ---
    {
        // Off with input present -> turns on, no re-arm (input still there).
        const auto turnOn = redstone::diodeTick(repeater(2, false), true, false, 4);
        REQUIRE(turnOn.changed && turnOn.newState.powered());
        REQUIRE(!turnOn.pulseReschedule.has_value());
        // On with input gone -> turns off.
        const auto turnOff = redstone::diodeTick(repeater(2, true), false, false, 4);
        REQUIRE(turnOff.changed && !turnOff.newState.powered());
        // Off but input ALREADY gone -> turns on AND re-arms a turn-off (a pulse
        // shorter than the delay still comes out full length).
        const auto pulse = redstone::diodeTick(repeater(2, false), false, false, 4);
        REQUIRE(pulse.changed && pulse.newState.powered());
        REQUIRE(pulse.pulseReschedule.has_value());
        REQUIRE(pulse.pulseReschedule->delayGameticks == 4);
        REQUIRE(pulse.pulseReschedule->priority == TickPriority::VeryHigh);
        // On and should stay on -> nothing.
        REQUIRE(!redstone::diodeTick(repeater(2, true), true, false, 4).changed);
        // Locked: nothing either way.
        REQUIRE(!redstone::diodeTick(repeater(2, true), false, true, 4).changed);
    }

    // --- Comparator evaluate: compare vs subtract, the side-vs-back rules. ---
    {
        // SUBTRACT: output = back - side; on when back > side.
        REQUIRE(redstone::comparatorEvaluate(15, 0, true).output == 15);
        REQUIRE(redstone::comparatorEvaluate(15, 0, true).shouldTurnOn);
        REQUIRE(redstone::comparatorEvaluate(15, 10, true).output == 5);
        REQUIRE(redstone::comparatorEvaluate(15, 10, true).shouldTurnOn);
        // SUBTRACT with side == back: 0, and OFF (equality does not turn on).
        REQUIRE(redstone::comparatorEvaluate(15, 15, true).output == 0);
        REQUIRE(!redstone::comparatorEvaluate(15, 15, true).shouldTurnOn);
        // COMPARE with side == back: passes the back through, ON.
        REQUIRE(redstone::comparatorEvaluate(15, 15, false).output == 15);
        REQUIRE(redstone::comparatorEvaluate(15, 15, false).shouldTurnOn);
        // side > back: 0 and OFF, both modes.
        REQUIRE(redstone::comparatorEvaluate(10, 15, false).output == 0);
        REQUIRE(!redstone::comparatorEvaluate(10, 15, false).shouldTurnOn);
        // no back input: 0, OFF.
        REQUIRE(redstone::comparatorEvaluate(0, 5, false).output == 0);
        REQUIRE(!redstone::comparatorEvaluate(0, 5, false).shouldTurnOn);
    }

    // --- Comparator checkTick / tick. ---
    {
        const auto fresh = BlockState{Block::Comparator, BlockOrientation::North}
                               .withComparatorSubtract(true)
                               .withAnalogSignal(0)
                               .withPowered(false);
        // Back 15, no side: output 15, should turn on -> schedule at NORMAL.
        const auto eval = redstone::comparatorEvaluate(15, 0, true);
        const auto sched = redstone::comparatorCheckTick(fresh, eval, false, false);
        REQUIRE(sched.has_value());
        REQUIRE(sched->delayGameticks == 2);
        REQUIRE(sched->priority == TickPriority::Normal);
        REQUIRE(!redstone::comparatorCheckTick(fresh, eval, true, false)); // dedup
        // Diode faces output -> HIGH.
        REQUIRE(redstone::comparatorCheckTick(fresh, eval, false, true)->priority ==
                TickPriority::High);
        // Nothing to change.
        const auto settled = fresh.withAnalogSignal(15).withPowered(true);
        REQUIRE(!redstone::comparatorCheckTick(settled, eval, false, false));

        // tick writes the analog value and turns on.
        const auto tick = redstone::comparatorTick(fresh, eval);
        REQUIRE(tick.changed);
        REQUIRE(tick.newState.analogSignal() == 15);
        REQUIRE(tick.newState.powered());
        REQUIRE(tick.notifyFront);
    }

    return 0;
}

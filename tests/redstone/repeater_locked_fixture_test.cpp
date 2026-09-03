#include "redstone/RedstoneHarness.hpp"

#include "gameplay/BlockBehavior.hpp"

// AR-B4-4: RepeaterBlock's LOCKED, both of vanilla's write points.
//
//   getStateForPlacement:59-62 — locked is known the moment the repeater lands.
//   updateShape:78-80          — recomputed whenever a neighbour changes on any
//                                axis *other than* the repeater's own FACING
//                                axis. FACING is horizontal, so Y counts: a
//                                change directly above or below is on axis Y,
//                                which is not the FACING axis, and reading the
//                                condition as "the two horizontal sides" drops
//                                it and leaves LOCKED stale.
//
// The property serves the renderer and the save. The *simulation* keeps deriving
// the lock at tick time through redstone::repeaterIsLocked, exactly as vanilla's
// DiodeBlock.tick calls isLocked() rather than reading the property. Both are
// asserted here together, so that nobody later "unifies" one into the other and
// quietly makes the tick depend on write ordering.

namespace {

using namespace mc::test::redstone;
using mc::gameplay::redstone::Direction;
using mc::world::BlockPos;

void require(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "repeater locked fixture: %s\n", what);
        std::abort();
    }
}

// In this harness a repeater's Direction argument names its *input* side (see
// repeater_delay_fixture_test's bench), so the output is the opposite cell.
//
//   (0,0,0)   main repeater, input East  -> driven by the lever at (1,0,0)
//   (0,0,-1)  side repeater, input North -> output South, i.e. into the main
//             repeater's north side, which is what locks it
//   (0,0,-2)  the lever driving the side repeater
void buildLatch(RedstoneCircuit& circuit) {
    circuit.solid({0, -1, 0})    // floor under the main repeater
        .solid({0, -1, -1})      // floor under the side repeater
        .solid({1, 0, 1})        // the main lever's mount wall
        .solid({0, 0, -3})       // the side lever's mount wall
        .repeater({0, 0, 0}, Direction::East, 1)
        .lever({1, 0, 0}, Direction::North, /*on=*/false)
        .repeater({0, 0, -1}, Direction::North, 1)
        .lever({0, 0, -2}, Direction::South, /*on=*/false);
}

const BlockPos kMain{0, 0, 0};
const BlockPos kMainLever{1, 0, 0};
const BlockPos kSideLever{0, 0, -2};

} // namespace

int main() {
    // --- The property tracks the side diode, in both directions, and agrees
    // with the derivation the simulation uses. ---
    {
        RedstoneCircuit circuit;
        buildLatch(circuit);
        require(!circuit.state(kMain).repeaterLocked(), "should start unlocked");

        circuit.setLever(kSideLever, true);
        circuit.advance(6); // the side repeater's own 2gt delay, plus slack
        require(mc::gameplay::redstone::repeaterIsLocked(
                    circuit.worldRef(), circuit.absoluteOf(kMain), circuit.state(kMain)),
                "the derivation should report locked once the side diode is powered");
        require(circuit.state(kMain).repeaterLocked(),
                "and the LOCKED property should have been written, not merely be derivable");

        circuit.setLever(kSideLever, false);
        circuit.advance(6);
        require(!circuit.state(kMain).repeaterLocked(), "LOCKED must fall with the side diode");
        require(!mc::gameplay::redstone::repeaterIsLocked(
                    circuit.worldRef(), circuit.absoluteOf(kMain), circuit.state(kMain)),
                "derivation and property must not drift apart");
    }

    // --- The behaviour the property describes: DiodeBlock.tick skips its whole
    // body while isLocked(), so a locked repeater holds its output when the
    // input falls, and follows it again once unlocked. This is the simulation
    // running off the derivation, not off the property. ---
    {
        RedstoneCircuit circuit;
        buildLatch(circuit);
        circuit.setLever(kMainLever, true);
        circuit.advance(6);
        require(circuit.lit(kMain), "the repeater should be on before locking");

        circuit.setLever(kSideLever, true);
        circuit.advance(6);
        require(circuit.state(kMain).repeaterLocked(), "should be locked");

        circuit.setLever(kMainLever, false);
        circuit.advance(8);
        require(circuit.lit(kMain), "a locked repeater must hold its output when the input falls");

        circuit.setLever(kSideLever, false);
        circuit.advance(8);
        require(!circuit.state(kMain).repeaterLocked(), "unlocked once the side diode falls");
        // And the held output is released with nobody touching the input. This
        // used to need the main lever nudged, because a diode's write woke
        // nothing in front of it; W-x-1 gave DiodeBlock#updateNeighborsInFront
        // its two steps, so the side diode falling now reaches this repeater on
        // its own and it drops the output it had been holding.
        require(!circuit.lit(kMain),
                "unlocking must release the held output with no further input change");
    }

    // --- The axis rule itself, exercised at the slot rather than through a
    // circuit. Vanilla's condition is `axis != FACING.getAxis()`, and FACING is
    // horizontal, so *Y passes it*: a neighbour change above or below triggers
    // the recompute. A circuit cannot tell the two readings apart — a vertical
    // edit never changes whether a side diode is powered — so the discriminator
    // is a deliberately stale LOCKED that only the correct axis rule repairs.
    // Reading the condition as "the two horizontal sides" leaves it stale.
    {
        RedstoneCircuit circuit;
        buildLatch(circuit);
        circuit.setLever(kSideLever, true);
        circuit.advance(6);
        require(circuit.state(kMain).repeaterLocked(), "locked, so the truth is LOCKED=true");

        const auto stale = circuit.state(kMain).withRepeaterLocked(false);
        const auto mainAbs = circuit.absoluteOf(kMain);
        const auto runSlot = [&](mc::world::BlockPos fromOffset) {
            const mc::world::BlockPos neighbor{mainAbs.x + fromOffset.x, mainAbs.y + fromOffset.y,
                                               mainAbs.z + fromOffset.z};
            const mc::gameplay::NeighborUpdateContext context{
                circuit.worldRef(), mainAbs, stale, fromOffset,
                circuit.worldRef().state(neighbor.x, neighbor.y, neighbor.z)};
            return mc::gameplay::detail::repeaterLockedUpdateShapeSlot(context);
        };
        // Above and below are axis Y, which is not the FACING axis: recompute.
        require(runSlot({0, 1, 0}).value().repeaterLocked(),
                "a neighbour change ABOVE must recompute LOCKED (axis Y != FACING axis)");
        require(runSlot({0, -1, 0}).value().repeaterLocked(),
                "a neighbour change BELOW must recompute LOCKED as well");
        // The sides are perpendicular to FACING, so they recompute too.
        require(runSlot({0, 0, 1}).value().repeaterLocked(), "a side change must recompute");
        // Only the FACING axis is excluded.
        require(!runSlot({1, 0, 0}).value().repeaterLocked(),
                "a change on the repeater's own FACING axis must be skipped");
        require(!runSlot({-1, 0, 0}).value().repeaterLocked(),
                "...on both sides of that axis");
    }

    // --- The Y axis through a circuit: a vertical edit must not corrupt a
    // correct answer either. ---
    {
        RedstoneCircuit circuit;
        buildLatch(circuit);
        circuit.setLever(kSideLever, true);
        circuit.advance(6);
        require(circuit.state(kMain).repeaterLocked(), "locked before the vertical edit");
        circuit.place({0, 1, 0}, mc::world::BlockState{mc::world::Block::Stone});
        require(circuit.state(kMain).repeaterLocked(),
                "a vertical neighbour edit must leave a genuinely locked repeater locked");
        circuit.setLever(kSideLever, false);
        circuit.advance(6);
        require(!circuit.state(kMain).repeaterLocked(),
                "and the lock still falls with a block sitting on top");
    }

    return 0;
}

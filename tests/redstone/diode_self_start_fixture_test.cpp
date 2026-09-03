#include "redstone/RedstoneHarness.hpp"

// W-8: DiodeBlock#setPlacedBy (DiodeBlock.java:159-163).
//
//     if (this.shouldTurnOn(level, pos, state)) level.scheduleTick(pos, this, 1);
//
// A diode dropped into a line that is already live has to start itself. Nothing
// else will: a placement's ordinary fan-out tells the new block's *neighbours*
// that something appeared, never the new cell about itself, so a repeater placed
// onto a powered wire stayed dark.
//
// The semantics matter more than the fix. Java has two hooks — onPlace, which
// runs on every setBlockState including a POWERED flip, and setPlacedBy, which
// runs only when the block genuinely arrived. W-7's output wake wants the first;
// this wants the second, so the behaviour slot is defined as setPlacedBy and
// MutationSink::onBlockPlaced is gated on the block kind changing.
//
// Measured honestly: widening it to every write does *not* break any output
// here. The schedule is deduplicated, the shouldTurnOn guard rejects most of it,
// and a redundant diode tick writes nothing — the whole suite still passed with
// it widened. The cost is wasted scheduled ticks, not a wrong answer. So the
// semantics are pinned where they are observable, in
// world_mutation_service_test, rather than pretended to be visible here.
//
// Reproduction trap, worth stating because the first attempt fell into it: a
// placement's own fan-out will wake the diode if anything else is placed nearby
// in the same breath. The bench below places the diode and then touches nothing.

namespace {

using namespace mc::test::redstone;
using mc::gameplay::redstone::Direction;
using mc::world::BlockPos;

void require(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "diode self start: %s\n", what);
        std::abort();
    }
}

const BlockPos kDiode{2, 0, 0};
const BlockPos kWire{1, 0, 0};
const BlockPos kLever{3, 0, 0};

// Floor, a lever on a wall, and a wire the lever drives. Everything except the
// diode, so a later placement is the only thing that happens.
void bench(RedstoneCircuit& circuit) {
    for (int x = 0; x <= 3; ++x) {
        circuit.solid({x, -1, 0});
    }
    circuit.solid({3, 0, -1});
    circuit.wire(kWire).lever(kLever, Direction::South, /*on=*/false);
}

} // namespace

int main() {
    // --- A: the defect. The line is already live; place only the repeater and
    // wait. Before W-8 it stayed dark for good. ---
    {
        RedstoneCircuit circuit;
        bench(circuit);
        circuit.setLever(kLever, true);
        circuit.advance(4);
        require(circuit.power(kWire) == 0, "the wire past the gap is dark with no diode");

        circuit.placeBlock(kDiode, mc::world::BlockState{mc::world::Block::Repeater,
                                                          orientationOf(Direction::East)});
        // Nothing else is touched from here on — that is the whole test.
        circuit.advance(8);
        require(circuit.lit(kDiode), "a repeater placed into a live line must start itself");
        require(circuit.power(kWire) > 0, "and drive what it feeds");
    }

    // --- B: the control. The same repeater, present before the lever goes on,
    // has always worked — so A is about placement and not about the circuit. ---
    {
        RedstoneCircuit circuit;
        bench(circuit);
        circuit.repeater(kDiode, Direction::East, 1);
        circuit.setLever(kLever, true);
        circuit.advance(8);
        require(circuit.lit(kDiode), "a pre-existing repeater turns on from the lever");
    }

    // --- C: why it went unnoticed for so long. Place the diode into a dead
    // line, then energise: it catches up, because the lever's own fan-out wakes
    // it. Any nearby edit "heals" the bug, which is exactly why a bench that
    // places anything else alongside the diode cannot reproduce it. ---
    {
        RedstoneCircuit circuit;
        bench(circuit);
        circuit.placeBlock(kDiode, mc::world::BlockState{mc::world::Block::Repeater,
                                                          orientationOf(Direction::East)});
        circuit.advance(4);
        require(!circuit.lit(kDiode), "placed into a dead line it stays off, correctly");
        circuit.setLever(kLever, true);
        circuit.advance(8);
        require(circuit.lit(kDiode), "and follows the lever once it is live");
    }

    // --- The comparator too: shouldTurnOn is virtual in Java, and a comparator
    // decides it from its own signal evaluation rather than the repeater's
    // "is my input live". ---
    {
        RedstoneCircuit circuit;
        bench(circuit);
        circuit.setLever(kLever, true);
        circuit.advance(4);
        circuit.placeBlock(kDiode, mc::world::BlockState{mc::world::Block::Comparator,
                                                          orientationOf(Direction::East)});
        circuit.advance(8);
        require(circuit.lit(kDiode), "a comparator placed into a live line must start itself");
        require(circuit.power(kWire) > 0, "and drive what it feeds");
    }

    // --- Stability. Not the semantics guard (see the header: that lives in
    // world_mutation_service_test, because a circuit cannot see it), but the
    // property that would actually break if a self-start ever did start feeding
    // itself: drive the diode hard, then leave it alone and check that nothing
    // moves on its own. ---
    {
        RedstoneCircuit circuit;
        bench(circuit);
        circuit.repeater(kDiode, Direction::East, 1);
        for (int i = 0; i < 6; ++i) {
            circuit.setLever(kLever, i % 2 == 0);
            circuit.advance(6);
        }
        circuit.setLever(kLever, true);
        circuit.advance(10);
        const bool settledOn = circuit.lit(kDiode);
        circuit.advance(20); // a long quiet stretch: nothing may change on its own
        require(circuit.lit(kDiode) == settledOn,
                "a settled diode must not keep rescheduling itself");
        require(settledOn, "and it settled in the state the lever asks for");
    }

    return 0;
}

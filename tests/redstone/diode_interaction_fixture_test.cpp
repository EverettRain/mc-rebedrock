#include "redstone/RedstoneHarness.hpp"

#include <cassert>

// AR-B4-7 fixture: the two diode `useWithoutItem` handlers, driven through the
// real UseItemOn path (RedstoneCircuit::openByHand sends the same command a
// player's right-click sends, so nothing here reaches past the interaction
// layer to poke a state bit directly).
//
// Before AR-B4-7 both were simply absent — PlayerInteraction had no reference to
// either block — so every `openByHand` below returned false: a comparator could
// not be switched between compare and subtract, and a repeater's delay could not
// be changed at all.
//
// The load-bearing case is the last one. A comparator's mode change alters its
// OUTPUT immediately (compare 15 vs subtract 15-15), which is why vanilla's
// ComparatorBlock#useWithoutItem ends in `refreshOutputState` rather than just
// writing the state: without it the mode and the sprite change while everything
// downstream keeps the old signal until some unrelated edit happens to wake it.
// Asserting only the comparator's own state would miss that entirely, so the
// probe is a trapdoor hung on the comparator's output face.

namespace {

using namespace mc::test::redstone;
using mc::gameplay::redstone::Direction;
using mc::world::BlockPos;

const BlockPos kDiode{0, 0, 0};

} // namespace

int main() {
    using mc::world::Block;

    // --- A repeater's DELAY cycles 1 -> 2 -> 3 -> 4 -> 1 (RepeaterBlock.java:49,
    //     `state.cycle(DELAY)`), and every click reports that it did something. ---
    {
        RedstoneCircuit circuit;
        circuit.solid({0, -1, 0}).repeater(kDiode, Direction::East, /*delay=*/1);
        assert(circuit.state(kDiode).repeaterDelay() == 1);
        for (const int expected : {2, 3, 4, 1, 2}) {
            assert(circuit.openByHand(kDiode) && "a repeater must answer a right-click");
            assert(circuit.state(kDiode).repeaterDelay() == expected);
        }
    }

    // --- A repeater that is running keeps its POWERED across a delay change: the
    //     click writes DELAY only. (Vanilla's flags-3 write fans out to the
    //     neighbours, which is why this is worth pinning — a fan-out that
    //     re-evaluated the repeater itself could drop it.) ---
    {
        RedstoneCircuit circuit;
        circuit.solid({0, -1, 0})
            .solid({1, 0, -1})
            .repeater(kDiode, Direction::East, /*delay=*/1)
            .lever({1, 0, 0}, Direction::South, /*on=*/false);
        circuit.setLever({1, 0, 0}, true);
        circuit.advance(4);
        assert(circuit.lit(kDiode));
        assert(circuit.openByHand(kDiode));
        assert(circuit.state(kDiode).repeaterDelay() == 2);
        assert(circuit.lit(kDiode) && "a delay change must not drop a running repeater");
    }

    // --- A comparator's MODE cycles, in all four (mode x powered) combinations.
    //     Powered here is driven by a redstone block on the back; the mode click
    //     must flip MODE and leave the block's identity and facing alone. ---
    for (const bool powered : {false, true}) {
        RedstoneCircuit circuit;
        circuit.solid({0, -1, 0}).comparator(kDiode, Direction::East, /*subtract=*/false);
        if (powered) {
            circuit.redstoneBlock({1, 0, 0});
            circuit.advance(4);
        }
        assert(circuit.lit(kDiode) == powered);
        assert(!circuit.state(kDiode).comparatorSubtract());

        assert(circuit.openByHand(kDiode) && "a comparator must answer a right-click");
        assert(circuit.state(kDiode).comparatorSubtract());
        assert(circuit.state(kDiode).block() == Block::Comparator);
        // subtract with nothing on the side subtracts nothing, so POWERED is
        // unchanged by the mode flip in this bench.
        assert(circuit.lit(kDiode) == powered);

        assert(circuit.openByHand(kDiode));
        assert(!circuit.state(kDiode).comparatorSubtract());
        assert(circuit.lit(kDiode) == powered);
    }

    // --- refreshOutputState: back 15, side 15. COMPARE passes the back through
    //     (output 15), SUBTRACT gives 15-15 = 0, so switching mode flips the
    //     output *now*. A trapdoor on the output face must follow within the same
    //     click — no gametick may pass first, because nothing would wake it. ---
    {
        RedstoneCircuit circuit;
        circuit.recordWorldEdits();
        // Comparator facing East: FACING names its input side, so its output is
        // the cell to the west.
        circuit.solid({0, -1, 0})
            .solid({-1, -1, 0})
            .comparator(kDiode, Direction::East, /*subtract=*/false)
            .redstoneBlock({1, 0, 0})   // back input, a constant 15
            .redstoneBlock({0, 0, 1})   // side input, a constant 15
            .trapdoor({-1, 0, 0}, Direction::North);
        circuit.advance(6);
        assert(circuit.lit(kDiode) && "compare with side == back stays on");
        assert(circuit.state({-1, 0, 0}).open() && "the trapdoor starts open, driven at 15");

        circuit.clearPublishedEdits();
        assert(circuit.openByHand(kDiode));
        assert(circuit.state(kDiode).comparatorSubtract());
        assert(!circuit.lit(kDiode) && "subtract with side == back drops the output to 0");
        assert(!circuit.state({-1, 0, 0}).open() &&
               "the trapdoor must close in the same click: refreshOutputState");
        assert(circuit.publishedEdit({-1, 0, 0}) &&
               "and the close must be published, not merely written to the world");

        // Back to COMPARE: the output returns to 15 and the trapdoor reopens,
        // again without a gametick passing.
        circuit.clearPublishedEdits();
        assert(circuit.openByHand(kDiode));
        assert(circuit.lit(kDiode));
        assert(circuit.state({-1, 0, 0}).open());
        assert(circuit.publishedEdit({-1, 0, 0}));
    }

    // --- A comparator with no input at all: switching mode must not invent a
    //     signal. (comparatorEvaluate returns {0,false} for input 0 in both
    //     modes, and refreshOutputState must not turn POWERED on regardless.) ---
    {
        RedstoneCircuit circuit;
        circuit.solid({0, -1, 0})
            .solid({-1, -1, 0})
            .comparator(kDiode, Direction::East, /*subtract=*/false)
            .trapdoor({-1, 0, 0}, Direction::North);
        circuit.advance(4);
        assert(!circuit.lit(kDiode));
        assert(circuit.openByHand(kDiode));
        assert(!circuit.lit(kDiode) && "a mode flip must not power an unfed comparator");
        assert(!circuit.state({-1, 0, 0}).open());
    }

    // --- The interaction leaves no synchronous-write backlog behind (the same
    //     invariant every other sink-driven path in this harness holds). ---
    {
        RedstoneCircuit circuit;
        circuit.solid({0, -1, 0}).comparator(kDiode, Direction::East, /*subtract=*/false);
        assert(circuit.openByHand(kDiode));
        assert(circuit.pendingSynchronousWrites() == 0U);
    }

    return 0;
}

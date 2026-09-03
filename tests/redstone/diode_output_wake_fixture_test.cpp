#include "redstone/RedstoneHarness.hpp"

// W-x-1: DiodeBlock#updateNeighborsInFront (DiodeBlock.java:177-183).
//
// A diode's own write goes out with flags 2, which fans out nothing, so vanilla
// wakes its output side explicitly — and it is *base-class* behaviour, shared by
// the repeater and the comparator, not something one of them does specially.
// This build had neither step for the repeater and only half of one for the
// comparator, so nothing downstream of any diode was ever woken:
//
//   * a trapdoor hung on a repeater's face never moved;
//   * `lever -> wire -> repeater -> wire -> trapdoor` left the second wire at 0
//     for good, i.e. every multi-stage circuit with a diode in the middle was
//     cut in half.
//
// The two steps are separate concerns here on purpose. Step (2) alone —
// `updateNeighborsAt(front)`, which notifies the six neighbours *of* the front
// cell but never the front cell itself — is what the comparator already had, and
// a wire chain can be revived by it through a side path. A trapdoor is the probe
// wherever step (1) is what is under test: a pure sink, nothing else in the
// circuit can open it, so it cannot be rescued by another route.

namespace {

using namespace mc::test::redstone;
using mc::gameplay::redstone::Direction;
using mc::world::BlockPos;

void require(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "diode output wake: %s\n", what);
        std::abort();
    }
}

void ground(RedstoneCircuit& circuit, int fromX, int toX) {
    for (int x = fromX; x <= toX; ++x) {
        circuit.solid({x, -1, 0});
    }
}

const BlockPos kSink{0, 0, 0};

} // namespace

int main() {
    // --- (1) A sink hung directly on a repeater's output face. This is the step
    // that did not exist at all: updateNeighborsAt(front) cannot reach the front
    // cell, only its neighbours. ---
    {
        RedstoneCircuit circuit;
        ground(circuit, 0, 2);
        circuit.solid({2, 0, -1});
        circuit.repeater({1, 0, 0}, Direction::East, 1)
            .lever({2, 0, 0}, Direction::South, false)
            .trapdoor(kSink, Direction::North);
        const BlockPos lever{2, 0, 0};

        require(!circuit.state(kSink).open(), "starts shut");
        circuit.setLever(lever, true);
        circuit.advance(6);
        require(circuit.lit({1, 0, 0}), "the repeater should have turned on");
        require(circuit.state(kSink).open(),
                "a trapdoor on the repeater's own output face must open");

        circuit.setLever(lever, false);
        circuit.advance(6);
        require(!circuit.state(kSink).open(), "and shut again when the repeater turns off");
    }

    // --- (2) A wire one cell past the diode, then the sink. This is the chain
    // the field report caught: wire0 read 15, the repeater read on, wire2 stayed
    // 0 for the whole run. ---
    {
        RedstoneCircuit circuit;
        ground(circuit, 0, 3);
        circuit.solid({3, 0, -1});
        circuit.trapdoor(kSink, Direction::North)
            .wire({1, 0, 0})
            .repeater({2, 0, 0}, Direction::East, 1)
            .lever({3, 0, 0}, Direction::South, false);
        const BlockPos lever{3, 0, 0};
        const BlockPos wire{1, 0, 0};

        circuit.setLever(lever, true);
        circuit.advance(8);
        require(circuit.lit({2, 0, 0}), "the repeater turned on");
        require(circuit.power(wire) > 0, "the wire past the repeater must carry its output");
        require(circuit.state(kSink).open(), "and the sink past the wire must react");

        circuit.setLever(lever, false);
        circuit.advance(8);
        require(circuit.power(wire) == 0, "the wire falls with the repeater");
        require(!circuit.state(kSink).open(), "and so does the sink");
    }

    // --- (3) Two repeaters in series, the ordinary way to extend a line. The
    // second is fed by the first's output face, so it needs step (1) as much as
    // the trapdoor did. ---
    {
        RedstoneCircuit circuit;
        ground(circuit, 0, 3);
        circuit.solid({3, 0, -1});
        circuit.trapdoor(kSink, Direction::North)
            .repeater({1, 0, 0}, Direction::East, 1)
            .repeater({2, 0, 0}, Direction::East, 1)
            .lever({3, 0, 0}, Direction::South, false);
        const BlockPos lever{3, 0, 0};

        circuit.setLever(lever, true);
        circuit.advance(10);
        require(circuit.lit({2, 0, 0}), "the first repeater is on");
        require(circuit.lit({1, 0, 0}), "the second repeater must be woken by the first");
        require(circuit.state(kSink).open(), "and the sink past both of them");

        circuit.setLever(lever, false);
        circuit.advance(10);
        require(!circuit.lit({1, 0, 0}), "the chain falls all the way back");
        require(!circuit.state(kSink).open(), "and the sink closes");
    }

    // --- (4) The comparator, on the same bench. The old code claimed this was a
    // comparator-only behaviour and still had only half of it. ---
    {
        RedstoneCircuit circuit;
        ground(circuit, 0, 2);
        circuit.solid({2, 0, -1});
        circuit.comparator({1, 0, 0}, Direction::East, /*subtract=*/false)
            .lever({2, 0, 0}, Direction::South, false)
            .trapdoor(kSink, Direction::North);
        const BlockPos lever{2, 0, 0};

        circuit.setLever(lever, true);
        circuit.advance(6);
        require(circuit.lit({1, 0, 0}), "the comparator should have turned on");
        require(circuit.state(kSink).open(),
                "a trapdoor on the comparator's output face must open too");
        circuit.setLever(lever, false);
        circuit.advance(6);
        require(!circuit.state(kSink).open(), "and shut again");
    }

    // --- (5) A door and a fence gate on the same face, so the wake reaches every
    // openable sink rather than only the one that happened to be tested. ---
    {
        const BlockPos lever{2, 0, 0};
        RedstoneCircuit circuit;
        ground(circuit, 0, 2);
        circuit.solid({2, 0, -1});
        circuit.repeater({1, 0, 0}, Direction::East, 1)
            .lever(lever, Direction::South, false)
            .door(kSink, Direction::North);
        circuit.setLever(lever, true);
        circuit.advance(6);
        require(circuit.state(kSink).open() && circuit.state({0, 1, 0}).open(),
                "both halves of a door on a repeater's face must open");

        RedstoneCircuit gateCircuit;
        ground(gateCircuit, 0, 2);
        gateCircuit.solid({2, 0, -1});
        gateCircuit.repeater({1, 0, 0}, Direction::East, 1)
            .lever(lever, Direction::South, false)
            .fenceGate(kSink, Direction::North);
        gateCircuit.setLever(lever, true);
        gateCircuit.advance(6);
        require(gateCircuit.state(kSink).open(), "a fence gate on a repeater's face must open");
    }

    // --- (6) The other two entries. JE reaches updateNeighborsInFront from
    // onPlace and affectNeighborsAfterRemoval as well as from the tick (via
    // LevelChunk.setBlockState, which runs onPlace even for a flags-2 write), so
    // patching only the tick branch would leave a diode that is placed into, or
    // broken out of, a live circuit silently stale. ---
    {
        const BlockPos lever{3, 0, 0};
        const BlockPos diode{2, 0, 0};
        const BlockPos wire{1, 0, 0};

        // Breaking a powered repeater must reset everything past it, including
        // the wire one step beyond its face — that cell is not a neighbour of
        // the broken block, so it is step (2) that has to carry it.
        RedstoneCircuit broken;
        ground(broken, 0, 3);
        broken.solid({3, 0, -1});
        broken.trapdoor(kSink, Direction::North)
            .wire(wire)
            .repeater(diode, Direction::East, 1)
            .lever(lever, Direction::South, false);
        broken.setLever(lever, true);
        broken.advance(8);
        require(broken.state(kSink).open(), "the chain is live before the break");
        broken.breakBlock(diode);
        broken.advance(8);
        require(broken.power(wire) == 0, "the wire past a broken repeater must fall");
        require(!broken.state(kSink).open(), "and the sink with it");

        // Placing a diode that is already on must wake what it now feeds. It is
        // placed POWERED outright rather than left to turn itself on, because a
        // diode placed into a live line does not currently start at all — the
        // newly placed cell is never notified about itself, which is a separate
        // gap on the *input* side and is registered as such (see this node's
        // landing record). Writing POWERED directly is what isolates the entry
        // under test: nothing here ticks the diode, so the sink can only have
        // been woken by onPlace.
        RedstoneCircuit placed;
        ground(placed, 0, 3);
        placed.solid({3, 0, -1});
        placed.trapdoor(kSink, Direction::North).wire(wire).lever(lever, Direction::South, false);
        // The lever is on, so the arriving diode is genuinely correct to be
        // powered and stays that way; nothing about the gap in the note above is
        // being relied on, only sidestepped.
        placed.setLever(lever, true);
        placed.advance(4);
        require(!placed.state(kSink).open(), "the sink is untouched with no diode in the line");
        placed.placeBlock(diode, mc::world::BlockState{mc::world::Block::Repeater,
                                                        orientationOf(Direction::East)}
                                     .withPowered(true));
        placed.advance(8);
        require(placed.power(wire) > 0, "the wire in front of the placed diode must light");
        require(placed.state(kSink).open(), "and the sink past it must be woken by onPlace");
    }

    // --- (7) The step the place/remove entry is actually *for*. Everything above
    // could be carried by the ordinary six-neighbour fan-out a place or a break
    // already does, because the diode's front cell is adjacent to the diode. Step
    // (2) reaches one cell further: the front cell's own neighbours. Put a solid
    // block on the diode's face and a torch on that block — the torch is two
    // cells from the diode and is a neighbour of nobody who changed, so only
    // updateNeighborsInFront can reach it.
    //
    // This is the ordinary "repeater into a block, torch on top" inverter, not a
    // contrived shape. ---
    {
        const BlockPos lever{3, 0, 0};
        const BlockPos diode{2, 0, 0};
        const BlockPos base{1, 0, 0};   // the diode's output face
        const BlockPos torch{1, 1, 0};  // on top of it, two cells from the diode

        const auto build = [&](RedstoneCircuit& circuit, bool withDiode) {
            ground(circuit, 0, 3);
            circuit.solid({3, 0, -1});
            circuit.solid(base).torch(torch, /*lit=*/true).lever(lever, Direction::South, false);
            if (withDiode) {
                circuit.repeater(diode, Direction::East, 1);
            }
        };

        // Breaking a powered diode must relight the torch it had been holding out.
        RedstoneCircuit broken;
        build(broken, /*withDiode=*/true);
        broken.setLever(lever, true);
        broken.advance(10);
        require(!broken.lit(torch), "the powered repeater holds the torch out");
        broken.breakBlock(diode);
        broken.advance(10);
        require(broken.lit(torch),
                "breaking the diode must relight the torch two cells past it — the only route "
                "there is updateNeighborsInFront's second step");

        // ...and placing one already on must put it back out.
        RedstoneCircuit placedDiode;
        build(placedDiode, /*withDiode=*/false);
        placedDiode.setLever(lever, true);
        placedDiode.advance(10);
        require(placedDiode.lit(torch), "the torch is lit with no diode feeding the block");
        placedDiode.placeBlock(diode, mc::world::BlockState{mc::world::Block::Repeater,
                                                             orientationOf(Direction::East)}
                                          .withPowered(true));
        placedDiode.advance(10);
        require(!placedDiode.lit(torch), "placing a powered diode must put the torch out");
    }

    return 0;
}

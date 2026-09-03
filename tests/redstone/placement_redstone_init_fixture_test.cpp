#include "redstone/RedstoneHarness.hpp"

// W-9: placement-time redstone initialisation for the three blocks that had
// none — redstone dust, the redstone lamp, and (from the roster scan that
// closed the task) the piston.
//
//   RedStoneWireBlock.java:296-304   onPlace: if (!oldState.is(state.getBlock())
//                                    && !level.isClientSide()) updatePowerStrength(...)
//   RedstoneLampBlock.java:31-33     getStateForPlacement:
//                                    LIT = level.hasNeighborSignal(clickedPos)
//   PistonBaseBlock.java:75-78       setPlacedBy: this.checkIfExtend(...)
//
// Both land dark and *stay* dark: nothing around them is going to change, so the
// placement is the only chance either had to read the signal it is sitting in.
//
// Two disciplines this fixture is built around, both learned the hard way:
//
//  1. It must go through the real item path. RedstoneHarness's wire()/place()/
//     placeBlock() all call WorldMutationService directly and never run
//     ItemPlacement, so they cannot see this class of defect at all. `placeByItem`
//     is the only helper here that sends a UseItemOn.
//
//  2. Only the cell touching the source can show it. Put dust beside an existing
//     wire and the placement's own six-neighbour fan-out re-solves the whole
//     island, new cell included — it looks fixed. Every bench below therefore
//     brings the source to a settled, asserted state *first* and then places one
//     block and touches nothing else.
//
// And each block is checked on both edges: landing lit is half the story, since
// POWERED/LIT/signal are also the sink's edge memory. A block that lands with a
// wrong record of the signal mis-reads the *next* change too, so every case here
// cuts the power afterwards and requires it to go dark.

namespace {

using namespace mc::test::redstone;
using mc::world::Block;
using mc::world::BlockPos;

void require(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "placement redstone init: %s\n", what);
        std::abort();
    }
}

const BlockPos kSource{0, 0, 0}; // a block of redstone, the constant source
const BlockPos kTarget{1, 0, 0}; // the cell the block under test is placed into

// The dust's own POWER, read off the cell rather than through the harness's
// `power` probe: that probe falls back to getBestNeighborSignal for a cell with
// no AnalogSignal property, which for an *empty* cell beside the source reads 15
// and would quietly pass an assertion meant to prove the cell is not carrying
// anything yet. What this defect is about is the value stored in the dust.
[[nodiscard]] int dustPower(const RedstoneCircuit& circuit, BlockPos rel) {
    const auto pos = circuit.absoluteOf(rel);
    const auto state = circuit.worldRef().state(pos.x, pos.y, pos.z);
    return state.block() == Block::RedstoneWire ? state.analogSignal() : -1;
}

// Floor only. The source is added per-case so a bench can also be built dead.
void bench(RedstoneCircuit& circuit) {
    for (int x = -1; x <= 3; ++x) {
        circuit.solid({x, -1, 0});
    }
}

} // namespace

int main() {
    // --- 1A: dust placed against a live source. The defect: it stayed at 0
    // forever. ---
    {
        RedstoneCircuit circuit;
        bench(circuit);
        circuit.redstoneBlock(kSource);
        circuit.advance(4);
        require(circuit.worldRef().block(circuit.absoluteOf(kTarget).x,
                                        circuit.absoluteOf(kTarget).y,
                                        circuit.absoluteOf(kTarget).z) == Block::Air,
                "the target cell starts empty");

        require(circuit.placeByItem(kTarget, Block::RedstoneWire), "the dust must place at all");
        // Nothing else is touched from here on — that is the whole test.
        circuit.advance(20);
        require(dustPower(circuit, kTarget) == 15,
                "dust placed against a block of redstone must light up");

        // The other edge: POWER is also the record the next change is read
        // against, so a cell that landed with the wrong one mis-reads the fall
        // as well. Cut the source and it must go dark.
        circuit.breakBlock(kSource);
        circuit.advance(8);
        require(dustPower(circuit, kTarget) == 0, "and go dark when the source is cut");
    }

    // --- 1B: the control. The same dust, present before the source arrives, has
    // always worked — so 1A is about placement, not about the wire model. ---
    {
        RedstoneCircuit circuit;
        bench(circuit);
        circuit.wire(kTarget);
        circuit.advance(4);
        require(dustPower(circuit, kTarget) == 0, "dust with no source is dark");
        circuit.redstoneBlock(kSource);
        circuit.advance(8);
        require(dustPower(circuit, kTarget) == 15,
                "and lights when the source turns up beside it");
    }

    // --- 1C: why it went unnoticed. Placed into a dead line it is correctly
    // dark; energise afterwards and it catches up, because the source's own
    // fan-out wakes it. Any nearby edit heals the bug. ---
    {
        RedstoneCircuit circuit;
        bench(circuit);
        require(circuit.placeByItem(kTarget, Block::RedstoneWire), "the dust must place at all");
        circuit.advance(4);
        require(dustPower(circuit, kTarget) == 0,
                "placed into a dead cell it stays dark, correctly");
        circuit.redstoneBlock(kSource);
        circuit.advance(8);
        require(dustPower(circuit, kTarget) == 15, "and follows the source once it is live");
    }

    // --- 2A: the lamp placed against a live source. ---
    {
        RedstoneCircuit circuit;
        bench(circuit);
        circuit.redstoneBlock(kSource);
        circuit.advance(4);

        require(circuit.placeByItem(kTarget, Block::RedstoneLamp), "the lamp must place at all");
        circuit.advance(20);
        require(circuit.lit(kTarget), "a lamp placed against a block of redstone must be lit");

        // RedstoneLampBlock#neighborChanged schedules the *off* four gameticks
        // out (the on edge is immediate); 8 is comfortably past that.
        circuit.breakBlock(kSource);
        circuit.advance(8);
        require(!circuit.lit(kTarget), "and go out when the source is cut");
    }

    // --- 2B: the control, as for the dust. ---
    {
        RedstoneCircuit circuit;
        bench(circuit);
        circuit.place(kTarget, mc::world::BlockState{Block::RedstoneLamp});
        circuit.advance(4);
        require(!circuit.lit(kTarget), "a lamp with no source is dark");
        circuit.redstoneBlock(kSource);
        circuit.advance(8);
        require(circuit.lit(kTarget), "and lights when the source turns up beside it");
        circuit.breakBlock(kSource);
        circuit.advance(8);
        require(!circuit.lit(kTarget), "and goes out again when it leaves");
    }

    // --- 2C: placed into a dead cell it is dark and stays dark on its own. ---
    {
        RedstoneCircuit circuit;
        bench(circuit);
        require(circuit.placeByItem(kTarget, Block::RedstoneLamp), "the lamp must place at all");
        circuit.advance(20);
        require(!circuit.lit(kTarget), "placed into a dead cell it stays dark, correctly");
        circuit.redstoneBlock(kSource);
        circuit.advance(8);
        require(circuit.lit(kTarget), "and follows the source once it is live");
    }

    // --- 2D: the lamp's timing, per gametick against RedstoneLampBlock. ON is
    // immediate — the lamp is already lit before a single tick has run, because
    // neighborChanged writes it outright. OFF is scheduled four gameticks out,
    // so the lamp is still lit through each of the three before it. Driven with
    // the direct helpers rather than placeByItem, which runs session ticks of
    // its own and would blur the count. ---
    {
        RedstoneCircuit circuit;
        bench(circuit);
        circuit.place(kTarget, mc::world::BlockState{Block::RedstoneLamp});
        circuit.advance(2);
        require(!circuit.lit(kTarget), "dark to begin with");

        circuit.redstoneBlock(kSource);
        require(circuit.lit(kTarget),
                "the lamp lights in the same update, with no gametick in between");

        circuit.breakBlock(kSource);
        for (int gt = 1; gt <= 3; ++gt) {
            circuit.advance(1);
            require(circuit.lit(kTarget), "and stays lit for three gameticks after the cut");
        }
        circuit.advance(1);
        require(!circuit.lit(kTarget), "going out on the fourth");
    }

    // --- 2E: the point of the four-tick delay. The source leaves and is back
    // inside the window; RedstoneLampBlock#tick re-reads the signal when it
    // fires rather than trusting the schedule, so the lamp never blinks. ---
    {
        RedstoneCircuit circuit;
        bench(circuit);
        circuit.place(kTarget, mc::world::BlockState{Block::RedstoneLamp});
        circuit.redstoneBlock(kSource);
        circuit.advance(4);
        require(circuit.lit(kTarget), "lit to begin with");

        circuit.breakBlock(kSource);
        circuit.advance(2);
        circuit.redstoneBlock(kSource);
        for (int gt = 0; gt < 8; ++gt) {
            circuit.advance(1);
            require(circuit.lit(kTarget),
                    "a source back inside the window must never blink the lamp");
        }
    }

    // --- 2F: written is not the same as seen. The lamp's two edges leave by
    // different doors — ON is a synchronous sink write inside a neighbour
    // notification, OFF is a scheduled tick — and each has its own publish path.
    // A write that reaches neither is a lamp that is lit in the world and dark on
    // screen, the failure the door's two halves already taught this codebase, so
    // both edges are checked against the path they actually take.
    {
        RedstoneCircuit circuit;
        bench(circuit);
        circuit.place(kTarget, mc::world::BlockState{Block::RedstoneLamp});
        circuit.advance(2);
        circuit.recordWorldEdits();
        circuit.clearPublishedEdits();

        circuit.redstoneBlock(kSource);
        circuit.advance(1);
        require(circuit.lit(kTarget), "lit by the source");
        require(circuit.publishedEdit(kTarget),
                "the ON edge must be published, not just written");

        // The OFF edge leaves by the other door: it is a scheduled tick, so it
        // rides the tick's BlockChange list (what GameSession turns into
        // WorldEditEvents) rather than the synchronous publish. Asserted on that
        // list for the same reason — a change missing from it is a change the
        // client never sees.
        circuit.clearTickedCells();
        circuit.breakBlock(kSource);
        circuit.advance(6);
        require(!circuit.lit(kTarget), "and put out again");
        require(circuit.tickedCell(kTarget),
                "the OFF edge must reach the tick's change list, not just the world");
    }

    // --- 3: the third block the W-9 scan turned up. PistonBaseBlock#setPlacedBy
    // (PistonBaseBlock.java:75-78) runs the same checkIfExtend its
    // neighborChanged does, so a piston placed into a powered region extends
    // immediately. Here it stayed retracted. Same defect, same slot, and the
    // same two edges: it must also retract when the source goes. ---
    {
        RedstoneCircuit circuit;
        bench(circuit);
        circuit.redstoneBlock(kSource);
        circuit.advance(4);

        require(circuit.placeByItem(kTarget, Block::Piston), "the piston must place at all");
        circuit.advance(20);
        require(circuit.lit(kTarget), "a piston placed in a powered region must extend");

        circuit.breakBlock(kSource);
        circuit.advance(8);
        require(!circuit.lit(kTarget), "and retract when the source is cut");
    }

    // --- 3B: the control — the same piston, present before the source, has
    // always worked. ---
    {
        RedstoneCircuit circuit;
        bench(circuit);
        circuit.piston(kTarget, mc::gameplay::redstone::Direction::East);
        circuit.advance(4);
        require(!circuit.lit(kTarget), "a piston with no source is retracted");
        circuit.redstoneBlock(kSource);
        circuit.advance(8);
        require(circuit.lit(kTarget), "and extends when the source turns up beside it");
    }

    return 0;
}

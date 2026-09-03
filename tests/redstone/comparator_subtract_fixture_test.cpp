#include "redstone/RedstoneHarness.hpp"

// W-4a fixture: the comparator, scenarios A and B of
// redstone-reference/fixtures/comparator-subtract.md, run gametick-by-gametick
// through the real runtime. A block of redstone feeds the back (15); a block of
// redstone toggled on the side (0<->15) drives the compare/subtract behaviour.
// The probe is the comparator's own POWERED; the discriminator is what happens
// when side == back: COMPARE stays on (passes the back through), SUBTRACT drops
// to 0. Delay is a fixed 2gt.

namespace {

using namespace mc::test::redstone;
using mc::gameplay::redstone::Direction;
using mc::world::BlockPos;

// (0,-1,0) Stone       floor
// (0, 0,0) Comparator  facing East (back input = (1,0,0))
// (1, 0,0) RedstoneBlock  the back source, a constant 15
// side input toggled at (0,0,1) (one of the perpendicular sides).
void bench(RedstoneCircuit& circuit, bool subtract) {
    circuit.solid({0, -1, 0})
        .comparator({0, 0, 0}, Direction::East, subtract)
        .redstoneBlock({1, 0, 0});
}

const BlockPos kComparator{0, 0, 0};
const BlockPos kSide{0, 0, 1};

} // namespace

int main() {
    // --- Scenario A: SUBTRACT, back 15, side toggled 0 -> 15 -> 0. Output is
    //     15 - side, so a full side input drops it to 0. ---
    {
        RedstoneCircuit circuit;
        bench(circuit, /*subtract=*/true);
        FixtureScript script;
        script.probes = {Probe{kComparator, Probe::Lit}};
        script.events = {
            {10, [](RedstoneCircuit& c) { c.redstoneBlock(kSide); }}, // side -> 15
            {20, [](RedstoneCircuit& c) { c.clear(kSide); }},         // side -> 0
        };
        // gt0-1: not yet settled; the back source's edge lands at gt2.
        script.expected = {{0, {0}}, {2, {1}}, {3, {1}}, {10, {1}}, {12, {0}},
                           {13, {0}}, {20, {0}}, {22, {1}}};
        runFixture(circuit, script);
    }

    // --- Scenario B: COMPARE, back 15, side -> 15. With side == back, COMPARE
    //     passes the back through, so POWERED stays on — the opposite of A. ---
    {
        RedstoneCircuit circuit;
        bench(circuit, /*subtract=*/false);
        FixtureScript script;
        script.probes = {Probe{kComparator, Probe::Lit}};
        script.events = {{10, [](RedstoneCircuit& c) { c.redstoneBlock(kSide); }}};
        script.expected = {{0, {0}}, {2, {1}}, {10, {1}}, {12, {1}}, {13, {1}}};
        runFixture(circuit, script);
    }

    // --- AR-B4-6: a comparator reading a container, end to end. Not just the
    // conversion function — the whole chain a player would build, chest into
    // comparator into wire into a door, which only became testable once W-x-1
    // let a diode wake anything downstream of it at all. ---
    {
        RedstoneCircuit circuit;
        for (int x = 0; x <= 2; ++x) {
            circuit.solid({x, -1, 0});
        }
        // Chest at x=2, comparator at x=1 reading it (FACING East = its input
        // side), wire at x=0 carrying the output, door beside the wire.
        circuit.chest({2, 0, 0}, /*items=*/0)
            .comparator({1, 0, 0}, Direction::East, /*subtract=*/false)
            .wire({0, 0, 0})
            .solid({0, -1, 1})
            .door({0, 0, 1}, Direction::North);
        const BlockPos comparator{1, 0, 0};
        const BlockPos wire{0, 0, 0};
        const BlockPos door{0, 0, 1};

        circuit.advance(4);
        if (circuit.power(wire) != 0 || circuit.state(door).open()) {
            std::fprintf(stderr, "comparator container: an empty chest must drive nothing\n");
            std::abort();
        }

        // Fill it and poke the comparator the way putting an item in would.
        circuit.setChestContents({2, 0, 0}, 27 * 64);
        circuit.notifyRedstone(comparator);
        circuit.advance(8);
        if (circuit.power(wire) != 15) {
            std::fprintf(stderr, "comparator container: a full chest should read 15, got %d\n",
                         circuit.power(wire));
            std::abort();
        }
        if (!circuit.state(door).open() || !circuit.state({0, 1, 1}).open()) {
            std::fprintf(stderr, "comparator container: the door past the wire must open\n");
            std::abort();
        }

        // Halfway is 8, and the chain follows it down rather than latching.
        circuit.setChestContents({2, 0, 0}, 27 * 32);
        circuit.notifyRedstone(comparator);
        circuit.advance(8);
        if (circuit.power(wire) != 8) {
            std::fprintf(stderr, "comparator container: a half-full chest should read 8, got %d\n",
                         circuit.power(wire));
            std::abort();
        }

        // Emptying it closes the door again.
        circuit.setChestContents({2, 0, 0}, 0);
        circuit.notifyRedstone(comparator);
        circuit.advance(8);
        if (circuit.power(wire) != 0 || circuit.state(door).open()) {
            std::fprintf(stderr, "comparator container: emptying the chest must reset the chain\n");
            std::abort();
        }
    }

    return 0;
}

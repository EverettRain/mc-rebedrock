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

    return 0;
}

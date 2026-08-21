#include "redstone/RedstoneHarness.hpp"

// W-4a fixture: the observer, run through the real runtime. It watches the block
// on its FACING side (via the updateShape pass — the observer is that
// mechanism's first real consumer) and emits a fixed 2gt pulse out its back on
// any change, whether the watched block appears or disappears. The pulse length
// is edge-triggered, independent of how long the change persists.

int main() {
    using namespace mc::test::redstone;
    using mc::gameplay::redstone::Direction;
    using mc::world::BlockPos;

    RedstoneCircuit circuit;
    // Observer at origin, watching East (its watched cell is (1,0,0)).
    circuit.observer({0, 0, 0}, Direction::East);
    const BlockPos observerPos{0, 0, 0};
    const BlockPos watched{1, 0, 0};

    FixtureScript script;
    script.probes = {Probe{observerPos, Probe::Lit}}; // POWERED
    script.events = {
        {5, [watched](RedstoneCircuit& c) { c.solid(watched); }},  // a block appears
        {15, [watched](RedstoneCircuit& c) { c.clear(watched); }}, // and disappears
    };
    // Each edge -> a 2gt pulse two gameticks later (schedule at +2, on for 2gt).
    script.expected = {
        {0, {0}},  {5, {0}},  {7, {1}},  {8, {1}},  {9, {0}}, // block placed at t=5
        {15, {0}}, {17, {1}}, {18, {1}}, {19, {0}},           // block removed at t=15
    };
    runFixture(circuit, script);
    return 0;
}

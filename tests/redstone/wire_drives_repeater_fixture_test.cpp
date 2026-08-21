#include "redstone/RedstoneHarness.hpp"

// W-4a fixture: wire driving a downstream component end to end (the W-4.5b
// wire-to-downstream notification). A lever feeds a wire, and the wire feeds a
// repeater's input. Toggling the lever re-solves the wire the next gametick,
// which wakes the repeater, which turns on its own delay later — a multi-hop
// chain across three component kinds, all through the real runtime.

int main() {
    using namespace mc::test::redstone;
    using mc::gameplay::redstone::Direction;
    using mc::world::BlockPos;

    RedstoneCircuit circuit;
    // Floor for the wire and repeater; a wall for the lever.
    circuit.solid({0, -1, 0}).solid({1, -1, 0}).solid({-1, 0, -1});
    // lever(-1,0,0) -> wire(0,0,0) -> repeater(1,0,0) facing West (input = the wire).
    circuit.lever({-1, 0, 0}, Direction::South, false)
        .wire({0, 0, 0})
        .repeater({1, 0, 0}, Direction::West, 1);

    const BlockPos leverPos{-1, 0, 0};
    const BlockPos wirePos{0, 0, 0};
    const BlockPos repeaterPos{1, 0, 0};

    FixtureScript script;
    script.probes = {Probe{wirePos, Probe::Power}, Probe{repeaterPos, Probe::Lit}};
    script.events = {{5, [leverPos](RedstoneCircuit& c) { c.setLever(leverPos, true); }}};
    // t=5 lever on. t=6 the wire re-solves to 15 and wakes the repeater. The
    // repeater (delay 1 = 2gt) then turns on at t=8.
    script.expected = {
        {0, {0, 0}}, // dark
        {6, {15, 0}}, // wire powered, repeater not yet
        {7, {15, 0}},
        {8, {15, 1}}, // repeater on, 2gt after it saw the wire
    };
    runFixture(circuit, script);
    return 0;
}

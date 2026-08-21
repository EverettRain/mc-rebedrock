#include "redstone/RedstoneHarness.hpp"

// W-4a fixture: the stone button, a timed pulse source. Pressing powers it
// immediately; it releases itself a fixed 20 gameticks later, independent of any
// input. Probe is the button's own POWERED.

int main() {
    using namespace mc::test::redstone;
    using mc::gameplay::redstone::Direction;
    using mc::world::BlockPos;

    RedstoneCircuit circuit;
    // Button hung on the stone to its west (connectedDir East -> mount West).
    circuit.solid({-1, 0, 0}).button({0, 0, 0}, Direction::East);
    const BlockPos buttonPos{0, 0, 0};

    FixtureScript script;
    script.probes = {Probe{buttonPos, Probe::Lit}}; // POWERED
    script.events = {{5, [buttonPos](RedstoneCircuit& c) { c.pressButton(buttonPos); }}};
    // Pressed at t=5: powered immediately, released 20gt later at t=25.
    script.expected = {{0, {0}}, {5, {1}}, {6, {1}}, {24, {1}}, {25, {0}}};
    runFixture(circuit, script);
    return 0;
}

#include "redstone/RedstoneHarness.hpp"

// W-4a fixture: the piston's two-phase block event (the reference's
// piston-door, at the depth W-4 covers — the trigger/settle timing and the
// EXTENDED state; the actual block movement is a separate task). Phase one, on
// the redstone update, queues an extend/contract event; phase two settles it at
// the end of the tick via the BlockEventQueue (W-2's first live consumer). Probe
// is the piston's EXTENDED (stored in POWERED).
//
// The lever input is applied after the tick step here, so the event queued on
// one gametick settles at the end of the next — a faithful "block events settle
// at tick end", one tick after the input edge.

int main() {
    using namespace mc::test::redstone;
    using mc::gameplay::redstone::Direction;
    using mc::world::BlockPos;

    RedstoneCircuit circuit;
    // Piston facing up; a lever on its east side powers it.
    circuit.piston({0, 0, 0}, Direction::Up)
        .solid({1, 0, -1})                          // lever's wall
        .lever({1, 0, 0}, Direction::South, false); // powers the piston when on
    const BlockPos pistonPos{0, 0, 0};
    const BlockPos leverPos{1, 0, 0};

    FixtureScript script;
    script.probes = {Probe{pistonPos, Probe::Lit}}; // EXTENDED
    script.events = {
        {5, [leverPos](RedstoneCircuit& c) { c.setLever(leverPos, true); }},   // power -> extend
        {15, [leverPos](RedstoneCircuit& c) { c.setLever(leverPos, false); }}, // cut -> contract
    };
    script.expected = {
        {0, {0}},  {5, {0}},  {6, {1}}, // powered at t=5, event settles at t=6 tick end
        {15, {1}}, {16, {0}},           // cut at t=15, contracts at t=16
    };
    runFixture(circuit, script);
    return 0;
}

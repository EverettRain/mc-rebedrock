#include "redstone/RedstoneHarness.hpp"

// W-signal fixture: the stone pressure plate as a redstone SOURCE. Pressing it
// (setPressurePlate, standing in for an entity stepping on it) writes POWERED
// and fans out through BasePressurePlateBlock#updateNeighbours to both the
// plate's own six neighbours and the six neighbours of the block it sits on —
// this is what lets a wire/repeater the plate stands next to actually react, the
// routing PlayerInteraction.cpp's tickPressurePlates now performs (sabotage①'s
// target: a missing updateNeighborsAt call here leaves downstream dark forever).
// The plate's own weak emission is checked directly (all sides, 15 while
// pressed — sabotage②'s target: the emission table returning 0 while POWERED).

int main() {
    using namespace mc::test::redstone;
    using mc::gameplay::redstone::Direction;
    using mc::world::BlockPos;

    RedstoneCircuit circuit;
    // Floor under all three cells: the plate needs BlockSupport::Ground under
    // itself too, or breakUnsupportedBlocks pops it the first tick.
    circuit.solid({0, -1, 0}).solid({1, -1, 0}).solid({2, -1, 0});
    // plate(0,0,0) -> wire(1,0,0) -> repeater(2,0,0) facing West (input = the wire).
    circuit.pressurePlate({0, 0, 0}).wire({1, 0, 0}).repeater({2, 0, 0}, Direction::West, 1);

    const BlockPos platePos{0, 0, 0};
    const BlockPos wirePos{1, 0, 0};
    const BlockPos repeaterPos{2, 0, 0};

    FixtureScript script;
    script.probes = {Probe{platePos, Probe::Lit}, Probe{wirePos, Probe::Power},
                     Probe{repeaterPos, Probe::Lit}};
    script.events = {
        {5, [platePos](RedstoneCircuit& c) { c.setPressurePlate(platePos, true); }},
        {20, [platePos](RedstoneCircuit& c) { c.setPressurePlate(platePos, false); }},
    };
    // t=5 pressed: POWERED immediately, wire re-solves to 15 the next gametick
    // (t=6), waking the repeater (delay 1 = 2gt) which turns on at t=8.
    // t=20 released: POWERED clears immediately, wire re-solves to 0 at t=21,
    // repeater turns off 2gt later at t=23.
    script.expected = {
        {0, {0, 0, 0}},
        {5, {1, 0, 0}},  // pressed, wire/repeater not yet caught up
        {6, {1, 15, 0}}, // wire powered
        {7, {1, 15, 0}},
        {8, {1, 15, 1}}, // repeater on
        {20, {0, 15, 1}}, // released, downstream still on for a moment
        {21, {0, 0, 1}},  // wire de-powers
        {22, {0, 0, 1}},
        {23, {0, 0, 0}},  // repeater off
    };
    runFixture(circuit, script);
    return 0;
}

#include "redstone/RedstoneHarness.hpp"

// W-4a fixture: the repeater, scenarios A and B of
// redstone-reference/fixtures/repeater-delay.md, run gametick-by-gametick
// through the real runtime. A lever on the repeater's input side drives it; the
// probe is the repeater's own POWERED. Delay = DELAY*2 gt, and a pulse shorter
// than the delay still comes out full length.

namespace {

using namespace mc::test::redstone;
using mc::gameplay::redstone::Direction;
using mc::world::BlockPos;

// Builds the standard "lever -> repeater(delay)" bench:
//   (0,-1,0) Stone   floor under the repeater
//   (0, 0,0) Repeater facing East (input side = (1,0,0))
//   (1, 0,0) Lever    at the input cell, hung on the stone to its north
//   (1, 0,-1) Stone   the lever's wall
void bench(RedstoneCircuit& circuit, int delay) {
    circuit.solid({0, -1, 0})
        .solid({1, 0, -1})
        .repeater({0, 0, 0}, Direction::East, delay)
        .lever({1, 0, 0}, Direction::South, /*on=*/false);
}

const BlockPos kRepeater{0, 0, 0};
const BlockPos kLever{1, 0, 0};

} // namespace

int main() {
    // --- Scenario A, DELAY=1 (2gt): a held input; the output follows 2gt behind
    //     each edge. ---
    {
        RedstoneCircuit circuit;
        bench(circuit, 1);
        FixtureScript script;
        script.probes = {Probe{kRepeater, Probe::Lit}};
        script.events = {
            {0, [](RedstoneCircuit& c) { c.setLever(kLever, true); }},
            {5, [](RedstoneCircuit& c) { c.setLever(kLever, false); }},
        };
        script.expected = {{0, {0}}, {1, {0}}, {2, {1}}, {3, {1}}, {5, {1}}, {7, {0}}};
        runFixture(circuit, script);
    }

    // --- Scenario A, DELAY=2 (4gt): the same edge, four ticks behind, proving
    //     the delay is adjustable. ---
    {
        RedstoneCircuit circuit;
        bench(circuit, 2);
        FixtureScript script;
        script.probes = {Probe{kRepeater, Probe::Lit}};
        script.events = {{0, [](RedstoneCircuit& c) { c.setLever(kLever, true); }}};
        script.expected = {{0, {0}}, {1, {0}}, {2, {0}}, {3, {0}}, {4, {1}}};
        runFixture(circuit, script);
    }

    // --- Scenario B, DELAY=1: a 1gt input pulse still yields a full 2gt output
    //     pulse (the tick re-arms a turn-off when it turns on into a dropped
    //     input). ---
    {
        RedstoneCircuit circuit;
        bench(circuit, 1);
        FixtureScript script;
        script.probes = {Probe{kRepeater, Probe::Lit}};
        script.events = {
            {0, [](RedstoneCircuit& c) { c.setLever(kLever, true); }},
            {1, [](RedstoneCircuit& c) { c.setLever(kLever, false); }},
        };
        script.expected = {{0, {0}}, {1, {0}}, {2, {1}}, {3, {1}}, {4, {0}}};
        runFixture(circuit, script);
    }

    return 0;
}

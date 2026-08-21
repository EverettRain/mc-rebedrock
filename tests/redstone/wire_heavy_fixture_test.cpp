#include "redstone/RedstoneHarness.hpp"

#include <algorithm>

// W-4a fixture: redstone wire, scenarios A and B of
// redstone-reference/fixtures/wire-heavy.md, run through the real runtime. POWER
// attenuates one per cell, and a source change re-solves the whole network. The
// steady-state distribution is uniquely fixed by the attenuation law, so the
// expected values are generated from it rather than hand-listed. This is the
// serial evaluator that W-5's AC graph must match bit for bit.

namespace {

using namespace mc::test::redstone;
using mc::world::BlockPos;

constexpr int kLength = 17; // w0..w16

// Lay a straight run of wire on a stone floor: w_i at (i,0,0), floor beneath.
void layWire(RedstoneCircuit& circuit) {
    for (int x = -1; x < kLength; ++x) {
        circuit.solid({x, -1, 0}); // floor (also under the source cell at x=-1)
    }
    for (int x = 0; x < kLength; ++x) {
        circuit.wire({x, 0, 0});
    }
}

} // namespace

int main() {
    // --- Scenario A: a constant source at the head; steady state is 15 - i,
    //     floored at 0. The source is a block of redstone against w0. ---
    {
        RedstoneCircuit circuit;
        layWire(circuit);
        circuit.redstoneBlock({-1, 0, 0}); // constant 15 against w0

        FixtureScript script;
        // Sample a handful of cells across the run.
        const int sampled[] = {0, 4, 8, 14, 15, 16};
        std::vector<int> expected;
        for (const int x : sampled) {
            script.probes.push_back(Probe{{x, 0, 0}, Probe::Power});
            expected.push_back(std::max(0, 15 - x)); // attenuation law
        }
        // The source was placed during setup, so the network settles on the
        // first gametick; assert the steady state a tick later.
        script.expected = {{2, expected}};
        runFixture(circuit, script);
    }

    // --- Scenario B: a togglable source (lever). Off -> all zero; on -> the
    //     attenuation ramp; off -> zero again. Wire has no per-cell delay, so
    //     the whole run flips together. ---
    {
        RedstoneCircuit circuit;
        layWire(circuit);
        circuit.solid({-1, 0, -1});                                     // lever's wall
        circuit.lever({-1, 0, 0}, mc::gameplay::redstone::Direction::South, false);

        FixtureScript script;
        script.probes = {Probe{{0, 0, 0}, Probe::Power}, Probe{{1, 0, 0}, Probe::Power},
                         Probe{{2, 0, 0}, Probe::Power}};
        script.events = {
            {5, [](RedstoneCircuit& c) { c.setLever({-1, 0, 0}, true); }},
            {15, [](RedstoneCircuit& c) { c.setLever({-1, 0, 0}, false); }},
        };
        script.expected = {
            {0, {0, 0, 0}},    // lever off, wire dark
            {6, {15, 14, 13}}, // one gametick after the lever turns on
            {16, {0, 0, 0}},   // one gametick after it turns off
        };
        runFixture(circuit, script);
    }

    return 0;
}

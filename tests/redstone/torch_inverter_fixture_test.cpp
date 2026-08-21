#include "redstone/RedstoneHarness.hpp"

// W-4a fixture: the redstone-torch inverter, scenario A of
// redstone-reference/fixtures/torch-inverter.md, run gametick-by-gametick
// through the real runtime. A lever powers the block a torch stands on; the
// torch inverts its input on a fixed 2gt delay. Every value below is the
// source-derived expected LIT state, and runFixture asserts each one.

int main() {
    using namespace mc::test::redstone;
    using mc::world::BlockPos;

    RedstoneCircuit circuit;

    // Layout (relative to origin):
    //   (0,0,0) Stone            — base, the block the torch stands on
    //   (0,1,0) RedstoneTorch    — the output torch under test (lit by default)
    //   (1,0,0) Lever            — east of base, hangs on base (mount = West),
    //                              getConnectedDirection East (strongly powers
    //                              its East side, i.e. into base).
    circuit.solid({0, 0, 0})
        .torch({0, 1, 0}, /*lit=*/true)
        .lever({1, 0, 0}, mc::gameplay::redstone::Direction::East, /*on=*/false);

    // Sanity: with the lever off, the torch is lit and its input is unpowered.
    if (circuit.lit({0, 1, 0}) != true) {
        std::fprintf(stderr, "setup: torch should start lit\n");
        return 1;
    }

    FixtureScript script;
    script.probes = {Probe{{0, 1, 0}, Probe::Lit}};
    script.events = {
        {0, [](RedstoneCircuit& c) { c.setLever({1, 0, 0}, true); }},  // t=0 lever ON
        {5, [](RedstoneCircuit& c) { c.setLever({1, 0, 0}, false); }}, // t=5 lever OFF
    };
    // Source-derived (torch-inverter.md scenario A): input edge -> output flips
    // 2gt later, both directions.
    script.expected = {
        {0, {1}}, // lever ON, torch schedules its tick, still lit
        {1, {1}}, // tick not due yet
        {2, {0}}, // torch.tick: powered -> goes out
        {3, {0}}, // stable
        {5, {0}}, // lever OFF, torch schedules its tick, still out
        {7, {1}}, // torch.tick: unpowered -> relights
    };

    runFixture(circuit, script);
    return 0;
}

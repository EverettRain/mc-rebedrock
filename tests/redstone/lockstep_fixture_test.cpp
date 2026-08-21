#include "redstone/RedstoneHarness.hpp"

// W-6 (island-analysis scope): the lockstep cross-check gate. Each scenario runs
// the same circuit under the Serial redstone drain (ground truth) and the Island
// drain (partitioned, island-major reordering) and asserts every probe agrees on
// every gametick. The island drain is single-threaded here — it only reorders the
// same work into independent islands — so an agreement proves the partition is
// sound and its merge order is deterministic, and a divergence would mean a
// coupled pair was wrongly split. This is the CI door the roadmap asks for; a
// future threaded W-6 keeps exactly this gate and only changes who runs each
// island.
//
// The standard circuit set (torch inverter, repeater chain, comparator, wire
// mesh, observer) exercises single-island coupling; the two-inverter scenario
// forces the partition to actually split into >1 island so the gate is not
// passing vacuously.

namespace {

using namespace mc::test::redstone;
using mc::gameplay::redstone::Direction;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "lockstep_fixture_test line %d failed: %s\n", line, expression);
        std::abort();
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

// A torch inverter driven by a lever, toggled on and off.
void testTorchInverterLockstep() {
    const auto build = [](RedstoneCircuit& c) {
        c.solid({0, 0, 0})
            .torch({0, 1, 0}, /*lit=*/true)
            .lever({1, 0, 0}, Direction::East, /*on=*/false);
    };
    FixtureScript script;
    script.probes = {Probe{{0, 1, 0}, Probe::Lit}};
    script.events = {
        {0, [](RedstoneCircuit& c) { c.setLever({1, 0, 0}, true); }},
        {5, [](RedstoneCircuit& c) { c.setLever({1, 0, 0}, false); }},
    };
    script.expected = {{9, {}}}; // just extends the timeline to gt 9
    static_cast<void>(runLockstep(build, script));
}

// A wire run feeding a repeater chain: wire-heavy plus a diode, the coupling the
// island partition must keep in one island.
void testWireRepeaterLockstep() {
    const auto build = [](RedstoneCircuit& c) {
        c.solid({0, 0, 0})
            .solid({1, 0, 0})
            .solid({2, 0, 0})
            .solid({3, 0, 0})
            .lever({0, 1, 0}, Direction::Up, /*on=*/false)
            .wire({1, 1, 0})
            .wire({2, 1, 0})
            .repeater({3, 1, 0}, Direction::West, /*delay=*/2);
    };
    FixtureScript script;
    script.probes = {
        Probe{{1, 1, 0}, Probe::Power},
        Probe{{2, 1, 0}, Probe::Power},
        Probe{{3, 1, 0}, Probe::Lit},
    };
    script.events = {
        {0, [](RedstoneCircuit& c) { c.setLever({0, 1, 0}, true); }},
        {8, [](RedstoneCircuit& c) { c.setLever({0, 1, 0}, false); }},
    };
    script.expected = {{14, {}}};
    static_cast<void>(runLockstep(build, script));
}

// A comparator in subtract mode: a back input reduced by a side input, both fed
// from redstone blocks placed and cleared over time.
void testComparatorLockstep() {
    const auto build = [](RedstoneCircuit& c) {
        c.solid({0, 0, 0})
            .solid({1, 0, 0})
            .comparator({0, 1, 0}, Direction::West, /*subtract=*/true) // back = West
            .redstoneBlock({-1, 1, 0});                                // back input source
    };
    FixtureScript script;
    script.probes = {Probe{{0, 1, 0}, Probe::Power}};
    script.events = {
        // Raise a side input, then drop it, so the comparator output changes.
        {1, [](RedstoneCircuit& c) { c.redstoneBlock({0, 1, -1}); }},
        {6, [](RedstoneCircuit& c) { c.clear({0, 1, -1}); }},
    };
    script.expected = {{11, {}}};
    static_cast<void>(runLockstep(build, script));
}

// An observer watching a block that toggles: a pulse source, single island.
void testObserverLockstep() {
    const auto build = [](RedstoneCircuit& c) {
        c.solid({0, 0, 0}).observer({0, 1, 0}, Direction::North); // watches North
    };
    FixtureScript script;
    script.probes = {Probe{{0, 1, 0}, Probe::Lit}};
    script.events = {
        {1, [](RedstoneCircuit& c) { c.solid({0, 1, -1}); }}, // place block it watches
        {6, [](RedstoneCircuit& c) { c.clear({0, 1, -1}); }},
    };
    script.expected = {{11, {}}};
    static_cast<void>(runLockstep(build, script));
}

// Two independent torch inverters far apart: the partition MUST split them into
// separate islands (their reach-2 footprints do not overlap across 20 cells), and
// the two drains must still agree bit for bit. This is the scenario that proves
// the gate is not passing on a single vacuous island.
void testTwoIslandLockstep() {
    const auto build = [](RedstoneCircuit& c) {
        c.solid({0, 0, 0})
            .torch({0, 1, 0}, /*lit=*/true)
            .lever({1, 0, 0}, Direction::East, /*on=*/false);
        c.solid({20, 0, 0})
            .torch({20, 1, 0}, /*lit=*/true)
            .lever({21, 0, 0}, Direction::East, /*on=*/false);
    };
    FixtureScript script;
    script.probes = {
        Probe{{0, 1, 0}, Probe::Lit},
        Probe{{20, 1, 0}, Probe::Lit},
    };
    script.events = {
        // Toggle both the same gametick, so both torches have a tick due together
        // and the partition sees two disjoint due components at once.
        {0,
         [](RedstoneCircuit& c) {
             c.setLever({1, 0, 0}, true);
             c.setLever({21, 0, 0}, true);
         }},
        {5,
         [](RedstoneCircuit& c) {
             c.setLever({1, 0, 0}, false);
             c.setLever({21, 0, 0}, false);
         }},
    };
    script.expected = {{9, {}}};
    const std::size_t maxIslands = runLockstep(build, script);
    REQUIRE(maxIslands >= 2);
}

} // namespace

int main() {
    testTorchInverterLockstep();
    testWireRepeaterLockstep();
    testComparatorLockstep();
    testObserverLockstep();
    testTwoIslandLockstep();
    std::puts("lockstep_fixture_test: serial == island, bit for bit, every scenario");
    return 0;
}

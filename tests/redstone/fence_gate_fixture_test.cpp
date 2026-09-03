#include "redstone/RedstoneHarness.hpp"

#include "world/BlockShape.hpp"

// AR-B4-3 fixture: the fence gate as a redstone SINK. Like the door, it had no
// redstone behaviour at all before this node — the sink was one hardcoded
// `block == Block::OakTrapdoor`. FenceGateBlock#neighborChanged is the plainest
// of the three (single cell, no partner half):
//
//   hasPower = hasNeighborSignal(pos)
//   if (POWERED != hasPower) setBlock(POWERED=hasPower, OPEN=hasPower, flags 2)
//
// Same-gametick, like the trapdoor: the lever's own updateNeighbours call
// reaches the gate inside the tick the lever flips, not one later.

int main() {
    using namespace mc::test::redstone;
    using mc::gameplay::redstone::Direction;
    using mc::world::BlockPos;

    RedstoneCircuit circuit;
    circuit.solid({-1, 0, -1});
    circuit.lever({-1, 0, 0}, Direction::South, false).fenceGate({0, 0, 0}, Direction::North);

    const BlockPos leverPos{-1, 0, 0};
    const BlockPos gatePos{0, 0, 0};

    FixtureScript script;
    script.probes = {Probe{gatePos, Probe::Lit}}; // POWERED, written with OPEN
    script.events = {
        {5, [leverPos](RedstoneCircuit& c) { c.setLever(leverPos, true); }},
        {10, [leverPos](RedstoneCircuit& c) { c.setLever(leverPos, false); }},
    };
    script.expected = {
        {0, {0}},
        {4, {0}},
        {5, {1}}, // lever on -> gate swings open the same gametick
        {6, {1}},
        {9, {1}},
        {10, {0}}, // lever off -> gate shuts the same gametick
        {11, {0}},
    };
    runFixture(circuit, script);

    // OPEN itself, not just the POWERED the probe aliases.
    if (circuit.state(gatePos).open()) {
        std::fprintf(stderr, "fence gate fixture: expected OPEN=false after lever off\n");
        std::abort();
    }
    circuit.setLever(leverPos, true);
    if (!circuit.state(gatePos).open() || !circuit.state(gatePos).powered()) {
        std::fprintf(stderr, "fence gate fixture: expected OPEN+POWERED after lever on\n");
        std::abort();
    }

    // AR-B4-1 rides along: a gate opened by redstone is genuinely passable —
    // collisionShape empties on OPEN, so the 1.5-cell closed box is gone. This
    // is the mechanism and the collision box agreeing, checked once so the two
    // nodes cannot drift apart.
    if (!mc::world::collisionShape(circuit.state(gatePos)).boxes.empty()) {
        std::fprintf(stderr, "fence gate fixture: a redstone-opened gate still collides\n");
        std::abort();
    }
    circuit.setLever(leverPos, false);
    if (mc::world::collisionShape(circuit.state(gatePos)).boxes.empty()) {
        std::fprintf(stderr, "fence gate fixture: a redstone-closed gate stopped colliding\n");
        std::abort();
    }

    // The gate's FACING is untouched by a redstone toggle — only a player's
    // right-click re-faces a gate (FenceGateBlock#useWithoutItem), never
    // neighborChanged.
    if (circuit.state(gatePos).orientation() != mc::world::BlockOrientation::North) {
        std::fprintf(stderr, "fence gate fixture: redstone re-faced the gate\n");
        std::abort();
    }

    // A sink, never a source.
    using mc::gameplay::redstone::isSignalSource;
    static_assert(!isSignalSource(mc::world::Block::OakFenceGate));

    return 0;
}

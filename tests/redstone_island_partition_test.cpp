// W-6 (island-analysis scope): the pure IslandPartitioner's contract, tested
// from synthetic footprints with no World in sight. It must group ticks that
// share a footprint cell into one island (transitively), keep ticks with
// disjoint footprints apart, collapse everything to one island when any tick
// declares global coupling, and emit a deterministic order — islands by their
// minimum (chunkPos, packed pos) member, ticks within an island in JE drain
// order — regardless of the order ticks were registered in. That determinism is
// what makes the island drain a stable reordering the lockstep gate can pin.

#include "gameplay/ChunkTickScheduler.hpp"
#include "gameplay/RedstoneIslandPartition.hpp"
#include "gameplay/SimulationPosition.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "redstone_island_partition_test line %d failed: %s\n", line, expression);
        std::abort();
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using mc::gameplay::ScheduledTick;
using mc::gameplay::SimulationPosition;
using mc::gameplay::TickPriority;
using mc::gameplay::redstone::IslandPartitioner;

[[nodiscard]] ScheduledTick tickAt(int x, int y, int z, std::uint64_t sequence,
                                   TickPriority priority = TickPriority::Normal) {
    return ScheduledTick{{x, y, z}, /*dueTick=*/10U, priority, sequence};
}

// A tick's footprint as an explicit cell list (the planner would generate these
// from a ball or a wire flood; here they are handed in directly).
void addTick(IslandPartitioner& partitioner, const ScheduledTick& tick,
             const std::vector<std::int64_t>& cells, bool global = false) {
    const std::size_t handle = partitioner.addTick(tick);
    if (global) {
        partitioner.markGlobal(handle);
    }
    for (const std::int64_t cell : cells) {
        partitioner.addFootprintCell(handle, cell);
    }
}

// Disjoint footprints stay separate islands; shared cells merge; sharing is
// transitive.
void testGrouping() {
    IslandPartitioner partitioner;

    // A and B share cell 100; C is alone on cell 300.
    partitioner.reset();
    addTick(partitioner, tickAt(0, 64, 0, 0), {100, 101});
    addTick(partitioner, tickAt(1, 64, 0, 1), {100, 102});
    addTick(partitioner, tickAt(40, 64, 0, 2), {300, 301});
    std::vector<ScheduledTick> plan;
    std::vector<std::size_t> offsets;
    partitioner.finalize(plan, offsets);
    REQUIRE(partitioner.islandCount() == 2);
    REQUIRE(plan.size() == 3);
    REQUIRE(offsets.size() == 2);

    // Transitive chain A-B-C (A∩B on 10, B∩C on 20, A and C disjoint) is one
    // island.
    partitioner.reset();
    addTick(partitioner, tickAt(0, 64, 0, 0), {10, 11});
    addTick(partitioner, tickAt(0, 64, 1, 1), {10, 20});
    addTick(partitioner, tickAt(0, 64, 2, 2), {20, 21});
    partitioner.finalize(plan, offsets);
    REQUIRE(partitioner.islandCount() == 1);
    REQUIRE(offsets.size() == 1);
    REQUIRE(plan.size() == 3);
}

// Global coupling collapses the whole due set to one island even when footprints
// are disjoint — the conservative "cannot prove independent" fallback.
void testGlobalCoupling() {
    IslandPartitioner partitioner;
    partitioner.reset();
    addTick(partitioner, tickAt(0, 64, 0, 0), {1});
    addTick(partitioner, tickAt(40, 64, 0, 1), {2}, /*global=*/true);
    addTick(partitioner, tickAt(80, 64, 0, 2), {3});
    std::vector<ScheduledTick> plan;
    std::vector<std::size_t> offsets;
    partitioner.finalize(plan, offsets);
    REQUIRE(partitioner.islandCount() == 1);
    REQUIRE(offsets.size() == 1);
    REQUIRE(plan.size() == 3);
}

// The order is a deterministic function of the circuit, not registration order:
// islands sorted by min (chunkPos, packed pos), ticks within an island in drain
// order. Registered scrambled, the plan must come out canonical.
void testDeterministicOrder() {
    IslandPartitioner partitioner;
    partitioner.reset();
    // Island X: two cells sharing 5, at chunk (2,0) — x=32,33. Registered with the
    // higher position first, and given sequences (pos33=7, pos32=9) that put the
    // higher position EARLIER in drain order, so the two order rules can be told
    // apart: between islands is (chunkPos, packed pos), but within an island it is
    // JE drain order (sequence here), not packed pos.
    addTick(partitioner, tickAt(33, 64, 0, 7), {5, 6});
    addTick(partitioner, tickAt(32, 64, 0, 9), {5, 4});
    // Island Y: single cell 900, at chunk (0,0) — x=1. Sorts first (lower chunk).
    addTick(partitioner, tickAt(1, 64, 0, 3), {900});
    std::vector<ScheduledTick> plan;
    std::vector<std::size_t> offsets;
    partitioner.finalize(plan, offsets);
    REQUIRE(partitioner.islandCount() == 2);
    REQUIRE(offsets.size() == 2);
    // Island Y (chunk 0) comes before island X (chunk 2): the between-island key is
    // the minimum (chunkPos, packed pos) member, independent of hash iteration.
    REQUIRE(offsets[0] == 0);
    REQUIRE(plan[0].position == (SimulationPosition{1, 64, 0}));
    // Island X starts at offset 1; within it, drain order (sequence 7 before 9)
    // puts pos33 before pos32 — the opposite of packed-pos order, proving the
    // in-island tie-break is drain order, not position.
    REQUIRE(offsets[1] == 1);
    REQUIRE(plan[1].position == (SimulationPosition{33, 64, 0}));
    REQUIRE(plan[2].position == (SimulationPosition{32, 64, 0}));
}

// Within one island, the tie-break is JE drain order: priority before sequence.
void testDrainOrderWithinIsland() {
    IslandPartitioner partitioner;
    partitioner.reset();
    // Three ticks all sharing cell 1 (one island). Registered NORMAL/seq0 first,
    // then HIGH/seq1: HIGH must drain first despite the later sequence.
    addTick(partitioner, tickAt(0, 64, 0, 0, TickPriority::Normal), {1});
    addTick(partitioner, tickAt(1, 64, 0, 1, TickPriority::High), {1});
    addTick(partitioner, tickAt(2, 64, 0, 2, TickPriority::Normal), {1});
    std::vector<ScheduledTick> plan;
    std::vector<std::size_t> offsets;
    partitioner.finalize(plan, offsets);
    REQUIRE(partitioner.islandCount() == 1);
    REQUIRE(plan.size() == 3);
    REQUIRE(plan[0].priority == TickPriority::High); // -1 sorts before 0
    REQUIRE(plan[1].sequence == 0);
    REQUIRE(plan[2].sequence == 2);
}

// A warm partitioner reused across ticks must not leak the previous partition:
// reset clears the cell ownership so an unrelated later tick is not merged
// through a stale cell.
void testResetClears() {
    IslandPartitioner partitioner;
    std::vector<ScheduledTick> plan;
    std::vector<std::size_t> offsets;

    partitioner.reset();
    addTick(partitioner, tickAt(0, 64, 0, 0), {42});
    partitioner.finalize(plan, offsets);
    REQUIRE(partitioner.islandCount() == 1);

    // Reuse: a single tick on the same cell 42 must be its own island, not merged
    // with the (now gone) previous tick.
    partitioner.reset();
    addTick(partitioner, tickAt(50, 64, 0, 0), {42});
    partitioner.finalize(plan, offsets);
    REQUIRE(partitioner.islandCount() == 1);
    REQUIRE(plan.size() == 1);
    REQUIRE(plan[0].position == (SimulationPosition{50, 64, 0}));
}

} // namespace

int main() {
    testGrouping();
    testGlobalCoupling();
    testDeterministicOrder();
    testDrainOrderWithinIsland();
    testResetClears();
    std::puts("redstone_island_partition_test: all scenarios passed");
    return 0;
}

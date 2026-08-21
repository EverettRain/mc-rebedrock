#include "gameplay/BlockEventQueue.hpp"
#include "gameplay/ChunkTickScheduler.hpp"
#include "persistence/ScheduledTickCodec.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

// W-2 acceptance: the scheduler orders ticks by JE's (dueTick, priority,
// subTickOrder); a scheduled tick serialises to JE's SavedTick with a *relative*
// delay and round-trips losslessly; and block events settle at tick end in a
// deterministic FIFO, so a piston's two phases resolve in Java's order.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "tick_priority_and_events_test line %d failed: %s\n", line, expression);
        std::abort();
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using mc::gameplay::BlockEvent;
using mc::gameplay::BlockEventQueue;
using mc::gameplay::ChunkTickScheduler;
using mc::gameplay::SavedTick;
using mc::gameplay::SimulationPosition;
using mc::gameplay::TickPriority;
using mc::gameplay::TickTask;

[[nodiscard]] std::vector<SimulationPosition> drain(ChunkTickScheduler& scheduler, TickTask task,
                                                    std::uint64_t now, std::size_t budget) {
    std::vector<SimulationPosition> seen;
    scheduler.drainDue(task, now, budget,
                       [&](SimulationPosition position) { seen.push_back(position); });
    return seen;
}

// --- Same game tick, different priority: HIGH-ish fires before NORMAL, and ties
// break by insertion FIFO. Insertion order is deliberately scrambled against
// priority order so a scheduler that ignored priority would come back wrong. ---
void testPriorityOrdering() {
    ChunkTickScheduler scheduler;
    scheduler.schedule(TickTask::Fluid, {1, 0, 0}, 10U, false, TickPriority::Normal);        // seq0
    scheduler.schedule(TickTask::Fluid, {2, 0, 0}, 10U, false, TickPriority::ExtremelyHigh); // seq1
    scheduler.schedule(TickTask::Fluid, {3, 0, 0}, 10U, false, TickPriority::High);          // seq2
    scheduler.schedule(TickTask::Fluid, {4, 0, 0}, 10U, false, TickPriority::Normal);        // seq3

    const auto seen = drain(scheduler, TickTask::Fluid, 10U, 16U);
    REQUIRE(seen.size() == 4U);
    REQUIRE(seen[0].x == 2); // ExtremelyHigh
    REQUIRE(seen[1].x == 3); // High
    REQUIRE(seen[2].x == 1); // Normal, seq0 — FIFO within priority
    REQUIRE(seen[3].x == 4); // Normal, seq3
}

// --- Earlier dueTick still wins over priority: the total order is
// (dueTick, priority, subTickOrder), dueTick first. ---
void testDueTickBeforePriority() {
    ChunkTickScheduler scheduler;
    scheduler.schedule(TickTask::Fluid, {1, 0, 0}, 10U, false, TickPriority::ExtremelyHigh);
    scheduler.schedule(TickTask::Fluid, {2, 0, 0}, 8U, false, TickPriority::ExtremelyLow);
    // Both overdue at now=10; the one due at 8 comes first despite lower priority.
    const auto seen = drain(scheduler, TickTask::Fluid, 10U, 16U);
    REQUIRE(seen.size() == 2U);
    REQUIRE(seen[0].x == 2);
    REQUIRE(seen[1].x == 1);
}

// --- A scheduled tick serialises to JE's SavedTick: `delay` is relative to game
// time, and the whole thing round-trips through bytes and back into a scheduler
// at a *different* game time with its remaining wait intact. ---
void testSavedTickRoundTrip() {
    ChunkTickScheduler source;
    constexpr std::uint64_t gameTime = 1000U;
    source.schedule(TickTask::Fluid, {3, 3, 3}, gameTime + 2U);                          // delay 2
    source.schedule(TickTask::Fluid, {1, 1, 1}, gameTime + 5U, false, TickPriority::Low);
    source.schedule(TickTask::SupportCheck, {2, 2, 2}, gameTime + 5U, false, TickPriority::High);

    const std::vector<SavedTick> saved = source.exportSavedTicks(gameTime);
    REQUIRE(saved.size() == 3U);
    // Delays are RELATIVE, not absolute triggerTicks (2, not 1002).
    for (const SavedTick& tick : saved) {
        if (tick.position == SimulationPosition{3, 3, 3}) {
            REQUIRE(tick.delay == 2);
        } else {
            REQUIRE(tick.delay == 5);
        }
    }

    // Byte round-trip is exact.
    std::vector<std::uint8_t> bytes;
    mc::persistence::appendSavedTicks(bytes, saved);
    std::size_t cursor = 0U;
    const std::vector<SavedTick> decoded = mc::persistence::readSavedTicks(bytes, cursor);
    REQUIRE(cursor == bytes.size());
    REQUIRE(decoded == saved);

    // Imported at a different game time, the relative delay becomes a new
    // absolute dueTick: a delay-2 tick imported at 5000 is due at 5002 — not
    // before, and then it fires.
    ChunkTickScheduler target;
    constexpr std::uint64_t laterTime = 5000U;
    target.importSavedTicks(laterTime, decoded);

    // Re-exporting at the import time reproduces the exact saved set (same order,
    // same relative delays, same priorities) — lossless. Checked before draining,
    // which would consume the ticks.
    REQUIRE(target.exportSavedTicks(laterTime) == saved);

    // And the relative delay became a new absolute dueTick: a delay-2 tick
    // imported at 5000 is due at 5002 — not before, and then it fires.
    REQUIRE(drain(target, TickTask::Fluid, laterTime + 1U, 16U).empty());
    const auto dueAt5002 = drain(target, TickTask::Fluid, laterTime + 2U, 16U);
    REQUIRE(dueAt5002.size() == 1U);
    REQUIRE(dueAt5002[0] == (SimulationPosition{3, 3, 3}));
}

// --- Per-chunk export mirrors JE saving ticks with their chunk. ---
void testPerChunkExport() {
    ChunkTickScheduler scheduler;
    scheduler.schedule(TickTask::Fluid, {2, 10, 2}, 5U);   // chunk (0,0)
    scheduler.schedule(TickTask::Fluid, {40, 10, 2}, 5U);  // chunk (2,0)
    REQUIRE(scheduler.exportSavedTicks(0, 0, 0U).size() == 1U);
    REQUIRE(scheduler.exportSavedTicks(2, 0, 0U).size() == 1U);
    REQUIRE(scheduler.exportSavedTicks(0U).size() == 2U);
}

// --- Block events settle at tick end in a FIFO, and a follow-up an event raises
// settles in the same drain, after the events already queued. This is the piston
// two-phase order: both pistons extend, then both moves resolve. ---
void testBlockEventTwoPhase() {
    enum Action : std::int32_t { Extend = 0, Moved = 1 };
    constexpr std::uint16_t kPiston = 7U;

    BlockEventQueue queue;
    // Phase 1: two pistons, in world order, decide to extend.
    REQUIRE(queue.queue({{0, 0, 0}, kPiston, Extend, 0}));
    REQUIRE(queue.queue({{10, 0, 0}, kPiston, Extend, 0}));

    std::vector<BlockEvent> settled;
    // Phase 2 (tick end): settle. An extending piston, as it fires, wakes the
    // cell it pushed — a follow-up event queued for the same drain.
    const std::size_t count = queue.drain([&](BlockEvent event) {
        settled.push_back(event);
        if (event.param0 == Extend) {
            queue.queue({{event.position.x + 1, event.position.y, event.position.z}, kPiston, Moved,
                         0});
        }
    });

    REQUIRE(count == 4U);
    // Java's FIFO: both extends first, then both moves — not depth-first.
    REQUIRE(settled[0] == (BlockEvent{{0, 0, 0}, kPiston, Extend, 0}));
    REQUIRE(settled[1] == (BlockEvent{{10, 0, 0}, kPiston, Extend, 0}));
    REQUIRE(settled[2] == (BlockEvent{{1, 0, 0}, kPiston, Moved, 0}));
    REQUIRE(settled[3] == (BlockEvent{{11, 0, 0}, kPiston, Moved, 0}));
    REQUIRE(queue.empty());
}

// --- Exact duplicates collapse (Java's linked hash set), first order kept. ---
void testBlockEventDedup() {
    BlockEventQueue queue;
    REQUIRE(queue.queue({{0, 0, 0}, 1U, 0, 0}));
    REQUIRE(!queue.queue({{0, 0, 0}, 1U, 0, 0})); // same event: dropped
    REQUIRE(queue.queue({{0, 0, 0}, 1U, 5, 0}));  // different params: distinct
    REQUIRE(queue.size() == 2U);

    std::vector<std::int32_t> params;
    queue.drain([&](BlockEvent event) { params.push_back(event.param0); });
    REQUIRE(params.size() == 2U);
    REQUIRE(params[0] == 0 && params[1] == 5);
}

} // namespace

int main() {
    testPriorityOrdering();
    testDueTickBeforePriority();
    testSavedTickRoundTrip();
    testPerChunkExport();
    testBlockEventTwoPhase();
    testBlockEventDedup();
    return 0;
}

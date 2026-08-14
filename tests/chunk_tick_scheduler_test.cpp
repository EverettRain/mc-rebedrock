#include "gameplay/ChunkTickScheduler.hpp"

#include <stdexcept>
#include <string>
#include <vector>

// Scheduled ticks are stored per chunk so they can be dropped when the chunk
// unloads and saved with it. What has to survive that change is the *order* the
// work runs in: falling blocks and fluid spread are order-sensitive, and
// bucketing by chunk would otherwise let a chunk that happens to hash first run
// work queued later than another chunk's.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"chunk_tick_scheduler_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using mc::gameplay::ChunkTickScheduler;
using mc::gameplay::SimulationPosition;
using mc::gameplay::TickTask;

[[nodiscard]] std::vector<SimulationPosition> drain(ChunkTickScheduler& scheduler, TickTask task,
                                                    std::uint64_t now, std::size_t budget) {
    std::vector<SimulationPosition> seen;
    scheduler.drainDue(task, now, budget,
                       [&](SimulationPosition position) { seen.push_back(position); });
    return seen;
}

} // namespace

int main() {
    // --- Insertion order is global, across chunks. These positions land in
    // four different chunks, interleaved, and must come back in the order they
    // were scheduled — not grouped by chunk. ---
    {
        ChunkTickScheduler scheduler;
        const SimulationPosition order[] = {
            {2, 10, 2}, {40, 10, 2}, {2, 10, 40}, {-20, 10, -20}, {5, 10, 5},
        };
        for (const auto position : order) {
            REQUIRE(scheduler.schedule(TickTask::Fluid, position, 0U));
        }
        const auto seen = drain(scheduler, TickTask::Fluid, 0U, 16U);
        REQUIRE(seen.size() == std::size(order));
        for (std::size_t index = 0; index < seen.size(); ++index) {
            REQUIRE(seen[index] == order[index]);
        }
        REQUIRE(scheduler.pending(TickTask::Fluid) == 0U);
    }

    // --- A budget stops the drain, and what is left keeps its order. ---
    {
        ChunkTickScheduler scheduler;
        for (int index = 0; index < 6; ++index) {
            REQUIRE(scheduler.schedule(TickTask::SupportCheck, {index * 20, 5, 0}, 0U));
        }
        const auto first = drain(scheduler, TickTask::SupportCheck, 0U, 2U);
        REQUIRE(first.size() == 2U);
        REQUIRE(first[0].x == 0 && first[1].x == 20);
        REQUIRE(scheduler.pending(TickTask::SupportCheck) == 4U);
        const auto rest = drain(scheduler, TickTask::SupportCheck, 0U, 16U);
        REQUIRE(rest.size() == 4U);
        REQUIRE(rest[0].x == 40);
    }

    // --- Entries not yet due stay put. ---
    {
        ChunkTickScheduler scheduler;
        REQUIRE(scheduler.schedule(TickTask::LeafDecay, {1, 1, 1}, 10U));
        REQUIRE(drain(scheduler, TickTask::LeafDecay, 9U, 16U).empty());
        REQUIRE(scheduler.pending(TickTask::LeafDecay) == 1U);
        REQUIRE(drain(scheduler, TickTask::LeafDecay, 10U, 16U).size() == 1U);
    }

    // --- De-duplication, and the opt-out falling blocks rely on. ---
    {
        ChunkTickScheduler scheduler;
        REQUIRE(scheduler.schedule(TickTask::Fluid, {1, 1, 1}, 0U));
        REQUIRE(!scheduler.schedule(TickTask::Fluid, {1, 1, 1}, 0U));
        REQUIRE(scheduler.pending(TickTask::Fluid) == 1U);
        // The same cell in a different task is a different entry.
        REQUIRE(scheduler.schedule(TickTask::SupportCheck, {1, 1, 1}, 0U));
        REQUIRE(scheduler.contains(TickTask::SupportCheck, {1, 1, 1}));
        // Falling blocks kept the flat deque's behaviour: two entries, two
        // units of work, each re-checked against the world when it fires.
        REQUIRE(scheduler.schedule(TickTask::FallingBlock, {2, 2, 2}, 0U, true));
        REQUIRE(scheduler.schedule(TickTask::FallingBlock, {2, 2, 2}, 0U, true));
        REQUIRE(scheduler.pending(TickTask::FallingBlock) == 2U);
        REQUIRE(drain(scheduler, TickTask::FallingBlock, 0U, 16U).size() == 2U);
    }

    // --- Work a handler schedules as immediately due runs in the same drain.
    // Support checks depend on this: popping an unsupported block strands the
    // next one along, and that cascade has to resolve within the tick. ---
    {
        ChunkTickScheduler scheduler;
        REQUIRE(scheduler.schedule(TickTask::SupportCheck, {0, 10, 0}, 0U));
        std::vector<int> seen;
        scheduler.drainDue(TickTask::SupportCheck, 0U, 16U, [&](SimulationPosition position) {
            seen.push_back(position.y);
            if (position.y > 6) {
                // The block below just lost its support.
                static_cast<void>(
                    scheduler.schedule(TickTask::SupportCheck, {0, position.y - 1, 0}, 0U));
            }
        });
        REQUIRE(seen.size() == 5U); // y = 10, 9, 8, 7, 6
        REQUIRE(seen.front() == 10 && seen.back() == 6);
    }

    // --- The reason this is keyed by chunk: unloading. An entry used to
    // outlive its chunk and fire against cells that were no longer loaded. ---
    {
        ChunkTickScheduler scheduler;
        REQUIRE(scheduler.schedule(TickTask::Fluid, {2, 10, 2}, 0U));      // chunk (0,0)
        REQUIRE(scheduler.schedule(TickTask::LeafDecay, {5, 20, 5}, 0U));  // chunk (0,0)
        REQUIRE(scheduler.schedule(TickTask::Fluid, {40, 10, 2}, 0U));     // chunk (2,0)
        REQUIRE(scheduler.pending(TickTask::Fluid) == 2U);

        scheduler.forgetChunk(0, 0);
        // Everything in that chunk is gone, whatever the task...
        REQUIRE(scheduler.pending(TickTask::Fluid) == 1U);
        REQUIRE(scheduler.pending(TickTask::LeafDecay) == 0U);
        // ...and the surviving chunk's work is untouched.
        const auto seen = drain(scheduler, TickTask::Fluid, 0U, 16U);
        REQUIRE(seen.size() == 1U);
        REQUIRE(seen[0].x == 40);
    }

    // --- Negative coordinates must bucket the way the world does, or a chunk
    // at -1 never matches the key forgetChunk erases. ---
    {
        ChunkTickScheduler scheduler;
        REQUIRE(scheduler.schedule(TickTask::Fluid, {-1, 10, -1}, 0U));
        scheduler.forgetChunk(0, 0);
        REQUIRE(scheduler.pending(TickTask::Fluid) == 1U); // not in chunk (0,0)
        scheduler.forgetChunk(-1, -1);
        REQUIRE(scheduler.pending(TickTask::Fluid) == 0U);
    }

    // --- Every chunk holding work is enumerable, which is what persistence
    // and the unload sweep iterate. ---
    {
        ChunkTickScheduler scheduler;
        REQUIRE(scheduler.schedule(TickTask::Fluid, {2, 10, 2}, 0U));
        REQUIRE(scheduler.schedule(TickTask::Fluid, {40, 10, 40}, 0U));
        REQUIRE(scheduler.scheduledChunks().size() == 2U);
        scheduler.clear();
        REQUIRE(scheduler.scheduledChunks().empty());
        REQUIRE(scheduler.pending(TickTask::Fluid) == 0U);
    }

    return 0;
}

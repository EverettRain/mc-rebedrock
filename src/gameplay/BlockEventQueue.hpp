#pragma once

// Block events, collected through a tick and settled at its end — the C++ shape
// of Java's `Level.blockEvent` / `runBlockEvents`.
//
// Some block reactions cannot resolve the instant they are triggered: a piston
// that decides to extend must first let the tick finish deciding everything else,
// then move its blocks in a second, deterministic pass, or two pistons pushing
// into each other would settle in whatever order they happened to tick. Java
// handles that with a queue drained once at the end of the tick; this is the
// same contract — collect during the tick, drain in a fixed FIFO order at the
// end — kept determinism-first so a piston door behaves identically every tick.
//
// There is no piston or note block content yet, so nothing queues events at
// runtime; this is the settled ordering mechanism they will plug into (W-4+),
// tested against the two-phase order Java produces.

#include "gameplay/SimulationPosition.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

namespace mc::gameplay {

// One pending block event, a one-for-one map of Java's `BlockEventData`
// (pos, block, paramA, paramB). `type` names what kind of event it is (which
// block/behaviour raised it); the two params carry its arguments — for a piston,
// the action (extend/retract/drop) and the facing.
struct BlockEvent final {
    SimulationPosition position;
    std::uint16_t type = 0U;
    std::int32_t param0 = 0;
    std::int32_t param1 = 0;

    [[nodiscard]] bool operator==(const BlockEvent&) const = default;
};

class BlockEventQueue final {
  public:
    // Collect an event raised during this tick. Deduplicated on the whole event
    // against what is already pending — Java's ObjectLinkedOpenHashSet — so a
    // block that raises the same event twice settles once. Returns whether it
    // was newly queued. First-insertion order is preserved.
    bool queue(BlockEvent event) {
        for (const BlockEvent& pending : events_) {
            if (pending == event) {
                return false;
            }
        }
        events_.push_back(event);
        return true;
    }

    // Settle every pending event in insertion order. An event is removed before
    // its handler runs, so a handler may re-raise it or queue a follow-up; those
    // follow-ups are appended and settled in the same drain (Java's
    // runBlockEvents loops until the queue empties), which is what lets a
    // piston's second phase resolve this tick. Returns how many were settled.
    template <typename Handler>
    std::size_t drain(Handler&& handler) {
        std::size_t settled = 0U;
        while (!events_.empty()) {
            const BlockEvent event = events_.front();
            events_.pop_front();
            handler(event);
            ++settled;
        }
        return settled;
    }

    [[nodiscard]] bool empty() const { return events_.empty(); }
    [[nodiscard]] std::size_t size() const { return events_.size(); }
    void clear() { events_.clear(); }

  private:
    // Insertion-ordered FIFO: the front is the next to settle, new events go to
    // the back. Volume is tiny (a handful of pistons a tick), so the linear
    // dedup scan in queue() costs nothing worth a hash set.
    std::deque<BlockEvent> events_;
};

} // namespace mc::gameplay

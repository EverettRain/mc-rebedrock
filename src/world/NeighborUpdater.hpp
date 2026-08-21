#pragma once

#include "world/BlockPos.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace mc::world {

// The order the six orthogonal neighbours of a changed cell are notified in,
// bit-for-bit Java Edition's NeighborUpdater.UPDATE_ORDER
// {WEST, EAST, DOWN, UP, NORTH, SOUTH}. Redstone is sensitive to this sequence,
// so it is fixed, not incidental to a loop's write order.
inline constexpr std::array<BlockPos, 6> kNeighborUpdateOrder{{
    {-1, 0, 0}, // WEST
    {1, 0, 0},  // EAST
    {0, -1, 0}, // DOWN
    {0, 1, 0},  // UP
    {0, 0, -1}, // NORTH
    {0, 0, 1},  // SOUTH
}};

// The order a changed cell's neighbours recompute their *shape* in, bit-for-bit
// Java Edition's BlockBehaviour.UPDATE_SHAPE_ORDER
// {WEST, EAST, NORTH, SOUTH, DOWN, UP}. Deliberately distinct from the neighbour
// order above: JE runs the shape pass (updateNeighbourShapes) and the reaction
// pass (updateNeighborsAt) with different fixed orders, and a fence corner or a
// stair join derived from neighbours is sensitive to which one is used.
inline constexpr std::array<BlockPos, 6> kShapeUpdateOrder{{
    {-1, 0, 0}, // WEST
    {1, 0, 0},  // EAST
    {0, 0, -1}, // NORTH
    {0, 0, 1},  // SOUTH
    {0, -1, 0}, // DOWN
    {0, 1, 0},  // UP
}};

// The C++ equivalent of Java's CollectingNeighborUpdater: neighbour reactions
// run through a queue drained iteratively, never by deep recursion.
//
// Why a queue rather than a plain six-way loop: when a cell changes, its six
// neighbours are told, and any of them may in turn change a cell whose
// neighbours must be told, and so on. Doing that with recursion makes the C++
// call stack as deep as the update chain — a redstone update storm would blow
// it. Collecting instead means the stack depth stays O(1) no matter how long
// the chain, and a single counter (`count_`, capped at `updateLimit`) makes a
// pathological self-feeding topology terminate rather than spin forever.
//
// The drain order is exact: within one source's fan-out the six directions run
// in kNeighborUpdateOrder, and a reaction that queues further updates has them
// run to completion before the next direction of the original source — the same
// depth-first-per-source order Java's runUpdates produces, so the observable
// sequence of notifications matches JE.
//
// Zero allocation on the steady path: records are plain-old-data {packed pos,
// cursor} kept in two vectors reused across drains (capacity retained), and the
// position is a packed int64 (BlockPos.asLong encoding), not a heap object.
class NeighborUpdater final {
  public:
    // Notify the six neighbours of `source` that it changed, draining the whole
    // resulting chain before returning — unless a drain is already in progress
    // (a reaction re-entered here), in which case the work is queued onto the
    // running drain and processed in order. `updateLimit` caps the total number
    // of chained neighbour updates; < 0 means unbounded.
    //
    // `notify(neighbor, source)` is invoked once per neighbour, in
    // kNeighborUpdateOrder. It is a template so the call inlines with no virtual
    // or std::function indirection on the hot path.
    template <class Notify>
    void updateNeighborsAt(BlockPos source, int updateLimit, Notify&& notify) {
        addAndRun(packBlockPos(source), updateLimit, notify);
    }

    // True while a drain is running: callers can tell an ordinary edit from one
    // reacting inside the neighbour chain.
    [[nodiscard]] bool draining() const { return count_ > 0; }

  private:
    // One source's pending fan-out: which cell changed and how many of the six
    // directions have already been notified.
    struct Fanout final {
        std::int64_t source = 0;
        std::uint8_t cursor = 0;
    };

    // Notify the next un-notified direction of `pending`. Returns whether any
    // direction still remains — the caller keeps the record on the stack while
    // it does. Mirrors CollectingNeighborUpdater.MultiNeighborUpdate.runNext.
    template <class Notify>
    bool runNext(Fanout& pending, Notify& notify) {
        const BlockPos source = unpackBlockPos(pending.source);
        const BlockPos& offset = kNeighborUpdateOrder[pending.cursor++];
        notify(BlockPos{source.x + offset.x, source.y + offset.y, source.z + offset.z}, source);
        return pending.cursor < kNeighborUpdateOrder.size();
    }

    template <class Notify>
    void addAndRun(std::int64_t source, int updateLimit, Notify& notify) {
        const bool runningAlready = count_ > 0;
        const bool tooManyUpdates = updateLimit >= 0 && count_ >= updateLimit;
        ++count_;
        if (!tooManyUpdates) {
            if (runningAlready) {
                addedThisLayer_.push_back({source, 0});
            } else {
                stack_.push_back({source, 0});
            }
        }
        // The first (outermost) call owns the drain; re-entrant calls have just
        // queued their work above and return so the outer loop picks it up.
        if (!runningAlready) {
            runUpdates(notify);
        }
    }

    template <class Notify>
    void runUpdates(Notify& notify) {
        // Guarantee the updater is left empty even if a notify throws, so a
        // later drain does not resume a half-finished chain.
        struct Reset final {
            NeighborUpdater* self;
            ~Reset() {
                self->stack_.clear();
                self->addedThisLayer_.clear();
                self->count_ = 0;
            }
        } reset{this};

        while (!stack_.empty() || !addedThisLayer_.empty()) {
            // Move this layer's reactions onto the stack in reverse, so they pop
            // in the order they were queued (depth-first, source order).
            for (std::size_t i = addedThisLayer_.size(); i-- > 0;) {
                stack_.push_back(addedThisLayer_[i]);
            }
            addedThisLayer_.clear();

            // Run the top record direction by direction until it is exhausted or
            // a reaction queues a new layer — which must run before the rest of
            // this record's directions.
            while (addedThisLayer_.empty()) {
                if (!runNext(stack_.back(), notify)) {
                    stack_.pop_back();
                    break;
                }
            }
        }
    }

    // Reused across drains: cleared, never shrunk, so a steady stream of edits
    // allocates nothing after the first storm sizes them.
    std::vector<Fanout> stack_;
    std::vector<Fanout> addedThisLayer_;
    int count_ = 0;
};

} // namespace mc::world

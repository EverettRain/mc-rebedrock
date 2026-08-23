// CS-2: headless unit tests for the strict ring-ordered section delivery
// queue (src/render/SectionDeliveryQueue.hpp). Pure container logic, no
// world/render/GPU dependency — see the header's dependency-light rationale.

#include "render/SectionDeliveryQueue.hpp"

#include <cassert>
#include <cstddef>
#include <optional>

namespace {

struct Key final {
    int chunkX = 0;
    int chunkZ = 0;

    [[nodiscard]] bool operator==(const Key&) const = default;
};

struct KeyHash final {
    [[nodiscard]] std::size_t operator()(const Key& key) const noexcept {
        return std::hash<int>{}(key.chunkX) * 31U + std::hash<int>{}(key.chunkZ);
    }
};

using Queue = mc::render::SectionDeliveryQueue<Key, KeyHash>;

} // namespace

int main() {
    // ---- Strict ring outward expansion: pushed out of order, drained in
    // ring order. This is the core CS-2 property README problem② asks for:
    // "严格环序交付" independent of arrival/insertion order. ----
    {
        Queue queue;
        // Insert deliberately out of ring order (mimics batches completing
        // out of order): ring 3, ring 0, ring 2, ring 1, ring 0 (second
        // entry at the same ring).
        queue.push({3, 0}, 3, false);
        queue.push({0, 0}, 0, false);
        queue.push({2, 0}, 2, false);
        queue.push({1, 0}, 1, false);
        queue.push({0, 1}, 0, false);
        assert(queue.size() == 5U);

        // Drain order must be strictly non-decreasing ring: 0, 0, 1, 2, 3.
        // Within ring 0, arrival order is preserved: {0,0} before {0,1}.
        const Key first = queue.front();
        assert((first == Key{0, 0}));
        queue.popFront();
        const Key second = queue.front();
        assert((second == Key{0, 1}));
        queue.popFront();
        const Key third = queue.front();
        assert((third == Key{1, 0}));
        queue.popFront();
        const Key fourth = queue.front();
        assert((fourth == Key{2, 0}));
        queue.popFront();
        const Key fifth = queue.front();
        assert((fifth == Key{3, 0}));
        queue.popFront();
        assert(queue.empty());
    }

    // ---- Batch-internal order preserved within a ring (no reordering of a
    // single ring's own arrival order). ----
    {
        Queue queue;
        queue.push({5, 5}, 4, false);
        queue.push({6, 5}, 4, false);
        queue.push({5, 6}, 4, false);
        assert((queue.front() == Key{5, 5}));
        queue.popFront();
        assert((queue.front() == Key{6, 5}));
        queue.popFront();
        assert((queue.front() == Key{5, 6}));
        queue.popFront();
        assert(queue.empty());
    }

    // ---- Never cross-ring batching: draining ring N fully never yields a
    // ring N+1 entry before ring N is exhausted, even when far more entries
    // are queued at outer rings than inner ones. ----
    {
        Queue queue;
        for (int i = 0; i < 50; ++i) {
            queue.push({100 + i, 0}, 7, false);
        }
        queue.push({0, 0}, 0, false);
        queue.push({1, 0}, 1, false);
        assert((queue.front() == Key{0, 0}));
        queue.popFront();
        assert((queue.front() == Key{1, 0}));
        queue.popFront();
        // Every remaining entry must be the ring-7 batch; none of them
        // jumped ahead of the (now exhausted) inner rings, and the ring-7
        // entries themselves keep arrival order.
        for (int i = 0; i < 50; ++i) {
            assert((queue.front() == Key{100 + i, 0}));
            queue.popFront();
        }
        assert(queue.empty());
    }

    // ---- Priority lane: highPriority sections jump straight to the front,
    // ahead of every ring bucket including ring 0, and are exempt from the
    // farthest-ring eviction policy. ----
    {
        Queue queue;
        queue.push({0, 0}, 0, false);
        queue.push({9, 9}, 1, true); // edit, arrives after the ring-0 push
        assert((queue.front() == Key{9, 9}));
        queue.popFront();
        assert((queue.front() == Key{0, 0}));
        queue.popFront();
        assert(queue.empty());
    }

    // ---- Backlog eviction takes from the farthest ring, never starving
    // near-centre entries even when they arrived after the far ones (the
    // exact failure mode the real trace showed: 8 centre chunks stuck
    // behind a big far-ring backlog). ----
    {
        Queue queue;
        queue.push({20, 20}, 6, false); // far, arrived first
        queue.push({0, 0}, 0, false);   // near, arrived second
        queue.push({15, 15}, 5, false); // far
        const auto victim = queue.evictFarthest();
        assert(victim.has_value());
        assert((*victim == Key{20, 20})); // farthest ring (6), not FIFO-oldest
        assert(queue.contains(Key{0, 0}));
        assert(queue.contains(Key{15, 15}));
        assert(!queue.contains(Key{20, 20}));
        const auto secondVictim = queue.evictFarthest();
        assert(secondVictim.has_value());
        assert((*secondVictim == Key{15, 15}));
        assert(queue.contains(Key{0, 0}));
    }

    // ---- Priority entries are exempt from evictFarthest: with only a
    // priority-lane entry queued, eviction finds nothing to take. ----
    {
        Queue queue;
        queue.push({0, 0}, 0, true);
        const auto victim = queue.evictFarthest();
        assert(!victim.has_value());
        assert(queue.size() == 1U);
    }

    // ---- erase() removes a queued key from whichever bucket holds it
    // (ring or priority), without disturbing the rest of the queue's
    // order. ----
    {
        Queue queue;
        queue.push({0, 0}, 0, false);
        queue.push({1, 0}, 1, false);
        queue.push({2, 0}, 1, false);
        queue.erase({1, 0});
        assert(!queue.contains(Key{1, 0}));
        assert(queue.size() == 2U);
        assert((queue.front() == Key{0, 0}));
        queue.popFront();
        assert((queue.front() == Key{2, 0}));
        queue.popFront();
        assert(queue.empty());
    }

    // ---- contains()/size() track pushes and pops accurately, including
    // across a clear(). ----
    {
        Queue queue;
        assert(queue.empty());
        queue.push({0, 0}, 0, false);
        queue.push({1, 1}, 2, false);
        assert(queue.size() == 2U);
        assert(queue.contains(Key{0, 0}));
        assert(queue.contains(Key{1, 1}));
        queue.clear();
        assert(queue.empty());
        assert(!queue.contains(Key{0, 0}));
    }

    // ---- Negative ring clamps to bucket 0 (defensive: a caller passing a
    // ring computed before a centre re-clamp should not crash or silently
    // create a huge sparse bucket vector). ----
    {
        Queue queue;
        queue.push({0, 0}, -3, false);
        queue.push({1, 0}, 0, false);
        assert(queue.size() == 2U);
        // Both land in bucket 0; arrival order preserved.
        assert((queue.front() == Key{0, 0}));
        queue.popFront();
        assert((queue.front() == Key{1, 0}));
        queue.popFront();
    }

    return 0;
}

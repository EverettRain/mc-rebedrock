#pragma once

// CS-2: strict ring-ordered section delivery.
//
// Evidence (docs/content-dev/CS-chunk-streaming/README.md, "真机证据与决策"):
// the ~5s streaming long tail on a real run was not missing work or a single
// frame's CPU cost — it was severe *reordering*. The pending-section backlog
// was a plain FIFO (arrival order), so far-ring sections that finished
// meshing first could sit ahead of near-centre sections in the same batch (a
// 24-chunk generation batch already spans multiple Chebyshev rings), and the
// eviction cap picked the *oldest* entry regardless of ring, which could
// (and, per the trace, did) evict/starve near-centre work behind an already
// long far-ring tail. Eight centre-adjacent chunks were observed starved to
// 4.7s while distant chunks with no priority claim landed first.
//
// This container replaces the plain deque with a small array of per-ring
// FIFO buckets:
//   - within a ring, arrival order is preserved (no reordering of a batch's
//     own internal completion order — "batch内保序");
//   - across rings, the *lowest* non-empty ring is always drained first
//     (strict centre-out delivery — "近中心优先" / "不跨环成批" at the
//     consumer side, independent of how coarse the producer's batch was);
//   - the eviction policy for the backlog cap now takes from the *farthest*
//     non-empty ring, so a full backlog sheds distant work, never centre
//     work — the direct fix for the 4.7s starve.
//   - a separate priority lane is unchanged from the pre-CS-2 behaviour:
//     gameplay edits still jump straight to the front of the whole queue,
//     ahead of every ring bucket (edits are not part of the ring-fill
//     picture at all).
//
// DOD shape: ring bucket count is small and bounded (loadRadius +
// unload-hysteresis, on the order of ten), so `evictFarthest`'s reverse scan
// for the first non-empty bucket is a handful of empty-check iterations, not
// a hot-path cost; push/pop/erase touch only their own bucket, O(1)
// amortised, same deque node cost the old single-deque backlog already paid.
// No per-call heap allocation beyond what std::deque itself does.
//
// Dependency-light like ChunkStreamingTrace.hpp: templated on the caller's
// key/hash types instead of depending on world::SectionPosition directly, so
// this header (and its test) never pulls in ChunkStreamer.cpp's generation/
// meshing/persistence graph.

#include <cstddef>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

namespace mc::render {

template <typename Key, typename KeyHash>
class SectionDeliveryQueue final {
  public:
    // Inserts `key` at ring `ring` (negative rings clamp to 0).
    // `highPriority` sections (gameplay edits) go to a priority lane that is
    // always drained before any ring bucket, matching the pre-CS-2
    // push_front-to-front behaviour for edits. Callers are expected to check
    // `contains(key)` first (same discipline the old
    // `!pendingSectionUpdates.contains(update.position)` guard already
    // enforced) — pushing a key that is already queued would desync
    // `location_` from the bucket it actually lives in.
    void push(const Key& key, int ring, bool highPriority) {
        if (highPriority) {
            priority_.push_back(key);
            location_.insert_or_assign(key, -1);
            return;
        }
        const std::size_t bucket = bucketFor(ring);
        ensureBucket(bucket);
        rings_[bucket].push_back(key);
        location_.insert_or_assign(key, static_cast<int>(bucket));
    }

    [[nodiscard]] bool contains(const Key& key) const { return location_.contains(key); }

    [[nodiscard]] std::size_t size() const { return location_.size(); }

    [[nodiscard]] bool empty() const { return location_.empty(); }

    // Next key to deliver: priority lane first (FIFO), then the lowest
    // non-empty ring bucket (FIFO within the ring). Caller must check
    // `empty()` first (mirrors the old `!pendingSectionOrder.empty()`
    // while-loop guard at call sites).
    [[nodiscard]] const Key& front() const {
        if (!priority_.empty()) {
            return priority_.front();
        }
        return rings_[lowestNonEmptyRing()].front();
    }

    void popFront() {
        if (!priority_.empty()) {
            location_.erase(priority_.front());
            priority_.pop_front();
            return;
        }
        auto& bucket = rings_[lowestNonEmptyRing()];
        location_.erase(bucket.front());
        bucket.pop_front();
    }

    // Evicts one entry from the farthest non-empty ring bucket (never the
    // priority lane — priority entries are not subject to the backlog cap,
    // same as before) and returns it, or nullopt if there is nothing to
    // evict. This is the CS-2 fix for the starve: the old policy evicted
    // `pendingSectionOrder.back()`, which — because the queue was pure
    // arrival-order FIFO — was frequently a near-centre section that simply
    // arrived late in a batch that also carried a lot of far-ring work ahead
    // of it in the deque. Evicting from the farthest ring instead guarantees
    // the shed work is always at least as far as anything still queued.
    [[nodiscard]] std::optional<Key> evictFarthest() {
        for (std::size_t bucket = rings_.size(); bucket-- > 0U;) {
            if (rings_[bucket].empty()) {
                continue;
            }
            const Key victim = rings_[bucket].back();
            location_.erase(victim);
            rings_[bucket].pop_back();
            return victim;
        }
        return std::nullopt;
    }

    // Removes a specific key wherever it is queued (priority lane or a ring
    // bucket), without delivering it. Used when a caller invalidates a
    // pending entry out of band (e.g. it is about to be re-inserted with
    // fresher data at a possibly different ring).
    void erase(const Key& key) {
        const auto found = location_.find(key);
        if (found == location_.end()) {
            return;
        }
        if (found->second < 0) {
            eraseFrom(priority_, key);
        } else {
            eraseFrom(rings_[static_cast<std::size_t>(found->second)], key);
        }
        location_.erase(found);
    }

    void clear() {
        priority_.clear();
        rings_.clear();
        location_.clear();
    }

  private:
    [[nodiscard]] static std::size_t bucketFor(int ring) {
        return static_cast<std::size_t>(ring < 0 ? 0 : ring);
    }

    void ensureBucket(std::size_t bucket) {
        if (bucket >= rings_.size()) {
            rings_.resize(bucket + 1U);
        }
    }

    // Scans from ring 0 upward for the first non-empty bucket. Bounded by
    // the (small) ring-bucket count; see the DOD note above the class.
    [[nodiscard]] std::size_t lowestNonEmptyRing() const {
        for (std::size_t bucket = 0U; bucket < rings_.size(); ++bucket) {
            if (!rings_[bucket].empty()) {
                return bucket;
            }
        }
        return 0U; // Unreachable when front()/popFront() are only called on
                   // a non-empty queue, per their documented precondition.
    }

    static void eraseFrom(std::deque<Key>& bucket, const Key& key) {
        for (auto it = bucket.begin(); it != bucket.end(); ++it) {
            if (*it == key) {
                bucket.erase(it);
                return;
            }
        }
    }

    std::deque<Key> priority_;
    std::vector<std::deque<Key>> rings_;
    // Which bucket (ring index) or the priority lane (-1) currently holds
    // each queued key, so erase()/popFront() do not need to linear-scan
    // every bucket to find it.
    std::unordered_map<Key, int, KeyHash> location_;
};

} // namespace mc::render

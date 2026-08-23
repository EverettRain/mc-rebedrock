// CS-1: headless coverage for the chunk-streaming diagnostic instrumentation
// (world/ChunkStreamingTrace.hpp). This is pure logic — no ChunkStreamer, no
// render thread — so it runs without MC_REBEDROCK_CHUNK_TRACE set; the trace
// recorders themselves have no env-gate internally (the call *sites* in
// WorldRenderer gate on chunkTraceEnabled(), not the recorders), matching how
// core/FrameTrace.hpp's ScopedAccumulate is unconditionally cheap and the
// caller decides whether to invoke it.

#include "world/ChunkStreamingTrace.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <thread>

int main() {
    using namespace mc::diag;
    using Clock = ChunkStreamingMetrics::Clock;

    // -------------------------------------------------------------------
    // 1a. firstMeshLatency: arm -> record measures elapsed time, and only
    // for the chunk that was armed.
    // -------------------------------------------------------------------
    {
        ChunkStreamingMetrics metrics;
        const auto requested = Clock::now();
        metrics.armFirstMesh({3, 0, -2}, requested);
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        const auto uploaded = Clock::now();
        metrics.recordFirstMesh({3, 0, -2}, uploaded);

        const auto samples = metrics.firstMeshSamples();
        assert(samples.size() == 1U);
        assert(samples.front().chunkX == 3);
        assert(samples.front().chunkZ == -2);
        assert(samples.front().latencyMs >= 4.0); // sleep was 5ms; allow slack
        assert(samples.front().latencyMs < 2000.0); // sanity upper bound

        // A chunk that was never armed produces no sample (e.g. a remesh of
        // an already-resolved chunk should not fabricate a latency).
        metrics.recordFirstMesh({99, 0, 99}, Clock::now());
        assert(metrics.firstMeshSamples().size() == 1U);

        // Recording twice for the same chunk only counts the first landing
        // (armFirstMesh disarms after the first recordFirstMesh), matching
        // "first mesh" semantics — a second section of the same chunk must
        // not re-trigger.
        metrics.recordFirstMesh({3, 0, -2}, Clock::now());
        assert(metrics.firstMeshSamples().size() == 1U);
    }

    // -------------------------------------------------------------------
    // 1b. radiusFillTime: begin -> progress completes only once the
    // expected chunk count is reached for the *same* centre.
    // -------------------------------------------------------------------
    {
        ChunkStreamingMetrics metrics;
        const auto started = Clock::now();
        metrics.beginRadiusFill(0, 0, 1, 9U, started);

        // Progress from a different centre must not complete or corrupt the
        // armed sample (a stale batch from before a teleport, for example).
        metrics.noteRadiusProgress(5, 5, 9U, Clock::now());
        assert(metrics.radiusFillSamples().empty());

        // Partial progress at the right centre does not complete either.
        metrics.noteRadiusProgress(0, 0, 4U, Clock::now());
        assert(metrics.radiusFillSamples().empty());

        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        metrics.noteRadiusProgress(0, 0, 9U, Clock::now());
        const auto samples = metrics.radiusFillSamples();
        assert(samples.size() == 1U);
        assert(samples.front().centerX == 0);
        assert(samples.front().centerZ == 0);
        assert(samples.front().radius == 1);
        assert(samples.front().chunkCount == 9U);
        assert(samples.front().fillMs >= 4.0);

        // Once completed, the arm is consumed: further progress at the same
        // centre does not produce a duplicate sample.
        metrics.noteRadiusProgress(0, 0, 9U, Clock::now());
        assert(metrics.radiusFillSamples().size() == 1U);
    }

    // -------------------------------------------------------------------
    // 1c. streamingFrameCost: samples accumulate, ring-buffer bounded.
    // -------------------------------------------------------------------
    {
        ChunkStreamingMetrics metrics;
        metrics.recordFrameCost(1.5, 0.0, 0U);
        metrics.recordFrameCost(0.0, 2.5, 4U);
        const auto samples = metrics.streamingFrameCostSamples();
        assert(samples.size() == 2U);
        assert(samples[0].queueBatchMs == 1.5);
        assert(samples[1].uploadPrepMs == 2.5);
        assert(samples[1].sectionsUploaded == 4U);
    }

    // -------------------------------------------------------------------
    // 2. Missing-chunk detector.
    // -------------------------------------------------------------------
    {
        MissingChunkDetector detector;
        const auto deliveredAt = Clock::now();
        detector.noteChunkDelivered(1, 1, deliveredAt);
        assert(detector.trackedCount() == 1U);

        // Immediately after delivery, well inside the grace window: not yet
        // reported missing.
        const auto soon = Clock::now();
        assert(detector.scanMissing(soon, 1000.0).empty());

        // Past the grace window without a resolution: reported.
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        const auto later = Clock::now();
        const auto missing = detector.scanMissing(later, 5.0);
        assert(missing.size() == 1U);
        assert(missing.front().chunkX == 1);
        assert(missing.front().chunkZ == 1);
        assert(missing.front().openMs >= 5.0);

        // Resolving it (a section reached the GPU, or was confirmed empty)
        // clears the report even past the grace window.
        detector.noteChunkResolved(1, 1);
        assert(detector.scanMissing(Clock::now(), 5.0).empty());

        // A second chunk that is removed (legitimate unload) before ever
        // resolving must never be reported, regardless of how much time
        // passes — an intentional unload is not a gap. noteChunkRemoved
        // drops it from tracking entirely (unlike the already-resolved
        // chunk {1,1} above, which stays tracked-but-resolved).
        assert(detector.trackedCount() == 1U); // {1,1}, resolved
        detector.noteChunkDelivered(2, 2, Clock::now());
        assert(detector.trackedCount() == 2U);
        detector.noteChunkRemoved(2, 2);
        assert(detector.trackedCount() == 1U); // back to just {1,1}
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        assert(detector.scanMissing(Clock::now(), 0.0).empty());
    }

    // Multiple missing chunks are reported ordered worst-first (largest
    // openMs first), so a human scanning output sees the longest-open gap
    // at the top.
    {
        MissingChunkDetector detector;
        const auto t0 = Clock::now();
        detector.noteChunkDelivered(10, 0, t0);
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        detector.noteChunkDelivered(20, 0, Clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        const auto reports = detector.scanMissing(Clock::now(), 0.0);
        assert(reports.size() == 2U);
        assert(reports.front().chunkX == 10); // delivered first -> open longest
        assert(reports.front().openMs >= reports.back().openMs);
    }

    // -------------------------------------------------------------------
    // 3. Delivery-order trace.
    // -------------------------------------------------------------------
    {
        DeliveryOrderTrace trace;
        // A clean ring-by-ring expansion (0, 0, 1, 1, 1, 2, ...) is
        // monotonic.
        trace.record(0, 0, 0);
        trace.record(1, 0, 1);
        trace.record(0, 1, 1);
        trace.record(2, 0, 2);
        assert(trace.isMonotonicRingExpansion());
        const auto events = trace.events();
        assert(events.size() == 4U);
        assert(events[0].sequence < events[1].sequence);
        assert(events[3].ring == 2);
    }
    {
        // A regression (ring 2 delivered, then ring 0 delivered later) is
        // exactly what README problem ② describes: delivery not a strict
        // outward expansion. The trace must catch it.
        DeliveryOrderTrace trace;
        trace.record(2, 2, 2);
        trace.record(0, 0, 0);
        assert(!trace.isMonotonicRingExpansion());

        // Slack tolerates small batch-internal reordering (same ring or one
        // below) without excusing a whole-ring regression.
        DeliveryOrderTrace withinSlack;
        withinSlack.record(3, 0, 3);
        withinSlack.record(2, 1, 2); // one ring back — within slack 1
        assert(withinSlack.isMonotonicRingExpansion(1));
        assert(!withinSlack.isMonotonicRingExpansion(0));
    }

    // -------------------------------------------------------------------
    // Bug 1 fix: firstMeshLatency arms when a chunk enters the request radius,
    // not when its CPU batch later arrives. The WorldRenderer seam now walks
    // the whole (2r+1)^2 window and arms every position at the enter-radius
    // instant; armFirstMesh is idempotent, so a position already armed from an
    // earlier centre keeps its *earlier* (enter-radius) timestamp and the
    // latency measured spans the full enter-radius -> GPU-visible interval,
    // never the shorter batch-arrival -> GPU-visible one. This models that seam
    // headlessly: arm at t0 (radius entry), re-arm at a later t1 (a subsequent
    // centre move re-walks the overlapping window), then record — the sample
    // must reflect t0, proving the batch-arrival re-arm cannot shorten it.
    {
        ChunkStreamingMetrics metrics;
        const auto enteredRadiusAt = Clock::now();
        // Simulate a (2r+1)^2 radius arm covering the chunk (r=1 -> 9 arms).
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                metrics.armFirstMesh({dx, 0, dz}, enteredRadiusAt);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        // A later centre move re-walks the same window (the chunk is still in
        // radius). The idempotent re-arm must NOT overwrite the earlier arm —
        // otherwise the enter-radius interval would be lost, understating the
        // long tail exactly as the batch-arrival arm did.
        const auto laterReArm = Clock::now();
        metrics.armFirstMesh({0, 0, 0}, laterReArm);
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        metrics.recordFirstMesh({0, 0, 0}, Clock::now());

        const auto samples = metrics.firstMeshSamples();
        assert(samples.size() == 1U);
        // Total elapsed spans both sleeps (~10ms) from the ORIGINAL arm; if the
        // re-arm had won, only the second sleep (~5ms) would show.
        assert(samples.front().latencyMs >= 9.0);
    }

    // -------------------------------------------------------------------
    // Bug 2 fix: the delivery-order ring is computed against the centre in
    // effect when the section was ENQUEUED, not the latest centre at upload
    // time. With the fix the ring the seam feeds record() is always within
    // [0, loadRadius] even after the centre has since moved. This models the
    // seam: capture ring from the enqueue-time centre, move the centre, then
    // record the captured ring (not one recomputed from the moved centre).
    // -------------------------------------------------------------------
    {
        DeliveryOrderTrace trace;
        const int loadRadius = 12;
        // Section at chunk (12, 0); enqueued when the centre was (0,0):
        // captured ring = max(|12-0|,|0-0|) = 12  (== loadRadius, in bounds).
        const int enqueueCenterX = 0;
        const int enqueueCenterZ = 0;
        const int sectionChunkX = 12;
        const int sectionChunkZ = 0;
        const int capturedRing = std::max(std::abs(sectionChunkX - enqueueCenterX),
                                          std::abs(sectionChunkZ - enqueueCenterZ));
        assert(capturedRing == 12);
        // The centre then moves AWAY to (-1, 0) before this queued event is
        // uploaded. The stale-centre bug would recompute ring against (-1,0):
        // max(|12-(-1)|,0) = 13 > loadRadius — an impossible out-of-range ring.
        const int movedCenterX = -1;
        const int staleRing = std::max(std::abs(sectionChunkX - movedCenterX),
                                       std::abs(sectionChunkZ - enqueueCenterZ));
        assert(staleRing == 13 && staleRing > loadRadius); // what the bug produced
        // The fix records the captured ring, which stays in bounds.
        trace.record(sectionChunkX, sectionChunkZ, capturedRing);
        const auto events = trace.events();
        assert(events.size() == 1U);
        assert(events.front().ring >= 0 && events.front().ring <= loadRadius);
        assert(events.front().ring == 12);
    }

    // -------------------------------------------------------------------
    // Bug 3 fix: isMonotonicRingExpansion sorts by `sequence` before walking,
    // so a ring buffer that has WRAPPED (physical slot order != logical
    // delivery order) is judged on the true delivery order. Construct a
    // genuine wrap: fill the 4096-slot buffer with ring 5, then overwrite the
    // oldest slots with a rising ring. Physically, slot 0 now holds a high
    // ring while later slots still hold ring 5 — a physical-order walk sees a
    // regression and wrongly returns false; the sequence-sorted walk sees the
    // true order (all the ring-5s first, then the rising tail) and returns
    // true.
    // -------------------------------------------------------------------
    {
        DeliveryOrderTrace trace;
        constexpr int kCapacity = 4096;
        // Fill the buffer exactly: 4096 events, all ring 5, in delivery order.
        for (int i = 0; i < kCapacity; ++i) {
            trace.record(i, 0, 5);
        }
        // Now push a rising tail that wraps and overwrites the oldest slots.
        // In true (sequence) order these come strictly after every ring-5, and
        // never regress: 5 -> 5 -> 6 -> 7 -> 8. Monotonic.
        trace.record(0, 1, 5);
        trace.record(0, 2, 6);
        trace.record(0, 3, 7);
        trace.record(0, 4, 8);

        const auto raw = trace.events();
        assert(raw.size() == static_cast<std::size_t>(kCapacity));
        // Prove the buffer actually wrapped: physical order is NOT sorted by
        // sequence (some later slot holds an older sequence than slot 0).
        bool physicallyOutOfOrder = false;
        for (std::size_t i = 1; i < raw.size(); ++i) {
            if (raw[i].sequence < raw[i - 1].sequence) {
                physicallyOutOfOrder = true;
                break;
            }
        }
        assert(physicallyOutOfOrder); // confirms the wrap scenario is real
        // Despite the physical disorder, the true delivery order is a clean
        // outward expansion, so the sequence-sorted check accepts it.
        assert(trace.isMonotonicRingExpansion());

        // And a genuine post-wrap regression is still caught: append a ring-2
        // event (older-ring than the ring-8 just delivered) with the newest
        // sequence. In true order 8 -> 2 regresses; must return false.
        trace.record(9, 9, 2);
        assert(!trace.isMonotonicRingExpansion());
    }

    // -------------------------------------------------------------------
    // Global singletons + resetChunkStreamingTrace: the accessors used by
    // WorldRenderer's call sites should behave like ordinary singletons and
    // clear cleanly (verifies test isolation is possible for anything that
    // exercises the globals instead of a local instance).
    // -------------------------------------------------------------------
    {
        chunkStreamingMetrics().recordFrameCost(1.0, 1.0, 1U);
        missingChunkDetector().noteChunkDelivered(7, 7, Clock::now());
        deliveryOrderTrace().record(7, 7, 0);
        assert(!chunkStreamingMetrics().streamingFrameCostSamples().empty());
        assert(missingChunkDetector().trackedCount() > 0U);
        assert(!deliveryOrderTrace().events().empty());

        resetChunkStreamingTrace();
        assert(chunkStreamingMetrics().streamingFrameCostSamples().empty());
        assert(missingChunkDetector().trackedCount() == 0U);
        assert(deliveryOrderTrace().events().empty());
    }

    // chunkTraceEnabled() reads the env var once (cached), same pattern as
    // FrameTrace::traceEnabled(); just confirm it is callable and stable
    // across repeated calls within one process.
    {
        const bool first = chunkTraceEnabled();
        const bool second = chunkTraceEnabled();
        assert(first == second);
    }

    return 0;
}

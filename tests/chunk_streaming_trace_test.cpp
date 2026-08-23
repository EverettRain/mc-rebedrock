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

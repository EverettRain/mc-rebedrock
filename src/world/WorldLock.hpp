#pragma once

// The reader/writer lock guarding the one shared World.
//
// P3 Step 4. The world is written by the simulation every tick, by the render
// thread when a streamer batch or a drained world edit lands, and by the few
// short write call sites (save, inventory, pause). Once the tick moves off the
// render thread those are concurrent, and the plan's answer is a single coarse
// `shared_mutex`: one write section per tick, short write sections elsewhere.
//
// The render thread's per-frame *reads* no longer take this lock. Since
// M-Chunk/P0 (2026-08-16) the frame path samples only:
//   - the render-owned `clientCache` (a distinct World written exclusively by
//     the render thread, so single-threaded reads need no lock), and
//   - one immutable render-snapshot bundle (PlayerTickSnapshot / WorldSnapshot /
//     EntityRenderSnapshot), published under the world write lock through an
//     atomic shared handle, so an acquire read pins a complete frame and storage
//     is not reused while that reader still owns it.
// The remaining read sections cover one-time or non-frame reads
// (initializeSpawnPosition, the save path). Crucially, the GPU fence wait,
// command-buffer record, submit and present in drawFrame() hold no lock, so the
// simulation never waits on the GPU.
//
// This is deliberately *not* folded into World. Making every accessor lock
// would put a lock on the hottest read in the engine and would still not be
// correct — a caller that reads two blocks needs them under one lock, not two.
// The unit that has to be atomic is the *call site*, so that is where the guard
// goes.
//
// **The mutex is not recursive.** Taking a write section inside another is a
// deadlock, not a warning. The event queue (Step 3) is what makes that
// avoidable: a tick publishes rather than calling back into the renderer, so
// the write section around a tick never re-enters through a host callback.

#include <mutex>
#include <shared_mutex>

namespace mc::world {

class WorldLock final {
  public:
    // Held while reading the world. Several may be held at once.
    [[nodiscard]] std::shared_lock<std::shared_mutex> read() const {
        return std::shared_lock<std::shared_mutex>{mutex_};
    }
    // Held while writing. Excludes every reader and every other writer.
    [[nodiscard]] std::unique_lock<std::shared_mutex> write() {
        return std::unique_lock<std::shared_mutex>{mutex_};
    }

  private:
    mutable std::shared_mutex mutex_;
};

} // namespace mc::world

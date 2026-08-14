#pragma once

// The reader/writer lock guarding the one shared World.
//
// P3 Step 4. The world is read by the render thread every frame (block
// sampling for particles, rain, the weather ambience, the interaction ray, mesh
// building) and written by the simulation every tick, plus by the chunk
// streamer when a batch lands. Once the tick moves off the render thread those
// are concurrent, and the plan's answer is a single coarse `shared_mutex`: one
// read section per frame, one write section per tick. Fine-grained locking
// inside World would cost more (a lock per block read, on a path that does
// thousands per frame) and buy nothing while the write side is one tick.
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

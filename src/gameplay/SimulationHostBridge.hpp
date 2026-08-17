#pragma once

// The subscriber that turns the four event classes back into SimulationHost
// calls.
//
// A3a keeps SimulationHost rather than deleting it: it is still the right
// description of "the render-side reactions the simulation drives but does not
// own". What changes is who calls it. Emitters now publish; this bridge — one
// subscriber, installed once — performs the host calls in the same order they
// happened before, so the event layer is introduced with no behavioural
// difference to verify.
//
// P3 Step 3 (2026-08-13): the bridge now *queues* rather than calling straight
// through, and the main thread drains it once a frame. In a single-threaded
// build that only moves the side effects from mid-tick to end-of-frame; the
// point is that the simulation no longer touches renderer state at the moment
// it publishes, which is the precondition for Step 2 moving the tick onto its
// own thread. Making the queue thread-safe is then a change to this class
// alone, and not to any emitter.

#include "gameplay/GameEvents.hpp"

#include <variant>
#include <vector>
#include <atomic>
#include <mutex>

namespace mc::gameplay {

struct SimulationHost;

class SimulationHostBridge final {
  public:
    // Subscribes to every channel on `bus`. The bus must outlive the bridge,
    // which in practice means both are GameSession members.
    explicit SimulationHostBridge(GameEventBus& bus);

    SimulationHostBridge(const SimulationHostBridge&) = delete;
    SimulationHostBridge& operator=(const SimulationHostBridge&) = delete;

    // The host the queue is replayed into. Null until one is bound, and events
    // published before then are still queued — a headless test that never
    // supplies a host simply drops them on drain.
    void setHost(SimulationHost* host) { host_.store(host, std::memory_order_release); }
    [[nodiscard]] SimulationHost* host() const { return host_.load(std::memory_order_acquire); }

    // Replays everything queued since the last call, in publish order, into the
    // bound host. Returns how many events ran. Called once a frame by the
    // renderer; a headless caller that never drains simply accumulates, which
    // is why `clear` exists.
    std::size_t drain();
    void clear();
    [[nodiscard]] std::size_t pending() const;


  private:
    // One vector rather than four, because the *relative* order matters: a
    // block's break sound and its particles have to follow the world edit that
    // removed it, not run in a separate pass. Every payload is trivially
    // copyable, so this stays a flat buffer.
    using QueuedEvent =
        std::variant<WorldEditEvent, SoundEvent, ParticleEvent, PlayerDiedEvent,
                     ClientActionEvent>;

    void enqueue(QueuedEvent event);
    void run(const WorldEditEvent& event) const;
    void run(const SoundEvent& event) const;
    void run(const ParticleEvent& event) const;
    void run(const PlayerDiedEvent& event) const;
    void run(const ClientActionEvent& event) const;

    std::atomic<SimulationHost*> host_{nullptr};
    mutable std::mutex queueMutex_;
    std::vector<QueuedEvent> queued_;
};

} // namespace mc::gameplay

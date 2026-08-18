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

#include "gameplay/GameEventCodec.hpp"
#include "gameplay/GameEvents.hpp"

#include <variant>
#include <vector>
#include <atomic>
#include <mutex>

namespace mc::gameplay {

struct SimulationHost;

// Applies one decoded event to a host, the event->host mapping the bridge has
// always done, extracted so the client side of the transport (stage C-1b-3) can
// perform the very same host calls after decoding a GameEvent off the channel.
// GameEvent is the codec's variant, identical to the bridge's queued type.
void applyGameEvent(const GameEvent& event, SimulationHost& host);

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

    // Swaps the queued events out without applying them, for a caller that will
    // carry them elsewhere — stage C-1b-3's transport encodes each and sends it
    // on the channel, and the client applies them with applyGameEvent instead of
    // the bridge draining straight into the host. Preserves publish order.
    [[nodiscard]] std::vector<GameEvent> takeQueued();

  private:
    // One vector rather than four, because the *relative* order matters: a
    // block's break sound and its particles have to follow the world edit that
    // removed it, not run in a separate pass. Every payload is trivially
    // copyable, so this stays a flat buffer. The element type is the codec's
    // GameEvent (same variant), so takeQueued hands them straight to the codec.
    void enqueue(GameEvent event);

    std::atomic<SimulationHost*> host_{nullptr};
    mutable std::mutex queueMutex_;
    std::vector<GameEvent> queued_;
};

} // namespace mc::gameplay

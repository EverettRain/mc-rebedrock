#pragma once

// The fixed-step accumulator that turns real frame time into 20 TPS ticks.
//
// Step 1 lifted the loop out of the render loop; Step 2 gives it a thread.
//
// `start()` runs the tick on a std::jthread that wakes on the 50ms boundary,
// leaving the main thread to input, drawing and interpolation. `advance()`
// remains the synchronous form, and is what every headless test uses — the
// simulation must stay drivable without a thread, or `game_session_test` would
// be timing-dependent.
//
// The alpha is atomic because the render thread reads it every frame while the
// tick thread writes it. It is the only field crossing the boundary here;
// everything else the renderer needs travels through the event queue (Step 3),
// the render snapshot (Step 5) or the world lock (Step 4).

#include "gameplay/PlayerController.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <stop_token>
#include <thread>

namespace mc::gameplay {

class SimulationDriver final {
  public:
    // The longest backlog one frame may accumulate. A frame that took longer
    // than this (a stall, a breakpoint, a world load) drops the excess instead
    // of running a burst of catch-up ticks that would each be a frame of work.
    static constexpr float kMaximumBacklogSeconds = 0.25F;

    // Feeds `realDeltaSeconds` in and runs `tickOnce` for every whole step that
    // fits. Returns how many ticks ran, which the caller uses for diagnostics.
    template <typename TickOnce>
    std::size_t advance(float realDeltaSeconds, TickOnce&& tickOnce) {
        accumulator_ =
            std::min(accumulator_ + realDeltaSeconds, kMaximumBacklogSeconds);
        std::size_t ticks = 0U;
        while (accumulator_ >= PlayerController::kTickSeconds) {
            tickOnce();
            accumulator_ -= PlayerController::kTickSeconds;
            ++ticks;
        }
        publishAlpha();
        return ticks;
    }

    // Runs `tickOnce` on its own thread at 20 TPS until stop() (or destruction).
    // `running` is polled each wake-up: while it reads false the thread idles
    // and the backlog is dropped, which is how pausing and the title screen keep
    // the simulation quiet without tearing the thread down.
    void start(std::function<void()> tickOnce, std::function<bool()> running) {
        stop();
        thread_ = std::jthread{[this, tick = std::move(tickOnce),
                                active = std::move(running)](std::stop_token token) {
            auto next = std::chrono::steady_clock::now();
            const auto step = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>{PlayerController::kTickSeconds});
            while (!token.stop_requested()) {
                std::this_thread::sleep_until(next);
                const auto now = std::chrono::steady_clock::now();
                if (!active()) {
                    // Idle: keep the deadline rolling so resuming does not fire
                    // a burst of catch-up ticks.
                    next = now + step;
                    accumulator_ = 0.0F;
                    publishAlpha();
                    continue;
                }
                // A thread that fell far behind (a stall, a long chunk load)
                // resynchronises instead of chasing every missed deadline.
                if (now - next > std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<float>{kMaximumBacklogSeconds})) {
                    next = now;
                }
                tick();
                next += step;
                // The alpha is how far the *render* frame sits past this tick;
                // it is recomputed per frame from the wall clock below.
                lastTick_.store(now.time_since_epoch().count(), std::memory_order_release);
            }
        }};
    }

    void stop() {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
    }

    ~SimulationDriver() { stop(); }
    SimulationDriver() = default;
    SimulationDriver(const SimulationDriver&) = delete;
    SimulationDriver& operator=(const SimulationDriver&) = delete;

    // How far the render frame sits between the last tick and the next, in
    // [0, 1]. Read by the render thread every frame.
    [[nodiscard]] float interpolationAlpha() const {
        if (!thread_.joinable()) {
            return alpha_.load(std::memory_order_acquire);
        }
        // Threaded: derive it from the wall clock since the last tick, which is
        // smoother than sampling an accumulator the tick thread is mutating.
        const auto last = std::chrono::steady_clock::time_point{
            std::chrono::steady_clock::duration{lastTick_.load(std::memory_order_acquire)}};
        const auto elapsed = std::chrono::duration<float>{
            std::chrono::steady_clock::now() - last}.count();
        return std::clamp(elapsed / PlayerController::kTickSeconds, 0.0F, 1.0F);
    }

    // Drops the backlog. The render loop calls this while the simulation is not
    // running (paused, no world) so resuming does not immediately burn through
    // a quarter second of accumulated ticks.
    void reset() {
        accumulator_ = 0.0F;
        publishAlpha();
    }

    [[nodiscard]] bool threaded() const { return thread_.joinable(); }

  private:
    void publishAlpha() {
        alpha_.store(std::clamp(accumulator_ / PlayerController::kTickSeconds, 0.0F, 1.0F),
                     std::memory_order_release);
    }

    float accumulator_ = 0.0F;
    std::atomic<float> alpha_{0.0F};
    std::atomic<std::chrono::steady_clock::rep> lastTick_{0};
    std::jthread thread_;
};

} // namespace mc::gameplay

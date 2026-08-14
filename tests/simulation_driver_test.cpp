#include "gameplay/PlayerController.hpp"
#include "gameplay/SimulationDriver.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <stdexcept>
#include <string>

// The fixed-step accumulator, lifted out of the render loop (P3 Step 1). It has
// to behave exactly as the inline loop did — this is the step that changes
// nothing, so that the step which does change something has a fixed reference.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"simulation_driver_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

constexpr float kStep = mc::gameplay::PlayerController::kTickSeconds;

} // namespace

int main() {
    using mc::gameplay::SimulationDriver;

    // --- A frame shorter than one step runs no tick and leaves the leftover as
    // the interpolation alpha. ---
    {
        SimulationDriver driver;
        int ticks = 0;
        REQUIRE(driver.advance(kStep * 0.5F, [&] { ++ticks; }) == 0U);
        REQUIRE(ticks == 0);
        REQUIRE(std::fabs(driver.interpolationAlpha() - 0.5F) < 1.0e-4F);
    }

    // --- Steps accumulate across frames rather than being lost. Two half
    // frames make one tick, which is the whole point of the accumulator. ---
    {
        SimulationDriver driver;
        int ticks = 0;
        static_cast<void>(driver.advance(kStep * 0.6F, [&] { ++ticks; }));
        REQUIRE(ticks == 0);
        static_cast<void>(driver.advance(kStep * 0.6F, [&] { ++ticks; }));
        REQUIRE(ticks == 1);
        REQUIRE(std::fabs(driver.interpolationAlpha() - 0.2F) < 1.0e-3F);
    }

    // --- A long frame catches up with several ticks in one call. ---
    {
        SimulationDriver driver;
        int ticks = 0;
        REQUIRE(driver.advance(kStep * 3.0F, [&] { ++ticks; }) == 3U);
        REQUIRE(ticks == 3);
    }

    // --- The backlog is capped. A stall (or a breakpoint) must not queue up an
    // unbounded burst of catch-up ticks, each of which is a frame of work. ---
    {
        SimulationDriver driver;
        int ticks = 0;
        static_cast<void>(driver.advance(10.0F, [&] { ++ticks; }));
        const auto capped =
            static_cast<int>(SimulationDriver::kMaximumBacklogSeconds / kStep);
        REQUIRE(ticks <= capped);
        REQUIRE(ticks >= capped - 1);
    }

    // --- reset() drops the backlog, so resuming from pause does not burn
    // through a quarter second of accumulated ticks. ---
    {
        SimulationDriver driver;
        int ticks = 0;
        static_cast<void>(driver.advance(kStep * 0.9F, [&] { ++ticks; }));
        driver.reset();
        REQUIRE(driver.interpolationAlpha() == 0.0F);
        REQUIRE(driver.advance(kStep * 0.5F, [&] { ++ticks; }) == 0U);
        REQUIRE(ticks == 0);
    }

    // --- The alpha never leaves [0, 1], whatever it is fed: the renderer
    // multiplies positions by it, and an out-of-range value overshoots entities
    // past where they actually are. ---
    {
        SimulationDriver driver;
        static_cast<void>(driver.advance(10.0F, [] {}));
        REQUIRE(driver.interpolationAlpha() >= 0.0F);
        REQUIRE(driver.interpolationAlpha() <= 1.0F);
        SimulationDriver negative;
        static_cast<void>(negative.advance(-1.0F, [] {}));
        REQUIRE(negative.interpolationAlpha() >= 0.0F);
        REQUIRE(negative.interpolationAlpha() <= 1.0F);
    }

    // --- P3 Step 2: the threaded form actually ticks, and stops cleanly. ---
    {
        SimulationDriver driver;
        std::atomic<int> ticks{0};
        std::atomic<bool> running{true};
        REQUIRE(!driver.threaded());
        driver.start([&] { ++ticks; }, [&] { return running.load(); });
        REQUIRE(driver.threaded());

        // 20 TPS: a fifth of a second is ~4 ticks. Assert only that it ran at
        // all — a tighter bound would make this test fail on a loaded machine.
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        const int afterRunning = ticks.load();
        REQUIRE(afterRunning > 0);

        // While the predicate reads false the thread idles instead of ticking:
        // this is what pausing and the title screen use.
        running = false;
        std::this_thread::sleep_for(std::chrono::milliseconds{120});
        const int afterPause = ticks.load();
        std::this_thread::sleep_for(std::chrono::milliseconds{120});
        REQUIRE(ticks.load() == afterPause);

        // And resumes without firing a burst of catch-up ticks for the pause.
        running = true;
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        REQUIRE(ticks.load() > afterPause);
        REQUIRE(ticks.load() - afterPause < 20);

        driver.stop();
        REQUIRE(!driver.threaded());
        const int afterStop = ticks.load();
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        REQUIRE(ticks.load() == afterStop);
    }

    // --- Destruction joins the thread; a driver that outlived its callback
    // would tick into freed state. ---
    {
        std::atomic<int> ticks{0};
        {
            SimulationDriver driver;
            driver.start([&] { ++ticks; }, [] { return true; });
            std::this_thread::sleep_for(std::chrono::milliseconds{80});
        }
        const int afterDestruction = ticks.load();
        std::this_thread::sleep_for(std::chrono::milliseconds{80});
        REQUIRE(ticks.load() == afterDestruction);
    }

    // --- The alpha stays in range while the thread runs, since the renderer
    // multiplies positions by it every frame. ---
    {
        SimulationDriver driver;
        driver.start([] {}, [] { return true; });
        for (int sample = 0; sample < 200; ++sample) {
            const float alpha = driver.interpolationAlpha();
            REQUIRE(alpha >= 0.0F);
            REQUIRE(alpha <= 1.0F);
        }
        driver.stop();
    }

    return 0;
}

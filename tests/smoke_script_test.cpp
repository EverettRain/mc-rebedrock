#include "render/SmokeScript.hpp"

#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

// The scripted session's scheduler (render/SmokeScript.hpp). The script it drives
// only ever runs on a machine with a GPU, so the ordering rules that used to be
// spelled out inline in the render loop — which clock each step rides, when a
// wait is polled relative to the steps, when the finale and its exit fire — are
// pinned here instead, headless.

int main() {
    using mc::render::SmokeScript;

    // --- Menu steps ride the rendered-frame clock and stop at world start. ---
    {
        SmokeScript script;
        std::vector<int> fired;
        script.atRenderedFrame(2, [&] { fired.push_back(2); });
        script.atRenderedFrame(4, [&] {
            fired.push_back(4);
            script.markWorldStarted();
        });
        script.atRenderedFrame(6, [&] { fired.push_back(6); });

        for (std::uint64_t frame = 0; frame <= 8; ++frame) {
            script.advance(frame, /*worldReady=*/false);
        }
        // The step that opened the world ends the menu phase: frame 6 never runs.
        assert((fired == std::vector<int>{2, 4}));
        // No world was ready, so the gameplay clock never moved.
        assert(script.gameplayFrame() == 0);
    }

    // --- Gameplay steps ride a clock that only advances with a ready world. ---
    {
        SmokeScript script;
        std::vector<int> fired;
        script.atGameplayFrame(1, [&] { fired.push_back(1); });
        script.atGameplayFrame(3, [&] { fired.push_back(3); });

        script.advance(0, /*worldReady=*/false);
        script.advance(1, /*worldReady=*/false);
        assert(fired.empty());
        assert(script.gameplayFrame() == 0);

        script.advance(2, /*worldReady=*/true);  // gameplay frame 1
        assert((fired == std::vector<int>{1}));
        script.advance(3, /*worldReady=*/true);  // gameplay frame 2
        assert((fired == std::vector<int>{1}));
        script.advance(4, /*worldReady=*/true);  // gameplay frame 3
        assert((fired == std::vector<int>{1, 3}));
    }

    // --- A wait armed by a step is first polled on the NEXT frame. ---
    {
        SmokeScript script;
        int polls = 0;
        bool ready = false;
        bool landed = false;
        script.atGameplayFrame(1, [&] {
            script.waitFor(
                "effect lands", 10U,
                [&] {
                    ++polls;
                    return ready;
                },
                [&] { landed = true; });
        });

        script.advance(0, true);  // gameplay frame 1: the step arms the wait
        assert(polls == 0);       // the poll runs before the steps, so not yet
        script.advance(1, true);
        assert(polls == 1 && !landed);
        ready = true;
        script.advance(2, true);
        assert(polls == 2 && landed);
        // Satisfied waits stop polling.
        script.advance(3, true);
        assert(polls == 2);
    }

    // --- A wait that never comes true throws with its label. ---
    {
        SmokeScript script;
        script.atGameplayFrame(1, [&] {
            script.waitFor("never happens", 3U, [] { return false; });
        });
        bool threw = false;
        try {
            for (std::uint64_t frame = 0; frame < 20; ++frame) {
                script.advance(frame, true);
            }
        } catch (const std::runtime_error& error) {
            threw = true;
            assert(std::string{error.what()}.find("never happens") != std::string::npos);
        }
        assert(threw);
    }

    // --- Overlapping waits each keep their own assertion and their own clock. ---
    // The single wait slot used to let a later waitFor silently overwrite an
    // earlier, still-pending one: the dropped assertion never fired, never timed
    // out, and the smoke run stayed green while testing nothing. Two waits armed
    // a frame apart must both be polled and both resolve.
    {
        SmokeScript script;
        bool firstReady = false;
        bool secondReady = false;
        bool firstLanded = false;
        bool secondLanded = false;
        script.atGameplayFrame(1, [&] {
            script.waitFor(
                "first", 20U, [&] { return firstReady; }, [&] { firstLanded = true; });
        });
        script.atGameplayFrame(2, [&] {
            script.waitFor(
                "second", 20U, [&] { return secondReady; }, [&] { secondLanded = true; });
        });

        script.advance(0, true);  // gameplay frame 1: arms "first"
        script.advance(1, true);  // gameplay frame 2: arms "second"
        assert(script.pendingWaitCount() == 2U);
        // The second wait must not have displaced the first.
        secondReady = true;
        script.advance(2, true);
        assert(secondLanded && !firstLanded);
        assert(script.pendingWaitCount() == 1U);
        firstReady = true;
        script.advance(3, true);
        assert(firstLanded);
        assert(script.pendingWaitCount() == 0U);
    }

    // --- An overwritten wait used to vanish; now the stale one still times out. ---
    // The failing half of the same bug: if the earlier wait never comes true, the
    // run must fail with *its* label rather than silently passing.
    {
        SmokeScript script;
        script.atGameplayFrame(1, [&] {
            script.waitFor("stale assertion", 5U, [] { return false; });
        });
        script.atGameplayFrame(2, [&] {
            script.waitFor("fresh assertion", 50U, [] { return true; });
        });
        bool threw = false;
        try {
            for (std::uint64_t frame = 0; frame < 20; ++frame) {
                script.advance(frame, true);
            }
        } catch (const std::runtime_error& error) {
            threw = true;
            assert(std::string{error.what()}.find("stale assertion") != std::string::npos);
        }
        assert(threw);
    }

    // --- The finale fires once, then the exit follows after the delay. ---
    {
        SmokeScript script;
        bool ready = false;
        int finishes = 0;
        int exits = 0;
        script.finishWhen([&] { return ready; }, [&] { ++finishes; }, [&] { ++exits; });

        script.advance(0, true);
        assert(finishes == 0 && !script.finished());
        ready = true;
        script.advance(1, true);
        assert(finishes == 1 && exits == 0 && script.finished());
        // The gameplay clock stops once the run has finished.
        const std::uint64_t frozen = script.gameplayFrame();
        script.advance(1 + SmokeScript::kExitDelayFrames - 1, true);
        assert(exits == 0);
        script.advance(1 + SmokeScript::kExitDelayFrames, true);
        assert(exits == 1);
        assert(script.gameplayFrame() == frozen);
        // Both halves are one-shot.
        script.advance(100, true);
        assert(finishes == 1 && exits == 1);
    }

    return 0;
}

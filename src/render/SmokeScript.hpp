#pragma once

// The scheduler behind the scripted client session (MC_REBEDROCK_SMOKE_TEST and
// its MC_REBEDROCK_STRESS_FRAMES variant).
//
// The script used to live inline in the render loop: ~175 lines of
// `else if (smokeTest && smokeGameplayFrames == 40U) { … }` interleaved with the
// frame's real work, plus five loose locals (`smokeWaitActive`,
// `smokeWaitDeadline`, `smokeWaitCondition`, `smokeWaitAction`, `smokeWaitLabel`)
// carried across every frame of every run — a test harness threaded through the
// one loop that must stay readable. It is a scheduler, so it is one now: the
// steps are registered up front (SmokeScriptSteps.hpp) and the loop advances
// them with a single call, or never builds one at all when the env var is unset.
//
// Vulkan-free and GLFW-free: every effect is a std::function the caller supplies,
// so the ordering rules below are exercised by a headless unit test rather than
// only by a GPU run that no CI machine has.
//
// Frame semantics, preserved exactly from the inline form:
//   * menu steps run on the RENDERED-frame clock and stop once the world opens;
//   * gameplay steps run on the GAMEPLAY-frame clock, which only advances while
//     a world is ready and the run has not yet finished;
//   * a step may arm a wait for an effect that lands on a later server tick; the
//     wait is polled once per frame BEFORE that frame's steps, so a wait armed in
//     one frame is first checked in the next, and a timeout throws;
//   * the finale fires once its predicate holds, and the exit action follows a
//     few rendered frames later so the last screen actually draws.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mc::render {

class SmokeScript final {
  public:
    using Action = std::function<void()>;
    using Predicate = std::function<bool()>;

    // Rendered frames between the finale and the exit action, so the screen the
    // finale returned to is actually drawn before the window closes.
    static constexpr std::uint64_t kExitDelayFrames = 4;

    // A step on the rendered-frame clock, run only while the world has not
    // opened yet (the title/options/world-list walk).
    void atRenderedFrame(std::uint64_t frame, Action action) {
        menuSteps_.push_back({frame, std::move(action)});
    }

    // A step on the gameplay-frame clock (frames with a ready world).
    void atGameplayFrame(std::uint64_t frame, Action action) {
        gameplaySteps_.push_back({frame, std::move(action)});
    }

    // Called from a step body: stop treating the rendered-frame steps as due —
    // the world is open and the gameplay clock takes over.
    void markWorldStarted() noexcept { worldStarted_ = true; }

    // Arm a wait for something that lands on a later server tick (a chat command
    // taking effect, a mode switch). `condition` is polled once per frame until
    // it holds — then `then` runs — or until `timeoutFrames` gameplay frames have
    // passed, which throws with `label` in the message. Arming a second wait
    // replaces the first, matching the single-slot form this replaces.
    void waitFor(std::string label, std::uint64_t timeoutFrames, Predicate condition,
                 Action then = {}) {
        waitLabel_ = std::move(label);
        waitDeadline_ = gameplayFrame_ + timeoutFrames;
        waitCondition_ = std::move(condition);
        waitAction_ = std::move(then);
        waitActive_ = true;
    }

    // The finale: once `ready` holds (checked after each frame's steps), `finish`
    // runs, and `exit` follows kExitDelayFrames rendered frames later. Both are
    // one-shot. `ready` is never polled again after it fires, and the gameplay
    // clock stops advancing.
    void finishWhen(Predicate ready, Action finish, Action exit) {
        ready_ = std::move(ready);
        finish_ = std::move(finish);
        exit_ = std::move(exit);
    }

    // The gameplay-frame clock, for a caller that paces something else off it
    // (the stress camera's spiral).
    [[nodiscard]] std::uint64_t gameplayFrame() const noexcept { return gameplayFrame_; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    // One frame of the script. `renderedFrame` is the loop's own frame counter;
    // `worldReady` is whether a world is loaded and streamed enough to play.
    void advance(std::uint64_t renderedFrame, bool worldReady) {
        if (!worldStarted_) {
            fireDue(menuSteps_, renderedFrame, nextMenuStep_);
        }
        if (worldReady && !finished_) {
            ++gameplayFrame_;
        }
        pollWait();
        fireDue(gameplaySteps_, gameplayFrame_, nextGameplayStep_);
        if (!finished_ && ready_ && ready_()) {
            finished_ = true;
            finishedRenderedFrame_ = renderedFrame;
            if (finish_) {
                finish_();
            }
        }
        if (finished_ && exit_ && renderedFrame >= finishedRenderedFrame_ + kExitDelayFrames) {
            Action exit = std::move(exit_);
            exit_ = nullptr;
            exit();
        }
    }

  private:
    struct Step final {
        std::uint64_t frame = 0;
        Action action{};
    };

    // Steps are registered in ascending frame order and fired in that order, so
    // a single cursor is the whole traversal. `<=` rather than `==` means a step
    // is never silently skipped if the clock it rides ever jumps.
    static void fireDue(std::vector<Step>& steps, std::uint64_t clock, std::size_t& cursor) {
        while (cursor < steps.size() && steps[cursor].frame <= clock) {
            Action action = std::move(steps[cursor].action);
            ++cursor;
            if (action) {
                action();
            }
        }
    }

    void pollWait() {
        if (!waitActive_) {
            return;
        }
        if (waitCondition_ && waitCondition_()) {
            waitActive_ = false;
            waitCondition_ = nullptr;
            if (waitAction_) {
                Action action = std::move(waitAction_);
                waitAction_ = nullptr;
                action();
            }
            return;
        }
        if (gameplayFrame_ >= waitDeadline_) {
            throw std::runtime_error("Smoke test timed out: " + waitLabel_);
        }
    }

    std::vector<Step> menuSteps_{};
    std::vector<Step> gameplaySteps_{};
    std::size_t nextMenuStep_ = 0;
    std::size_t nextGameplayStep_ = 0;

    std::uint64_t gameplayFrame_ = 0;
    bool worldStarted_ = false;

    bool waitActive_ = false;
    std::uint64_t waitDeadline_ = 0;
    Predicate waitCondition_{};
    Action waitAction_{};
    std::string waitLabel_{};

    Predicate ready_{};
    Action finish_{};
    Action exit_{};
    bool finished_ = false;
    std::uint64_t finishedRenderedFrame_ = 0;
};

} // namespace mc::render

#pragma once

// PX-6: the game-in HUD toast infrastructure — a top-right notification queue
// with slide-in / stay / slide-out phases, mirroring 26.1's ToastComponent. This
// is Vulkan-free and GLFW-free: the queue, the visible-count cap and the per-
// toast animation state machine are pure logic in mc_rebedrock_runtime, so they
// are exercised by headless unit tests (push N over the cap -> visible <= cap;
// advance past a toast's lifetime -> it leaves; the phase progression). The
// renderer only reads visibleToasts() and paints each with GuiNineSlice/TextFont
// at the returned slide fraction.
//
// Toasts are CLIENT PRESENTATION (accessibility/feedback), not simulation, so the
// queue advances on frame delta-seconds, never on the world tick — matching
// vanilla, whose ToastComponent runs on the render clock.
//
// Gating: only a ToastKind whose trigger system already exists is ever pushed.
// Achievement / recipe-unlock / boss-bar toasts are intentionally absent (no such
// systems) — the enum carries only System, the one kind with a real trigger.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mc::ui {

// The category of a toast. Deliberately minimal: only System has a real trigger
// today (e.g. an option/gamerule change confirming to the player). Achievement/
// Recipe/etc. are NOT added until their systems exist — a placeholder kind would
// be a dead component. A `default` in any consumer switch keeps -Wswitch quiet if
// a kind is added later.
enum class ToastKind : std::uint8_t {
    System,
};

// One toast's content. A value: copyable, no heap graph beyond the two strings.
// `durationSeconds` is the STAY time; the slide in/out are added around it.
struct Toast final {
    ToastKind kind = ToastKind::System;
    std::string title{};
    std::string subtitle{};
    float durationSeconds = 5.0F;
};

// A toast's animation phase. Appearing slides in from the right edge; Visible is
// the steady stay; Disappearing slides back out; Done is removed next advance.
enum class ToastPhase : std::uint8_t {
    Appearing,
    Visible,
    Disappearing,
    Done,
};

// A live toast: its content plus the animation clock. `slideFraction` is 0 fully
// off-screen (right), 1 fully in — the renderer offsets the toast x by
// (1 - slideFraction) * width.
struct ActiveToast final {
    Toast toast{};
    ToastPhase phase = ToastPhase::Appearing;
    float elapsedInPhase = 0.0F;  // seconds spent in the current phase
    float slideFraction = 0.0F;   // 0 off-screen .. 1 fully shown
};

// The toast overlay's state. Holds the visible slots (capped) and a backlog of
// queued toasts waiting for a free slot, exactly like ToastComponent's visible
// grid + queue. Advancing on frame delta drives the phase machine and promotes
// backlog toasts as slots free up.
class ToastQueue final {
  public:
    // 26.1's ToastComponent shows a small fixed number at once; the rest wait.
    static constexpr std::size_t kMaxVisible = 5;
    // The slide in/out duration in seconds (vanilla's ~7 tick appear at 20 TPS).
    static constexpr float kSlideSeconds = 0.35F;

    // Enqueue a toast. If a visible slot is free it starts appearing immediately;
    // otherwise it waits in the backlog. Never drops or overflows a slot.
    void push(Toast toast) {
        if (visible_.size() < kMaxVisible) {
            visible_.push_back(ActiveToast{std::move(toast), ToastPhase::Appearing, 0.0F, 0.0F});
        } else {
            backlog_.push_back(std::move(toast));
        }
    }

    // Advance every visible toast by `deltaSeconds` (must be >= 0). Phase order:
    // Appearing (slide in over kSlideSeconds) -> Visible (stay durationSeconds) ->
    // Disappearing (slide out) -> Done (removed). Freed slots pull from backlog.
    void advance(float deltaSeconds) {
        if (deltaSeconds < 0.0F) {
            return;
        }
        for (ActiveToast& active : visible_) {
            stepToast(active, deltaSeconds);
        }
        // Remove finished toasts.
        std::size_t write = 0;
        for (std::size_t read = 0; read < visible_.size(); ++read) {
            if (visible_[read].phase != ToastPhase::Done) {
                if (write != read) {
                    visible_[write] = std::move(visible_[read]);
                }
                ++write;
            }
        }
        visible_.resize(write);
        // Promote backlog toasts into any freed slots.
        while (visible_.size() < kMaxVisible && !backlog_.empty()) {
            visible_.push_back(
                ActiveToast{std::move(backlog_.front()), ToastPhase::Appearing, 0.0F, 0.0F});
            backlog_.erase(backlog_.begin());
        }
    }

    [[nodiscard]] const std::vector<ActiveToast>& visibleToasts() const noexcept {
        return visible_;
    }
    [[nodiscard]] std::size_t visibleCount() const noexcept { return visible_.size(); }
    [[nodiscard]] std::size_t backlogCount() const noexcept { return backlog_.size(); }
    [[nodiscard]] bool empty() const noexcept { return visible_.empty() && backlog_.empty(); }

    void clear() noexcept {
        visible_.clear();
        backlog_.clear();
    }

  private:
    static void stepToast(ActiveToast& active, float deltaSeconds) {
        active.elapsedInPhase += deltaSeconds;
        switch (active.phase) {
            case ToastPhase::Appearing:
                active.slideFraction = clamp01(active.elapsedInPhase / kSlideSeconds);
                if (active.elapsedInPhase >= kSlideSeconds) {
                    active.slideFraction = 1.0F;
                    advancePhase(active, ToastPhase::Visible);
                }
                break;
            case ToastPhase::Visible:
                active.slideFraction = 1.0F;
                if (active.elapsedInPhase >= active.toast.durationSeconds) {
                    advancePhase(active, ToastPhase::Disappearing);
                }
                break;
            case ToastPhase::Disappearing:
                active.slideFraction = 1.0F - clamp01(active.elapsedInPhase / kSlideSeconds);
                if (active.elapsedInPhase >= kSlideSeconds) {
                    active.slideFraction = 0.0F;
                    advancePhase(active, ToastPhase::Done);
                }
                break;
            case ToastPhase::Done:
                break;
        }
    }

    static void advancePhase(ActiveToast& active, ToastPhase next) noexcept {
        active.phase = next;
        active.elapsedInPhase = 0.0F;
    }

    [[nodiscard]] static float clamp01(float value) noexcept {
        if (value < 0.0F) return 0.0F;
        if (value > 1.0F) return 1.0F;
        return value;
    }

    std::vector<ActiveToast> visible_{};
    std::vector<Toast> backlog_{};
};

}  // namespace mc::ui

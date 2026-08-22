// PX-6: the game-in HUD overlay infrastructure — the toast queue's cap/lifetime/
// animation state machine and the subtitle feed's cap/dedup/fade. Both are pure
// client-presentation logic (advance on frame delta), so they are fully headless-
// testable without a window or Vulkan.

#include "ui/SubtitleFeed.hpp"
#include "ui/Toast.hpp"

#include <cassert>
#include <cstddef>
#include <string>

using namespace mc;

namespace {

ui::Toast systemToast(std::string title, float stay = 5.0F) {
    ui::Toast t;
    t.kind = ui::ToastKind::System;
    t.title = std::move(title);
    t.durationSeconds = stay;
    return t;
}

// --- Gating: only the System toast kind exists (no achievement/recipe/boss) ----
// Those systems do not exist, so no placeholder kind is offered — a dead kind
// would let a fake trigger be wired. System is the sole enumerator (value 0).
void testGatingSystemOnly() {
    static_assert(static_cast<std::uint8_t>(ui::ToastKind::System) == 0,
                  "System is the only toast kind until its trigger systems exist");
    const ui::Toast t = systemToast("x");
    assert(t.kind == ui::ToastKind::System);
}

// --- Queue cap: pushing over the cap keeps visible <= cap, rest backlogged -----
void testQueueCap() {
    ui::ToastQueue queue;
    const std::size_t over = ui::ToastQueue::kMaxVisible + 3;
    for (std::size_t i = 0; i < over; ++i) {
        queue.push(systemToast("toast " + std::to_string(i)));
    }
    assert(queue.visibleCount() == ui::ToastQueue::kMaxVisible);
    assert(queue.backlogCount() == 3);
    // No advance yet: every visible toast is still appearing from off-screen.
    for (const auto& active : queue.visibleToasts()) {
        assert(active.phase == ui::ToastPhase::Appearing);
    }
}

// --- Animation state machine: appear -> visible -> disappear -> gone -----------
void testAnimationStateMachine() {
    ui::ToastQueue queue;
    queue.push(systemToast("hello", /*stay=*/1.0F));
    const auto& toasts = queue.visibleToasts();
    assert(toasts.size() == 1);
    assert(toasts[0].phase == ui::ToastPhase::Appearing);
    assert(toasts[0].slideFraction == 0.0F);

    // Half the slide-in: partway on-screen, still appearing.
    queue.advance(ui::ToastQueue::kSlideSeconds * 0.5F);
    assert(toasts[0].phase == ui::ToastPhase::Appearing);
    assert(toasts[0].slideFraction > 0.0F && toasts[0].slideFraction < 1.0F);

    // Finish the slide-in: fully shown, now in the stay phase.
    queue.advance(ui::ToastQueue::kSlideSeconds);
    assert(toasts[0].phase == ui::ToastPhase::Visible);
    assert(toasts[0].slideFraction == 1.0F);

    // Wait out the stay: it starts sliding out.
    queue.advance(1.0F);
    assert(toasts[0].phase == ui::ToastPhase::Disappearing);

    // Finish the slide-out: the toast is removed.
    queue.advance(ui::ToastQueue::kSlideSeconds);
    assert(queue.visibleCount() == 0);
    assert(queue.empty());
}

// --- Duration to expiry frees a slot and promotes the backlog ------------------
void testBacklogPromotion() {
    ui::ToastQueue queue;
    const std::size_t over = ui::ToastQueue::kMaxVisible + 1;
    for (std::size_t i = 0; i < over; ++i) {
        queue.push(systemToast("t" + std::to_string(i), /*stay=*/1.0F));
    }
    assert(queue.backlogCount() == 1);
    // Run one full toast lifetime: slide in + stay + slide out.
    queue.advance(ui::ToastQueue::kSlideSeconds);  // all appear
    queue.advance(1.0F);                           // all stay expires
    queue.advance(ui::ToastQueue::kSlideSeconds);  // all slide out, removed
    // The freed slots pulled in the backlog toast.
    assert(queue.backlogCount() == 0);
    assert(queue.visibleCount() == 1);
    assert(queue.visibleToasts()[0].toast.title == "t5");
}

// --- A negative delta is ignored (no time travel) ------------------------------
void testNegativeDeltaIgnored() {
    ui::ToastQueue queue;
    queue.push(systemToast("x", 1.0F));
    queue.advance(-1.0F);
    assert(queue.visibleToasts()[0].elapsedInPhase == 0.0F);
    assert(queue.visibleToasts()[0].phase == ui::ToastPhase::Appearing);
}

// --- Subtitle feed: cap, dedup-refresh, fade-out -------------------------------
void testSubtitleFeed() {
    ui::SubtitleFeed feed;
    // Fill past the cap; the oldest is evicted.
    for (std::size_t i = 0; i < ui::SubtitleFeed::kMaxCaptions + 2; ++i) {
        feed.show("sound " + std::to_string(i));
    }
    assert(feed.count() == ui::SubtitleFeed::kMaxCaptions);
    // The oldest ("sound 0"/"sound 1") were dropped; the newest is present.
    const auto& captions = feed.activeCaptions();
    assert(captions.back().text == "sound " + std::to_string(ui::SubtitleFeed::kMaxCaptions + 1));

    // Showing an existing caption refreshes its timer, not a duplicate row.
    const std::size_t before = feed.count();
    feed.advance(1.0F);  // age everything a bit
    feed.show(captions.back().text);
    assert(feed.count() == before);  // no new row
    // The refreshed one is back at full time.
    bool refreshed = false;
    for (const auto& c : feed.activeCaptions()) {
        if (c.text == "sound " + std::to_string(ui::SubtitleFeed::kMaxCaptions + 1)) {
            assert(c.remainingSeconds == ui::SubtitleFeed::kCaptionSeconds);
            refreshed = true;
        }
    }
    assert(refreshed);

    // Fade out: advancing past the caption lifetime removes them all.
    feed.advance(ui::SubtitleFeed::kCaptionSeconds + 0.01F);
    assert(feed.empty());
}

// --- Subtitle alpha fades from 1 toward 0 over the lifetime --------------------
void testSubtitleAlphaFade() {
    ui::SubtitleFeed feed;
    feed.show("clang");
    assert(feed.activeCaptions()[0].alpha() > 0.99F);  // fresh: full alpha
    feed.advance(ui::SubtitleFeed::kCaptionSeconds * 0.5F);
    const float mid = feed.activeCaptions()[0].alpha();
    assert(mid > 0.4F && mid < 0.6F);  // halfway: ~0.5
}

}  // namespace

int main() {
    testGatingSystemOnly();
    testQueueCap();
    testAnimationStateMachine();
    testBacklogPromotion();
    testNegativeDeltaIgnored();
    testSubtitleFeed();
    testSubtitleAlphaFade();
    return 0;
}

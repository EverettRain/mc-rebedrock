#pragma once

// PX-6: the sound-subtitle overlay feed (26.1 accessibility captions). When a
// sound with a subtitle plays and captions are enabled, its caption is shown at
// the bottom-right and fades over a few seconds; a repeated caption refreshes its
// timer instead of stacking a duplicate row, and only the most recent few are
// kept — exactly SubtitleOverlay's behaviour.
//
// Vulkan-free and GLFW-free: the caption list, its cap, the fade timer and the
// de-duplication are pure logic in mc_rebedrock_runtime, headless-testable. The
// data source is SoundRegistry.subtitle (already present); the renderer feeds
// captions in and reads activeCaptions() to draw. Client presentation — advances
// on frame delta, not the world tick.
//
// Gating: 26.1 exposes subtitles as a CLIENT accessibility option (not a
// gamerule; no showSubtitles gamerule exists here). The feed is inert until the
// renderer passes enabled=true from that option, so nothing shows until the
// toggle is wired — no fake-on component.

#include <cstddef>
#include <string>
#include <vector>

namespace mc::ui {

struct SubtitleCaption final {
    std::string text{};
    float remainingSeconds = 0.0F;  // counts down; removed at 0
    float totalSeconds = 3.0F;      // for the renderer's alpha fade

    [[nodiscard]] float alpha() const noexcept {
        if (totalSeconds <= 0.0F) return 0.0F;
        const float ratio = remainingSeconds / totalSeconds;
        return ratio < 0.0F ? 0.0F : (ratio > 1.0F ? 1.0F : ratio);
    }
};

class SubtitleFeed final {
  public:
    // 26.1 shows the most recent handful of captions.
    static constexpr std::size_t kMaxCaptions = 4;
    static constexpr float kCaptionSeconds = 3.0F;

    // Show a caption. If the same text is already active, its timer refreshes (no
    // duplicate row); otherwise it is added, evicting the oldest past the cap.
    void show(std::string text) {
        for (SubtitleCaption& caption : captions_) {
            if (caption.text == text) {
                caption.remainingSeconds = kCaptionSeconds;
                caption.totalSeconds = kCaptionSeconds;
                return;
            }
        }
        captions_.push_back(SubtitleCaption{std::move(text), kCaptionSeconds, kCaptionSeconds});
        if (captions_.size() > kMaxCaptions) {
            captions_.erase(captions_.begin());  // drop the oldest
        }
    }

    // Age every caption by `deltaSeconds`, removing any that have expired.
    void advance(float deltaSeconds) {
        if (deltaSeconds < 0.0F) {
            return;
        }
        std::size_t write = 0;
        for (std::size_t read = 0; read < captions_.size(); ++read) {
            captions_[read].remainingSeconds -= deltaSeconds;
            if (captions_[read].remainingSeconds > 0.0F) {
                if (write != read) {
                    captions_[write] = std::move(captions_[read]);
                }
                ++write;
            }
        }
        captions_.resize(write);
    }

    [[nodiscard]] const std::vector<SubtitleCaption>& activeCaptions() const noexcept {
        return captions_;
    }
    [[nodiscard]] std::size_t count() const noexcept { return captions_.size(); }
    [[nodiscard]] bool empty() const noexcept { return captions_.empty(); }
    void clear() noexcept { captions_.clear(); }

  private:
    std::vector<SubtitleCaption> captions_{};
};

}  // namespace mc::ui

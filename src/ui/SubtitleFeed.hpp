#pragma once

// 音效字幕叠加层的数据源，对应 26.1 的无障碍字幕
// 一个带字幕的音效播放且字幕开关打开时，它的字幕显示在右下角并在几秒内淡出
// 重复的字幕刷新自己的计时器而不是再堆一行，且只保留最近的少数几条，这正是 SubtitleOverlay 的行为
//
// 不碰 Vulkan 也不碰 GLFW
// 字幕列表、它的上限、淡出计时与去重都是 mc_rebedrock_runtime 里的纯逻辑，可无头测试
// 数据来源是已经存在的 SoundRegistry.subtitle
// 渲染器把字幕喂进来，再读 activeCaptions() 去画
// 它属于客户端呈现，按帧差推进而不是按世界 tick
//
// 门控原则上，26.1 把字幕作为客户端的无障碍选项暴露而不是游戏规则
// 这里也没有 showSubtitles 这条规则
// 渲染器从那个选项传进 enabled=true 之前，这个数据源始终是惰性的
// 开关没接好就什么都不显示，不存在假装打开的组件

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
    // 26.1 只显示最近的少数几条字幕
    static constexpr std::size_t kMaxCaptions = 4;
    static constexpr float kCaptionSeconds = 3.0F;

    // 显示一条字幕
    // 同样的文本已经在显示时刷新它的计时器，不再多出一行
    // 否则新增一条，超出上限时淘汰最旧的那条
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

    // 把每条字幕推进 deltaSeconds，并移除已经到期的
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

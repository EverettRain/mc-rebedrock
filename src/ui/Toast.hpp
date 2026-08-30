#pragma once

// 游戏内 HUD 的弹出提示设施，一个右上角的通知队列
// 它带滑入、停留、滑出三个阶段，对应 26.1 的 ToastComponent
// 这里不碰 Vulkan 也不碰 GLFW
// 队列、可见数量上限与逐条提示的动画状态机都是 mc_rebedrock_runtime 里的纯逻辑
// 它们因此能被无头单测覆盖：压入超过上限的条数后可见数不超过上限
// 越过一条的生命期后它离开，阶段推进本身也一样可测
// 渲染器只读 visibleToasts()，再按返回的滑动比例用 GuiNineSlice 与 TextFont 把每条画出来
//
// 弹出提示属于客户端呈现，是无障碍与反馈的一部分，不是模拟
// 所以队列按帧的秒差推进，绝不按世界 tick，这与 vanilla 一致，它的 ToastComponent 跑在渲染时钟上
//
// 门控原则是：只有触发它的系统已经存在的 ToastKind 才会被压入
// 成就、配方解锁、Boss 血条这几类刻意缺席，因为对应的系统还不存在
// 枚举里因此只有 System 这一种，它是唯一有真实触发源的

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mc::ui {

// 提示的类别，刻意保持最小
// 目前只有 System 有真实触发源，比如某个选项或游戏规则的改动向玩家确认
// 成就、配方之类在它们的系统存在之前不会加进来，占位的类别只会是一个死掉的组件
// 消费方的 switch 里留一个 default，日后新增类别时 -Wswitch 不会吵
enum class ToastKind : std::uint8_t {
    System,
};

// 一条提示的内容，是个值：可拷贝，除两个字符串外没有别的堆上结构
// durationSeconds 指的是停留时间，滑入与滑出各自加在它前后
struct Toast final {
    ToastKind kind = ToastKind::System;
    std::string title{};
    std::string subtitle{};
    float durationSeconds = 5.0F;
};

// 一条提示的动画阶段
// Appearing 从右边缘滑入，Visible 是稳定停留，Disappearing 滑回去，Done 会在下一次推进时被移除
enum class ToastPhase : std::uint8_t {
    Appearing,
    Visible,
    Disappearing,
    Done,
};

// 一条活着的提示：内容加上动画时钟
// slideFraction 为 0 表示完全在屏幕右侧之外，为 1 表示完全滑入
// 渲染器据此把提示的 x 偏移 (1 - slideFraction) * width
struct ActiveToast final {
    Toast toast{};
    ToastPhase phase = ToastPhase::Appearing;
    float elapsedInPhase = 0.0F;  // seconds spent in the current phase
    float slideFraction = 0.0F;   // 0 off-screen .. 1 fully shown
};

// 提示叠加层的状态
// 它持有可见槽位（有上限）与一列等待空槽的积压提示，正如 ToastComponent 的可见网格加队列
// 按帧差推进会驱动阶段机，并在槽位空出来时把积压的提示提上去
class ToastQueue final {
  public:
    // 26.1 的 ToastComponent 一次只显示固定的少数几条，其余等待
    static constexpr std::size_t kMaxVisible = 5;
    // 滑入与滑出的时长，单位秒，对应 vanilla 在 20 TPS 下约 7 tick 的出现过程
    static constexpr float kSlideSeconds = 0.35F;

    // 压入一条提示
    // 有空闲的可见槽位就立刻开始出现，否则在积压里等
    // 绝不丢弃，也绝不撑爆槽位
    void push(Toast toast) {
        if (visible_.size() < kMaxVisible) {
            visible_.push_back(ActiveToast{std::move(toast), ToastPhase::Appearing, 0.0F, 0.0F});
        } else {
            backlog_.push_back(std::move(toast));
        }
    }

    // 把每条可见提示推进 deltaSeconds，该值必须不小于零
    // 阶段顺序是 Appearing 用 kSlideSeconds 滑入，Visible 停留 durationSeconds
    // 然后 Disappearing 滑出，Done 被移除
    // 空出来的槽位从积压里补
    void advance(float deltaSeconds) {
        if (deltaSeconds < 0.0F) {
            return;
        }
        for (ActiveToast& active : visible_) {
            stepToast(active, deltaSeconds);
        }
        // 移除已经走完的提示
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
        // 把积压的提示提进任何空出来的槽位
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

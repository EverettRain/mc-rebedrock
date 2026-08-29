#pragma once

// 脚本化客户端会话（MC_REBEDROCK_SMOKE_TEST，及其 MC_REBEDROCK_STRESS_FRAMES 变体）的调度器
// 剧本是测试脚手架而不是玩法，所以它不住在渲染主循环里
// 步骤在 SmokeScriptSteps.hpp 里一次性注册好，主循环每帧只调一次 advance
// 不设环境变量时连脚本对象都不构造
//
// 不含 Vulkan 与 GLFW，所有效果都是调用方传进来的 std::function
// 下面这套时序规则因此由 headless 单测覆盖，不必依赖一台有 GPU 的机器
//
// 帧时序：
//   * 菜单步骤跑在"渲染帧"时钟上，世界一打开就不再触发
//   * 游戏步骤跑在"游戏帧"时钟上，该时钟只在世界就绪且本次运行尚未收尾时前进
//   * 步骤可以挂起一个等待，用于那些要到后续服务端 tick 才落地的效果
//     等待在每帧的步骤之前轮询一次，本帧挂起的等待因此最早下一帧才检查，超时直接抛异常
//   * 收尾条件成立时触发一次 finale，退出动作再延几帧执行，好让最后那屏真的画出来

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

    // finale 与退出动作之间相隔的渲染帧数，让 finale 返回到的那一屏在关窗前真的画出来
    static constexpr std::uint64_t kExitDelayFrames = 4;

    // 渲染帧时钟上的一步，仅在世界尚未打开时执行（标题/选项/世界列表那一段流程）
    void atRenderedFrame(std::uint64_t frame, Action action) {
        menuSteps_.push_back({frame, std::move(action)});
    }

    // 游戏帧时钟上的一步（世界已就绪的那些帧）
    void atGameplayFrame(std::uint64_t frame, Action action) {
        gameplaySteps_.push_back({frame, std::move(action)});
    }

    // 由某个步骤内部调用：世界已打开，渲染帧步骤不再触发，改由游戏帧时钟接管
    void markWorldStarted() noexcept { worldStarted_ = true; }

    // 挂起一个等待，用于要到后续服务端 tick 才生效的事情（聊天命令落地、模式切换）
    // `condition` 每帧轮询一次：成立则执行 `then`
    // 超过 `timeoutFrames` 个游戏帧仍不成立就抛异常，消息里带上 `label`
    // 只有一个等待槽，再挂一个会覆盖前一个
    void waitFor(std::string label, std::uint64_t timeoutFrames, Predicate condition,
                 Action then = {}) {
        waitLabel_ = std::move(label);
        waitDeadline_ = gameplayFrame_ + timeoutFrames;
        waitCondition_ = std::move(condition);
        waitAction_ = std::move(then);
        waitActive_ = true;
    }

    // 收尾：`ready` 在每帧的步骤之后检查，成立时执行 `finish`
    // 再过 kExitDelayFrames 个渲染帧执行 `exit`
    // 两者都只触发一次；`ready` 成立后不再轮询，游戏帧时钟也停止前进
    void finishWhen(Predicate ready, Action finish, Action exit) {
        ready_ = std::move(ready);
        finish_ = std::move(finish);
        exit_ = std::move(exit);
    }

    // 游戏帧时钟，供需要据此定拍的调用方使用（压测相机的螺旋轨迹）
    [[nodiscard]] std::uint64_t gameplayFrame() const noexcept { return gameplayFrame_; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    // 推进一帧
    // `renderedFrame` 是主循环自己的帧计数；`worldReady` 表示世界是否已加载并流送到可玩状态
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

    // 步骤按帧号升序注册、按同序触发，因此一个游标就是全部遍历状态
    // 用 `<=` 而不是 `==`：万一它所依附的时钟跳变，也不会有步骤被悄悄跳过
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

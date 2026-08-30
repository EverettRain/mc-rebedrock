#pragma once

// 本帧的玩家姿态，每帧从 tick 拥有的动作时间线与玩家的物理端点里提取一次
// 提取时用本帧的 tick 小数做插值
// 这把动画规范 §13.1 落到了实处，挥动、使用与行走步幅都在服务端 tick 上推进
// 帧内姿态则由上一 tick 与当前 tick 加 partialTicks 插出来
// 手臂因此不再以 20 TPS 跳变，同一个 tick 在任何帧率下都给出同一个姿态
//
// 这是一个纯值对象，不持有任何指向玩法层的引用
// 它可以安全地跨线程拷贝，且对每个消费者都是同一份，包括第一人称、世界中的玩家与背包预览

#include "gameplay/Inventory.hpp"
#include "gameplay/PlayerActionState.hpp"
#include "gameplay/PlayerController.hpp"
#include "gameplay/PlayerTickSnapshot.hpp"
#include "render/player/ArmPose.hpp"

#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace mc::render::player {

// 一次手臂挥动，在它按 tick 量化的两个端点之间插值
struct InterpolatedSwing final {
    bool active = false;
    gameplay::SwingAnimation animation = gameplay::SwingAnimation::Break;
    std::uint64_t sequence = 0U;
    float progress = 0.0F;  // in [0, 1], smooth between ticks
};

// 正在进行的物品使用，在它的两个 tick 端点之间插值
struct InterpolatedUse final {
    bool active = false;
    gameplay::UseAnimation animation = gameplay::UseAnimation::None;
    float progress = 0.0F;  // elapsed fraction of the use, in [0, 1]
};

// 渲染器画一个玩家所需的全部数据
// feetPosition、walkStride 与 walkSpeed 都在两个 tick 端点之间插值
// walkStride 是累积的步态相位，walkSpeed 是缓动后的运动幅度
struct PlayerRenderState final {
    glm::vec3 feetPosition{0.0F};
    float walkStride = 0.0F;
    float walkSpeed = 0.0F;

    // 旋转量，单位是度，由两个 tick 端点插出
    // bodyYaw 是躯干朝向，施加在世界根节点上
    // headYaw 是相对于躯干的量，头骨骼在躯干之上再转这么多
    // pitch 是头部与视线的俯仰
    // 第三人称身体与第一人称手部两个求解器都读这几个值，相机视角绝不参与，见动画规范 §5.2 与 §19.4
    float bodyYawDegrees = 0.0F;
    float headYawDegrees = 0.0F;  // relative to the body
    float pitchDegrees = 0.0F;

    bool sneaking = false;
    bool flying = false;
    bool sprinting = false;

    InterpolatedSwing swing;
    InterpolatedUse use;
    // 持有物的方块或物品，供 ArmPose 推导与手持物渲染使用
    gameplay::ItemStack heldStack{};

    // 手臂姿态在这里推导一次，所有消费者因此看到同一个结论
    // 主手渲染在主手臂上，默认是右手
    // 副手臂在副手槽位存在之前一律是 Empty
    ArmPose rightArmPose = ArmPose::Empty;
    ArmPose leftArmPose = ArmPose::Empty;
};

// 角度的最短路径插值，单位是度
// 从 179 到 -179 会跨过正 180 的接缝走 2 度，而不是朝反方向扫过 358 度，见动画规范 §13.3
// 这是一个纯函数
[[nodiscard]] inline float wrapDegrees(float degrees) {
    float wrapped = std::fmod(degrees + 180.0F, 360.0F);
    if (wrapped < 0.0F) {
        wrapped += 360.0F;
    }
    return wrapped - 180.0F;
}

[[nodiscard]] inline float lerpAngleDegrees(float from, float to, float alpha) {
    return from + wrapDegrees(to - from) * alpha;
}

// 用落在 0 到 1 之间的 partialTicks 对 tick 拥有的挥动做插值
// 动作的前后两个端点由模拟的 tick 采下，本帧位于它们之间
// lastSequence 是渲染器自己记住的上一帧采到的挥动序号
// 序号变化意味着这是一次全新的动作，进度必须直接跳到新挥动的起点而不是跨边界插值
// 从上一次的顶点插回 0 会让手臂看起来倒放了一遍
[[nodiscard]] inline InterpolatedSwing interpolateSwing(const gameplay::SwingState& state,
                                                        float partialTicks,
                                                        std::optional<std::uint64_t>& lastSequence) {
    InterpolatedSwing result;
    result.animation = state.animation;
    result.sequence = state.sequence;
    if (!state.active) {
        // 动作已经结束，收尾的挥动停在完成端点上
        // 进度 1.0 就是剪辑的静息姿态，与渲染器下一帧回落到的 None 姿态一致
        result.active = false;
        result.progress = 1.0F;
        lastSequence.reset();
        return result;
    }
    result.active = true;
    if (!lastSequence.has_value() || *lastSequence != state.sequence) {
        result.progress = state.progress;  // 新的挥动从它自己的值起步
    } else {
        result.progress =
            state.previousProgress + (state.progress - state.previousProgress) * partialTicks;
    }
    lastSequence = state.sequence;
    return result;
}

[[nodiscard]] inline InterpolatedUse interpolateUse(const gameplay::ItemUseState& state,
                                                    float partialTicks) {
    InterpolatedUse result;
    result.active = state.active;
    result.animation = state.animation;
    if (!state.active) {
        result.progress = 1.0F;
        return result;
    }
    // previousRemainingTicks 是本 tick 递减之前的剩余量，已用比例因此能平滑跨过 tick 边界
    const float previousElapsed = state.durationTicks > 0U
                                      ? 1.0F - static_cast<float>(state.previousRemainingTicks) /
                                                    static_cast<float>(state.durationTicks)
                                      : 0.0F;
    const float currentElapsed = state.durationTicks > 0U
                                     ? 1.0F - static_cast<float>(state.remainingTicks) /
                                                   static_cast<float>(state.durationTicks)
                                     : 0.0F;
    result.progress = previousElapsed + (currentElapsed - previousElapsed) * partialTicks;
    return result;
}

// 从逐 tick 快照提取出一整帧的玩家状态，用本帧的 tick 小数插值
// lastSwingSequence 是渲染器逐帧记住的上一次采到的挥动序号，动作重开时直接跳变而不是倒放
[[nodiscard]] inline PlayerRenderState extractPlayerRenderState(
    const gameplay::PlayerTickSnapshot& snapshot, float partialTicks,
    std::optional<std::uint64_t>& lastSwingSequence) {
    PlayerRenderState state;
    state.feetPosition = snapshot.physicsPrevious +
                         (snapshot.physicsCurrent - snapshot.physicsPrevious) * partialTicks;
    // 步态由控制器直接发布的 vanilla WalkAnimationState 驱动
    // 这里没有从视点摆动反推的取巧做法，也没有疾跑倍率
    // walkStride 是相位，对应 vanilla 的 position
    // walkSpeed 是幅度，对应 vanilla 的 speed，取 min(4d,1) 缓动后饱和到 1.0，停下时衰减到 0
    // 行走剪辑按 walk_position 与 walk_amount 读这两个值，求解器施加 cos(p * 0.6662) * A * s
    state.walkStride = snapshot.previousWalkPosition +
                       (snapshot.walkPosition - snapshot.previousWalkPosition) * partialTicks;
    state.walkSpeed = std::clamp(
        snapshot.previousWalkAmount +
            (snapshot.walkAmount - snapshot.previousWalkAmount) * partialTicks,
        0.0F, 1.0F);
    state.sneaking = snapshot.sneaking;
    state.flying = snapshot.flying;
    state.sprinting = snapshot.sprinting;

    // 旋转走带回绕的角度插值，正负 180 的接缝因此绝不会朝远端扫过去
    // 快照里存的头部偏航是绝对量，而渲染状态要的是相对躯干的量
    // 所以两者各自按最短路径插值之后再把躯干偏航减掉
    const float bodyYaw =
        lerpAngleDegrees(snapshot.previousBodyYawDegrees, snapshot.bodyYawDegrees, partialTicks);
    const float headYaw =
        lerpAngleDegrees(snapshot.previousHeadYawDegrees, snapshot.headYawDegrees, partialTicks);
    state.bodyYawDegrees = bodyYaw;
    state.headYawDegrees = wrapDegrees(headYaw - bodyYaw);
    state.pitchDegrees =
        snapshot.previousPitchDegrees +
        (snapshot.pitchDegrees - snapshot.previousPitchDegrees) * partialTicks;

    state.swing = interpolateSwing(snapshot.swing, partialTicks, lastSwingSequence);
    state.use = interpolateUse(snapshot.use, partialTicks);
    state.heldStack = snapshot.heldStack;

    // 手臂姿态在这里推导一次
    // 主手默认是右臂，当前正在使用物品的那只手就是主手
    // 副手臂还没有对应的槽位
    const bool usingMain = state.use.active && snapshot.use.hand == gameplay::InteractionHand::Main;
    state.rightArmPose = deriveArmPose(state.heldStack, usingMain, state.use.animation);
    state.leftArmPose = ArmPose::Empty;
    return state;
}

} // namespace mc::render::player

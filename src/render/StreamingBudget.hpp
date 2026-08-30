#pragma once

#include <cstddef>

namespace mc::render {

// 流送的 section 上传预算随 GPU 的实际负载调整
// 进入稠密区域时，工作线程会一次性把上百个 section 网格交给渲染线程
// 每帧的上传就是 GPU 的开销，包括传输本身与这些新 section 的首次绘制
// 平滑后的帧时间偏高说明 GPU 吃紧，预算随之下调，每帧只上传少数几个 section，负载因此被压住
// 帧时间恢复后预算回升，成片区域重新快速填满
// vanilla 的区块构建器按它自己的自然节奏交付；这是单进程引擎在渲染侧的等价物
inline constexpr std::size_t kMaxStreamingBudgetHigh = 16U;
inline constexpr std::size_t kMaxStreamingBudgetLow = 6U;
// 判定吃紧与恢复的帧时间阈值，单位毫秒，帧时间高于前者算吃紧，低于后者算恢复
// 两者之间的空档就是迟滞：预算只在两端变化，因此不会围着单一阈值来回抖
inline constexpr float kStreamingStressFrameMs = 13.0F;
inline constexpr float kStreamingRecoverFrameMs = 10.0F;

// 给定平滑后的帧时间（毫秒）与当前预算，算出该用的预算
// 吃紧时返回低预算，恢复后返回高预算，落在中间则保持当前值不变
[[nodiscard]] inline std::size_t streamingUploadBudgetForFrameMs(
    float frameMs, std::size_t currentBudget) {
    if (frameMs > kStreamingStressFrameMs) {
        return kMaxStreamingBudgetLow;
    }
    if (frameMs < kStreamingRecoverFrameMs) {
        return kMaxStreamingBudgetHigh;
    }
    return currentBudget;
}

} // namespace mc::render

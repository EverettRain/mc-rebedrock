#pragma once

#include <cstddef>

namespace mc::render {

// The streaming section-upload budget adapts to how loaded the GPU actually is.
// Entering a dense area, the worker hands the render thread hundreds of section
// meshes at once; each frame's uploads (transfer + first draw of the new
// sections) are the GPU cost. When the smoothed frame time is high the GPU is
// stressed, so the budget drops and only a few sections are uploaded per frame,
// damping the load; when the frame time recovers the budget returns and regions
// fill in fast again. Vanilla's chunk builder delivers work at its own natural
// rate; this is the render-side equivalent for a single-process engine.
inline constexpr std::size_t kMaxStreamingBudgetHigh = 16U;
inline constexpr std::size_t kMaxStreamingBudgetLow = 6U;
// Frame times (ms) that count as stressed (frame > this) or recovered
// (frame < this). The gap between them is hysteresis: the budget only changes
// at the extremes, so it does not oscillate around a single threshold.
inline constexpr float kStreamingStressFrameMs = 13.0F;
inline constexpr float kStreamingRecoverFrameMs = 10.0F;

// The budget to use given a smoothed frame time in milliseconds and the current
// budget. Returns the low budget when stressed, the high budget when recovered,
// and keeps the current value in between.
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

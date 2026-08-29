#pragma once

// 极低 Low 帧诊断插桩（临时，验证用）。
//
// 只做计时/计数并在超阈值帧输出一行，不改动任何游戏逻辑。全部访问都在渲染
// 主线程（帧循环、queueStreamBatch、persistUnloadedChunk 都跑在这里），因此
// 计数器用普通全局即可，无需原子。用环境变量开关，默认关闭：
//
//   MC_REBEDROCK_FRAME_TRACE     置任意值即开启
//   MC_REBEDROCK_FRAME_TRACE_MS  帧 CPU 时间阈值（毫秒，默认 16.67），
//                                只有超过阈值的帧才打印，避免逐帧日志自造卡顿
//
// 验证目标：证明 25–150ms 长帧的时间确实落在区块卸载同步落盘上。判定标准是
// 超阈值帧里 persistMs（≈lockHoldMs）占 frameMs 的比例 ≥70%，且 unloaded>0。

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace mc::diag {

struct FrameTrace final {
    using Clock = std::chrono::steady_clock;

    // 本帧累加项（渲染线程单线程访问）
    double persistMs = 0.0;    // persistUnloadedChunk 聚合墙钟
    double saveChunkMs = 0.0;  // 其中 SaveRepository::saveChunk 磁盘 I/O
    double lockHoldMs = 0.0;   // queueStreamBatch 内 worldLock.write 持有时长
    double drainMs = 0.0;      // drainEvents
    double fenceWaitMs = 0.0;  // drawFrame 的 vkWaitForFences
    double uploadMs = 0.0;     // world_.prepareStreamingUpdates（新 mesh 上传/暂存拷贝）
    double recordMs = 0.0;     // world_.recordCommandBuffer（遍历可见 section + 提交 draw call）
    double drawFrameMs = 0.0;  // drawFrame() 整体（含 record + HUD + acquire/submit/present）
    double inputMs = 0.0;      // processInput()（每帧输入准备）
    double acquireMs = 0.0;    // vkAcquireNextImageKHR（呈现节流/vsync 可能在此阻塞）
    double presentMs = 0.0;    // vkQueueSubmit + vkQueuePresentKHR
    double occlusionReadbackMs = 0.0; // releaseFrameResources + readBackOcclusionQueries（按 section 数 scale）
    double uniformMs = 0.0;    // updateShadowMatrix + updateUniform
    double imageWaitMs = 0.0;  // vkWaitForFences(swapchain image)（呈现节流真正阻塞点）
    std::uint32_t unloadedChunks = 0;
    std::uint32_t visibleSections = 0;  // recordCommandBuffer 本帧提交的可见 section 数
    std::uint32_t saveChunkCalls = 0;
    std::uint32_t queueBatchCount = 0;
    std::uint64_t editScan = 0;  // persistUnloadedChunk 累计扫描的 edits 条数
    int newCenterX = 0;
    int newCenterZ = 0;
    bool centerChanged = false;

    void reset() {
        persistMs = saveChunkMs = lockHoldMs = drainMs = fenceWaitMs = 0.0;
        uploadMs = recordMs = drawFrameMs = inputMs = acquireMs = presentMs = 0.0;
        occlusionReadbackMs = uniformMs = imageWaitMs = 0.0;
        unloadedChunks = visibleSections = saveChunkCalls = queueBatchCount = 0;
        editScan = 0;
        centerChanged = false;
    }
};

[[nodiscard]] inline FrameTrace& frameTrace() {
    static FrameTrace instance;
    return instance;
}

[[nodiscard]] inline bool traceEnabled() {
    static const bool enabled = std::getenv("MC_REBEDROCK_FRAME_TRACE") != nullptr;
    return enabled;
}

[[nodiscard]] inline double traceThresholdMs() {
    static const double threshold = [] {
        const char* value = std::getenv("MC_REBEDROCK_FRAME_TRACE_MS");
        if (value == nullptr || std::strlen(value) == 0) {
            return 16.67;
        }
        const double parsed = std::atof(value);
        return parsed > 0.0 ? parsed : 16.67;
    }();
    return threshold;
}

// 返回从 start 到现在的毫秒数。
[[nodiscard]] inline double msSince(FrameTrace::Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(FrameTrace::Clock::now() - start).count();
}

// RAII 计时器：把作用域内的墙钟累加到给定的累加器上（超阈值帧才有意义，但
// 无条件累加成本可忽略；调用点自己用 traceEnabled() 门控是否记录）。
class ScopedAccumulate final {
  public:
    explicit ScopedAccumulate(double& sink) : sink_(sink), start_(FrameTrace::Clock::now()) {}
    ~ScopedAccumulate() { sink_ += msSince(start_); }
    ScopedAccumulate(const ScopedAccumulate&) = delete;
    ScopedAccumulate& operator=(const ScopedAccumulate&) = delete;

  private:
    double& sink_;
    FrameTrace::Clock::time_point start_;
};

} // namespace mc::diag

#pragma once

// 极低帧诊断插桩，常驻但默认关闭
//
// 只做计时/计数并在超阈值帧输出一行，不改动任何游戏逻辑。全部访问都在渲染
// 主线程（帧循环、queueStreamBatch、persistUnloadedChunk 都跑在这里），因此
// 计数器用普通全局即可，无需原子。用环境变量开关，默认关闭：
//
//   MC_REBEDROCK_FRAME_TRACE     置任意值即开启
//   MC_REBEDROCK_FRAME_TRACE_MS  帧 CPU 时间阈值，单位毫秒，默认 16.67
//                                只有超过阈值的帧才打印，避免逐帧日志自己制造卡顿
//   MC_REBEDROCK_GRAPH_GAP_PROBE 置任意值即在世界那趟与界面那趟之间插入**两个空步**
//                                （RN-53 的判别仪器，见下面 graphGapProbeEnabled）
//
// 最初的验证目标是证明 25 到 150ms 的长帧确实落在区块卸载的同步落盘上
// 判定标准是超阈值帧里 persistMs 占 frameMs 的比例达到 70% 且 unloaded 大于 0
//
// 同一套判据后来复用给特效支线
// particleSimMs 加 rainSimMs 加 particleLightMs 合计占 cpuMs 的比例达到 70% 才算主因
// 实测只有约 4.8%，因此那条热路径维持原样不动

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace mc::diag {

struct FrameTrace final {
    using Clock = std::chrono::steady_clock;

    // RN-19d0：GPU 侧的阶段计时。上面每一项都是 CPU 墙钟，而 §4 那张优化清单要判的
    // 是**填充率与片元成本**——CPU 侧看不见它们，`fenceWaitMs` 只会告诉你「等了多久」，
    // 不会告诉你等的是哪一趟。这几项来自 frame graph 的步边界时间戳，阶段集合与执行
    // 集合同源（见 render/graph/GpuTimestamps.hpp）。
    //
    // 上限是槽位数，不是「今天有几步」：图会随画质开关重编译（关太阳阴影就少一步），
    // 20f 的光影包前端还会再加。超出的步只是不进报告，不会越界。
    static constexpr std::size_t kMaxGpuSteps = 16;

    // 本帧累加项（渲染线程单线程访问）
    double persistMs = 0.0;    // persistUnloadedChunk 聚合墙钟
    double saveChunkMs = 0.0;  // 其中 SaveRepository::saveChunk 磁盘 I/O
    double lockHoldMs = 0.0;   // queueStreamBatch 内 worldLock.write 持有时长
    double drainMs = 0.0;      // drainEvents
    double fenceWaitMs = 0.0;  // drawFrame 的 vkWaitForFences
    double uploadMs = 0.0;     // world_.prepareStreamingUpdates（新 mesh 上传/暂存拷贝）
    double recordMs = 0.0;     // world_.recordCommandBuffer（遍历可见 section + 提交 draw call）
    // RN-20a：烘焙式 frame graph 的 execute() 自身开销，**不含 pass body**
    // body 的时间已经被 recordMs 量着，两者是包含关系而不是并列关系
    double graphMs = 0.0;      // BakedGraph::execute（屏障合批 + begin/end renderpass 的编排）
    double drawFrameMs = 0.0;  // drawFrame() 整体（含 record + HUD + acquire/submit/present）
    // RN-54：帧循环里 drawFrame **之外**那一段。
    //
    // 立这三项之前，`cpuMs` 有近八成没有归属：一次实机 trace 里 cpuMs=17.23 而
    // drawFrameMs=3.62，13.6 ms 落在所有已插桩字段的**外面**（acquireMs=0.018、
    // presentMs=1.24、imageWaitMs=0.0002 全都不占）。那种状态下「呈现节奏是瓶颈」
    // 这句话既证不了也证不伪——等待点在哪都不知道。
    //
    // 恒等式 `cpuMs == beforeDrawMs + drawFrameMs + afterDrawMs` 是这三项的**自检**：
    // `unaccMs` 在报告里直接打出来，它不接近 0 就说明有测点错位或漏项，
    // 而不是「有一段神秘的时间」。别把 unaccMs 当成一个可优化的量。
    double cpuMs = 0.0;        // 帧循环一次迭代的全部墙钟（与打印的 cpuMs= 同一个数）
    double beforeDrawMs = 0.0; // 迭代开头 → drawFrame() 调用前（poll/插值/世界更新都在内）
    double afterDrawMs = 0.0;  // drawFrame() 返回 → 迭代结束
    double pollMs = 0.0;       // glfwPollEvents（Mac 上它跑 NSRunLoop，是等待的头号嫌疑）
    double inputMs = 0.0;      // processInput()（每帧输入准备）
    double acquireMs = 0.0;    // vkAcquireNextImageKHR（呈现节流/vsync 可能在此阻塞）
    double presentMs = 0.0;    // vkQueueSubmit + vkQueuePresentKHR
    double occlusionReadbackMs = 0.0; // releaseFrameResources + readBackOcclusionQueries（按 section 数 scale）
    double uniformMs = 0.0;    // updateShadowMatrix + updateUniform
    double imageWaitMs = 0.0;  // vkWaitForFences(swapchain image)（呈现节流真正阻塞点）
    // 特效支线也就是粒子与雨的三段，此前这三处完全没有归属
    // CPU 模拟散在 cpuMs 的余量里，逐粒子光照采样又混在 recordMs 里和地形 section 遍历搅在一起
    // 那种状态下这条假设既证不了也证不伪
    double particleSimMs = 0.0;    // ParticleSystem::update（逐粒子 world.state + 积分）
    double rainSimMs = 0.0;        // RainSystem::update + emitTextureImpacts
    double particleLightMs = 0.0;  // 粒子/雨滴记录构建，含逐条 packedSceneLight（各 2 次区块查找）
    // RN-22：半透明逐 quad 重排。两个数一起看才有意义——「一次多贵」乘「一秒几次」。
    // 重排的成本本身在 uploadMs 里（它挂在 prepareStreamingUpdates 尾巴上），
    // 这里单独把它拆出来，因为它的触发条件与网格上传完全不同：上传跟着区块流送走，
    // 重排跟着相机走，转个身可能一帧几十次、站着不动一次都没有。
    double translucentResortMs = 0.0;
    std::uint32_t translucentResorts = 0;      // 本帧真正重排的 section 数
    std::uint32_t translucentSections = 0;     // 有半透明几何、参与调度的 section 数
    std::uint32_t unloadedChunks = 0;
    std::uint32_t visibleSections = 0;  // recordCommandBuffer 本帧提交的可见 section 数
    std::uint32_t saveChunkCalls = 0;
    std::uint32_t queueBatchCount = 0;
    std::uint32_t particleCount = 0;   // 本帧存活粒子数
    std::uint32_t rainDropCount = 0;   // 本帧存活雨滴数
    std::uint32_t rainLookups = 0;     // RainSystem::lastUpdateLookups()（列探测的世界查询次数）
    std::uint64_t editScan = 0;  // persistUnloadedChunk 累计扫描的 edits 条数
    // GPU 侧：整张图的跨度，以及逐步的分解。名字取自 graph 的步名，不另立一张表。
    // 这几项是**赋值**不是累加：一帧只执行一次图，累加会把两帧的数糊在一起。
    double gpuFrameMs = 0.0;
    std::array<double, kMaxGpuSteps> gpuStepMs{};
    std::array<std::string_view, kMaxGpuSteps> gpuStepName{};
    std::uint32_t gpuStepCount = 0;
    int newCenterX = 0;
    int newCenterZ = 0;
    bool centerChanged = false;

    void reset() {
        persistMs = saveChunkMs = lockHoldMs = drainMs = fenceWaitMs = 0.0;
        uploadMs = recordMs = drawFrameMs = inputMs = acquireMs = presentMs = 0.0;
        cpuMs = beforeDrawMs = afterDrawMs = pollMs = 0.0;
        occlusionReadbackMs = uniformMs = imageWaitMs = graphMs = 0.0;
        particleSimMs = rainSimMs = particleLightMs = translucentResortMs = 0.0;
        translucentResorts = translucentSections = 0;
        unloadedChunks = visibleSections = saveChunkCalls = queueBatchCount = 0;
        particleCount = rainDropCount = rainLookups = 0;
        editScan = 0;
        gpuFrameMs = 0.0;
        gpuStepMs = {};
        gpuStepName = {};
        gpuStepCount = 0;
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

// RN-53 的判别仪器：世界那趟与界面那趟之间那 1 ms 归谁。
//
// 实机 frametrace 里 `gpu[menu_background]` 稳定 0.96–1.27 ms，而那一步在没有界面
// 打开时**一条命令都不录**，帧图也不为它下屏障（frame_graph_test 的
// testMenuBackgroundSampleOnlyAddsUsage 把这两条钉住了）。于是那段时间只可能来自
// 它两侧的边界，而不是它自己。开着这个开关，图会在 world → menu_background → gui
// 之间插入两个**同样空**的步：
//
//   gpu[probe_after_world]  world 那趟之后、menu_background 之前
//   gpu[menu_background]    原来那一步
//   gpu[probe_before_gui]   menu_background 之后、gui 那趟之前
//
// 三个读数一次分辨三种假说：
//   ① 只有 probe_after_world 有值   → 代价是世界那趟的收尾（Apple 上 21 MB 的 tile
//                                     store flush），被归到它后面第一个空隙上
//   ② 只有 probe_before_gui 有值    → 代价是界面那趟的开场（LOAD 把 scene_color 读回 tile）
//   ③ 三个都有值、大致均分或各自 ~1 ms → 代价是**时间戳边界本身**，即仪器自己造的
//
// ③ 尤其要排除：这些 `vkCmdWriteTimestamp` 只在 MC_REBEDROCK_FRAME_TRACE 开着时
// 存在，而那 1 ms 正是在 trace 开着时量到的。「先修仪器再信画面」在这条线上兑现过五次。
//
// 只在图**编译期**读一次（`buildFrameGraphTables`），因此关着时热路径上连一个恒假的
// if 都没有——那一步是被编译期剪枝剪掉的，不是运行期跳过的。
[[nodiscard]] inline bool graphGapProbeEnabled() {
    static const bool enabled = std::getenv("MC_REBEDROCK_GRAPH_GAP_PROBE") != nullptr;
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

// 返回从 start 到现在的毫秒数
[[nodiscard]] inline double msSince(FrameTrace::Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(FrameTrace::Clock::now() - start).count();
}

// RAII 计时器，把作用域内的墙钟时间累加到给定的累加器上
// 只有超阈值帧才用得上这个值，但无条件累加的成本可以忽略
// 调用点自己用 traceEnabled() 决定要不要记录
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

// RN-54：帧循环 CPU 归属的三段计时 —— 测点位置的护栏
//
// 立这几项之前，`cpuMs` 有近八成没有归属。一次实机 trace：
//
//   cpuMs=17.2323  drawFrameMs=3.62104  acquireMs=0.018042
//   presentMs=1.24079  imageWaitMs=0.000208  fenceWaitMs=0.001625
//
// 13.6 ms 落在所有已插桩字段的**外面**，而三个可能的阻塞点加起来不到 1.3 ms。
// 那种状态下「呈现节奏是瓶颈」既证不了也证不伪——连等待点在哪都不知道，
// 于是「持续 5 秒掉到 30 FPS」那条巨峰也无从定位（33 ms 的帧里会有 30 ms 没有归属）。
//
// 这个测试盯的是**测点位置**，不是算术。理由是 HANDOFF §5.3：把
// `beforeDrawMs` 挪到 `glfwPollEvents` 之后、或让 `pollMs` 顺手包住旁边那两个调用，
// 都不会让任何数值断言变红——恒等式照样成立，只是每一段的**含义**变了，
// 而那正是这几项存在的全部理由。一个量错了位置的计时字段比没有更糟：
// 它会把下一个人指向错误的方向，而且看起来完全正常。
//
// 恒等式本身（`cpuMs == beforeDrawMs + drawFrameMs + afterDrawMs`）由报告里的
// `unaccMs` 自检，那是实测项：容器里跑 20 秒，全部样本落在 ±0.0004 ms 内。

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#ifndef MC_REBEDROCK_SOURCE_DIR
#error "MC_REBEDROCK_SOURCE_DIR must point at the repository root"
#endif

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

[[nodiscard]] std::string readSourceFile(const std::filesystem::path& path) {
    std::ifstream stream{path};
    if (!stream) {
        std::cerr << "FAIL: cannot read " << path.string() << '\n';
        return {};
    }
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

// 从 `anchor` 起、到它之后第一个 `terminator` 止的那一段源码。
[[nodiscard]] std::string_view sliceFrom(std::string_view source, std::string_view anchor,
                                         std::string_view terminator) {
    const auto begin = source.find(anchor);
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = source.find(terminator, begin);
    if (end == std::string_view::npos) {
        return source.substr(begin);
    }
    return source.substr(begin, end - begin);
}

// ---- 1. pollMs 紧包 glfwPollEvents，一句不多 --------------------------------
//
// 会错的形态：把 `persistWindowPlacementIfSettled()` / `pollLanguageLoad()` 也圈进去。
// 那两个调用一个写窗口位置、一个轮询语言加载，它们的时间不是「等在事件循环」。
void testPollTimerWrapsOnlyPollEvents(const std::string& source) {
    const std::string_view block =
        sliceFrom(source, "const auto pollStart = std::chrono::steady_clock::now();",
                  "persistWindowPlacementIfSettled();");
    check(!block.empty(), "找得到 pollMs 的计时块");
    check(block.find("glfwPollEvents();") != std::string_view::npos,
          "pollMs 的计时块里就是 glfwPollEvents");
    check(block.find("diag::frameTrace().pollMs") != std::string_view::npos,
          "那一段结束时累加到 pollMs");
    // 一句不多：计时块内除了 glfwPollEvents 不得有别的函数调用。
    // 判据是分号数——`pollStart` 声明、`glfwPollEvents()`、以及 if 里那一句累加。
    check(std::count(block.begin(), block.end(), ';') == 3,
          "pollMs 只圈住 glfwPollEvents，没顺手圈进旁边的调用");
    check(block.find("pollLanguageLoad") == std::string_view::npos &&
              block.find("persistWindowPlacement") == std::string_view::npos,
          "窗口位置持久化与语言轮询不属于「等在事件循环」，不得计入 pollMs");
}

// ---- 2. beforeDrawMs 的起点是迭代开头，不是 poll 之后 -----------------------
//
// 会错的形态：拿 `pollStart` 或别的中途时刻当起点。那会让 beforeDrawMs 漏掉
// 迭代前半，而恒等式的差额会静默跑进 unaccMs——一个「看起来只是精度问题」的偏差。
void testBeforeDrawStartsAtIterationStart(const std::string& source) {
    check(source.find("diag::frameTrace().beforeDrawMs +=\n"
                      "                        std::chrono::duration<double, std::milli>("
                      "drawStart - frameCpuStart)") != std::string::npos,
          "beforeDrawMs 量的是 frameCpuStart → drawStart，两端都不许换");
    // drawStart 必须就是 drawFrame() 的调用前一刻——中间不许插别的工作
    const std::string_view block =
        sliceFrom(source, "const auto drawStart = std::chrono::steady_clock::now();",
                  "static_cast<void>(drawFrame(perfFrameId));");
    check(!block.empty(), "找得到 drawStart 到 drawFrame 之间那一段");
    check(block.find("frameCpuStart") != std::string_view::npos,
          "那一段里累加 beforeDrawMs，用的是 frameCpuStart");
    // 那一段里只许有：drawStart 声明、if(traceEnabled)、那一次累加。别的调用都是
    // 「被算进 beforeDrawMs 却发生在 drawStart 之后」的矛盾。
    check(block.find("world_.") == std::string_view::npos &&
              block.find("hud_.") == std::string_view::npos,
          "drawStart 与 drawFrame() 之间不得插入世界或界面的工作");
}

// ---- 3. afterDrawMs 的起点在 drawFrame 返回之后 ----------------------------
void testAfterDrawStartsWhenDrawFrameReturns(const std::string& source) {
    const std::string_view block = sliceFrom(source, "static_cast<void>(drawFrame(perfFrameId));",
                                             "++renderedFrames;");
    check(!block.empty(), "找得到 drawFrame 之后那一段");
    check(block.find("afterDrawStart = std::chrono::steady_clock::now();") !=
              std::string_view::npos,
          "afterDrawStart 在 drawFrame() 返回之后立刻取");
    check(block.find("drawFrameMs += diag::msSince(drawStart)") != std::string_view::npos,
          "drawFrameMs 仍在同一段里收口");
    check(source.find("diag::frameTrace().afterDrawMs = diag::msSince(afterDrawStart);") !=
              std::string::npos,
          "afterDrawMs 在报告前用 afterDrawStart 收口");
    // afterDrawStart 的初值必须是 frameCpuStart：诊断关着时它从不被赋值，
    // 而一个未初始化的时间点会让 afterDrawMs 变成垃圾。
    check(source.find("auto afterDrawStart = frameCpuStart;") != std::string::npos,
          "afterDrawStart 初值是 frameCpuStart，不是默认构造的时间点");
}

// ---- 5. 新 trace 的帧边界是本帧开始到下一帧开始 ----------------------------
//
// 这两条 span 不能从 frameCpuStart 起：它会漏掉本轮尾部的 pacing、存档加载钩子及
// smoke 脚本。frame.work 则必须在 limiter 前收口，才能把工作时间与限帧等待分开。
void testPerfFrameBoundaryAndWorkCut(const std::string& source) {
    check(source.find("const auto frameBoundary = diag::PerfTrace::Clock::now();") !=
              std::string::npos,
          "PerfTrace 在循环开头取唯一 frameBoundary");
    check(source.find("recordSpan(\"frame.wall\", *previousFrameStart,\n"
                      "                                                        frameBoundary, perfFrameId)") !=
              std::string::npos,
          "frame.wall 是前一次起点到本次起点的闭合墙钟 span");
    check(source.find("recordSpan(\"frame.work\", *previousFrameStart,\n"
                      "                                                            *previousFrameWorkEnd, perfFrameId)") !=
              std::string::npos,
          "frame.work 使用同一帧起点并在 work 结束处收口");
    const auto workEnd = source.find("previousFrameWorkEnd = diag::PerfTrace::Clock::now();");
    const auto pacing = source.find("scope(\"frame_pacing\", perfFrameId)");
    check(workEnd != std::string::npos && pacing != std::string::npos && workEnd < pacing,
          "frame.work 在 frame_pacing 之前收口，limiter 只计入 frame.wall 与 pacing span");
}

// ---- 4. 四项都进报告，且 unaccMs 是那条恒等式的余项 -------------------------
//
// 会错的形态：加了字段却没打印，或 unaccMs 少减一项。后者最坏——它会稳定报出
// 一个非零的「神秘时间」，而那正是这一整套要消灭的东西。
void testReportCarriesAllFourAndTheIdentity(const std::string& source) {
    for (const std::string_view field :
         {std::string_view{" beforeDrawMs=\""}, std::string_view{" pollMs=\""},
          std::string_view{" afterDrawMs=\""}, std::string_view{" unaccMs\""}}) {
        static_cast<void>(field);
    }
    check(source.find("\" beforeDrawMs=\" << t.beforeDrawMs") != std::string::npos,
          "报告里有 beforeDrawMs");
    check(source.find("\" pollMs=\" << t.pollMs") != std::string::npos, "报告里有 pollMs");
    check(source.find("\" afterDrawMs=\" << t.afterDrawMs") != std::string::npos,
          "报告里有 afterDrawMs");
    check(source.find("(t.cpuMs - t.beforeDrawMs - t.drawFrameMs - t.afterDrawMs)") !=
              std::string::npos,
          "unaccMs 是 cpuMs 减掉三段，一项不少——少减一项会稳定报出一个假的神秘时间");
    // cpuMs 必须被写进 trace，否则上面那个减法读的是一个恒为 0 的字段
    check(source.find("diag::frameTrace().cpuMs = frameMs;") != std::string::npos,
          "打印前把 frameMs 写进 trace.cpuMs，恒等式才有左边那一项");
}

} // namespace

int main() {
    const std::filesystem::path root{MC_REBEDROCK_SOURCE_DIR};
    const std::string source = readSourceFile(root / "src/render/vulkan/VulkanRenderer.cpp");
    check(!source.empty(), "读得到 VulkanRenderer.cpp");
    if (!source.empty()) {
        testPollTimerWrapsOnlyPollEvents(source);
        testBeforeDrawStartsAtIterationStart(source);
        testAfterDrawStartsWhenDrawFrameReturns(source);
        testReportCarriesAllFourAndTheIdentity(source);
        testPerfFrameBoundaryAndWorkCut(source);
    }
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "frame_cpu_accounting_test ok\n";
    return 0;
}

// UI-2：界面截图通道的纯逻辑。
//
// 通道本身要 GPU，这里测的是它**不需要 GPU 的那一半**：命令行解析与输出路径。
// 两者都直接关系到那条验收条件——"同一条命令行跑两遍逐字节相同"：
//   - 路径必须只是命令行的函数。哪怕文件名里混进一点运行期状态，第二遍就写到别处去了，
//     而 diff -r 会把它报成"只在一边存在的文件"，看起来像通道坏了。
//   - 参数写错必须抛。悄悄拍了另一个屏幕再退出 0，是自动化对照最坏的结果。

#include "render/UiCapture.hpp"

#include "ui/HudLayout.hpp"
#include "ui/MenuGeometry.hpp"
#include "ui/TitleScreenLayout.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        std::printf("ui_capture_test line %d: %s\n", line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

[[nodiscard]] std::optional<mc::render::UiCaptureOptions> parse(
    const std::vector<std::string_view>& arguments) {
    return mc::render::parseUiCaptureArguments(arguments);
}

// 解析这组参数必须抛，且抛的是 invalid_argument。
void expectThrows(const std::vector<std::string_view>& arguments, const char* what, int line) {
    try {
        static_cast<void>(mc::render::parseUiCaptureArguments(arguments));
    } catch (const std::invalid_argument&) {
        return;
    } catch (const std::exception& error) {
        std::printf("ui_capture_test line %d: %s threw the wrong type: %s\n", line, what,
                    error.what());
        ++failures;
        return;
    }
    std::printf("ui_capture_test line %d: %s did not throw\n", line, what);
    ++failures;
}

// 可变参数：参数表里带逗号的花括号初始化列表，单参数宏是接不住的。
#define EXPECT_THROWS(...)                                                                     \
    expectThrows(std::vector<std::string_view>__VA_ARGS__, #__VA_ARGS__, __LINE__)

// --- 1. 没有 --ui-shot 就不是一次截图运行 -------------------------------------
void testAbsent() {
    CHECK(!parse({}).has_value());
    CHECK(!parse({"--test-scene", "stone", "--export-preview"}).has_value());
    // 只给拍摄参数却不说拍什么：报错，而不是默默不拍。
    EXPECT_THROWS({"--ui-scale", "2"});
    EXPECT_THROWS({"--ui-size", "800x600"});
    EXPECT_THROWS({"--ui-out", "/tmp/x"});
}

// --- 2. 默认值 ---------------------------------------------------------------
void testDefaults() {
    const auto parsed = parse({"--ui-shot", "title"});
    CHECK(parsed.has_value());
    CHECK(parsed->pages.size() == 1U);
    CHECK(parsed->pages.front() == mc::ui::PageId::Title);
    // ★ 默认就拍两档：同一个屏幕在不同 GUI scale 下是不同的版面，只拍一档等于没拍。
    CHECK(parsed->guiScales.size() >= 2U);
    CHECK(parsed->width == 1280U);
    CHECK(parsed->height == 720U);
    CHECK(mc::render::uiCaptureImageCount(*parsed) ==
          parsed->pages.size() * parsed->guiScales.size());
}

// --- 3. 页名 ---------------------------------------------------------------
void testPageNames() {
    // 名字与 PageId 双向一致：一张表两个方向读，两边不可能各说各话。
    for (int raw = 0; raw <= static_cast<int>(mc::ui::PageId::Experimental); ++raw) {
        const auto page = static_cast<mc::ui::PageId>(raw);
        const auto name = mc::render::uiCapturePageName(page);
        CHECK(name != "unknown");
        const auto roundTrip = mc::render::uiCapturePageFromName(name);
        CHECK(roundTrip.has_value() && *roundTrip == page);
    }
    CHECK(!mc::render::uiCapturePageFromName("Title").has_value());     // 大小写敏感
    CHECK(!mc::render::uiCapturePageFromName("world_list").has_value()); // 分词用短横线

    const auto parsed = parse({"--ui-shot", "title,options", "--ui-shot", "language"});
    CHECK(parsed.has_value());
    CHECK(parsed->pages.size() == 3U);
    CHECK(parsed->pages[0] == mc::ui::PageId::Title);
    CHECK(parsed->pages[1] == mc::ui::PageId::Options);
    CHECK(parsed->pages[2] == mc::ui::PageId::Language);

    EXPECT_THROWS({"--ui-shot", "not-a-screen"});
    EXPECT_THROWS({"--ui-shot", "title,title"});            // 同一页给两次
    EXPECT_THROWS({"--ui-shot", "title", "--ui-shot", "title"});
    EXPECT_THROWS({"--ui-shot"});                            // 缺参数
    // 需要世界的页面拍出来不是命令行的函数，两次运行也不会一样：现在就说不行，
    // 而不是给出一张看着像成功的错图。
    EXPECT_THROWS({"--ui-shot", "game"});
    EXPECT_THROWS({"--ui-shot", "pause"});
    EXPECT_THROWS({"--ui-shot", "death"});
    EXPECT_THROWS({"--ui-shot", "loading"});
    EXPECT_THROWS({"--ui-shot", "title,pause"});
    CHECK(mc::render::uiCapturePageNeedsWorld(mc::ui::PageId::Pause));
    CHECK(!mc::render::uiCapturePageNeedsWorld(mc::ui::PageId::Title));
}

// --- 4. GUI 缩放档 -----------------------------------------------------------
void testScales() {
    const auto parsed = parse({"--ui-shot", "title", "--ui-scale", "1,4"});
    CHECK(parsed.has_value());
    // 第一次 --ui-scale 顶掉默认的两档，而不是追加在它们后面——否则会拍出没要的图。
    CHECK(parsed->guiScales.size() == 2U);
    CHECK(parsed->guiScales[0] == 1);
    CHECK(parsed->guiScales[1] == 4);

    // 之后的 --ui-scale 累加。
    const auto more = parse({"--ui-shot", "title", "--ui-scale", "1", "--ui-scale", "3"});
    CHECK(more.has_value());
    CHECK(more->guiScales.size() == 2U);
    CHECK(more->guiScales[0] == 1);
    CHECK(more->guiScales[1] == 3);

    // 0 是 Auto，合法。
    const auto autoScale = parse({"--ui-shot", "title", "--ui-scale", "0"});
    CHECK(autoScale.has_value() && autoScale->guiScales.size() == 1U &&
          autoScale->guiScales[0] == 0);

    EXPECT_THROWS({"--ui-shot", "title", "--ui-scale", "-1"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-scale", "99"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-scale", "two"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-scale", "2,2"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-scale", "2,"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-scale"});
}

// --- 5. 画布尺寸 -------------------------------------------------------------
void testSize() {
    const auto parsed = parse({"--ui-shot", "title", "--ui-size", "854x480"});
    CHECK(parsed.has_value() && parsed->width == 854U && parsed->height == 480U);

    // 下界是 spec §1.1 的最小逻辑画布 320x240：比它更小的窗口撑不住任何一档缩放。
    EXPECT_THROWS({"--ui-shot", "title", "--ui-size", "319x480"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-size", "854x239"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-size", "9000x480"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-size", "854"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-size", "854x"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-size", "x480"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-size", "854x480x2"});
    EXPECT_THROWS({"--ui-shot", "title", "--ui-size"});
}

// --- 6. 输出路径只是命令行的函数 ---------------------------------------------
void testPaths() {
    auto parsed = parse({"--ui-shot", "title,options", "--ui-scale", "2,3", "--ui-out",
                         "/tmp/shots"});
    CHECK(parsed.has_value());
    CHECK(mc::render::uiCaptureImageCount(*parsed) == 4U);

    const auto titleAtTwo =
        mc::render::uiCaptureImagePath(*parsed, mc::ui::PageId::Title, 2);
    CHECK(titleAtTwo == std::filesystem::path{"/tmp/shots/title/scale-2.png"});
    CHECK(mc::render::uiCaptureImagePath(*parsed, mc::ui::PageId::Options, 3) ==
          std::filesystem::path{"/tmp/shots/options/scale-3.png"});
    // Auto 档要有自己的名字，否则它会和 "scale-0" 撞在一起看不出是哪一档。
    CHECK(mc::render::uiCaptureImagePath(*parsed, mc::ui::PageId::Title, 0) ==
          std::filesystem::path{"/tmp/shots/title/scale-auto.png"});

    // 同一条命令行解析两遍，得到完全相同的参数与路径——确定性从这里就开始。
    const auto again = parse({"--ui-shot", "title,options", "--ui-scale", "2,3", "--ui-out",
                              "/tmp/shots"});
    CHECK(again.has_value() && *again == *parsed);
    CHECK(mc::render::uiCaptureImagePath(*again, mc::ui::PageId::Title, 2) == titleAtTwo);

    // 每一张图的路径互不相同：页与档都进了路径，所以四张图落在四个位置。
    std::vector<std::filesystem::path> written;
    for (const auto page : parsed->pages) {
        for (const int scale : parsed->guiScales) {
            written.push_back(mc::render::uiCaptureImagePath(*parsed, page, scale));
        }
    }
    CHECK(written.size() == mc::render::uiCaptureImageCount(*parsed));
    for (std::size_t i = 0; i < written.size(); ++i) {
        for (std::size_t j = i + 1U; j < written.size(); ++j) {
            CHECK(written[i] != written[j]);
        }
    }
}

// --- 7. determinism knobs --------------------------------------------------
//
// ★ 先说清这条测试为什么必须存在，别把它当成凑数的源码扫描：
//
// "同一条命令行跑两遍逐字节相同"是这条通道的验收条件，而它在本容器里**通不过**
// 大部分 knob 的 sabotage —— 实测：把光标那一钉删掉，两遍的图仍然逐字节相同
// （Xvfb + 隐藏窗口下 GLFW 读回的光标位置本来就恒定；即使用 XWarpPointer 在两遍
// 之间把指针挪到"单人游戏"按钮上，图还是一样）。UI 时钟同理：拍摄循环自己从不推进它，
// 一个全新进程里它本来就是 0。
//
// 也就是说，两遍比对能证明"这条通道现在是确定的"，但**证不了"每个 knob 都还在"**。
// 而 knob 防的是别的情形：可见窗口、别的平台、或者将来从主循环之后再驱动一次拍摄——
// 那时删掉的那一钉会让图片开始漂移，而两遍比对届时才发现就太晚了。
//
// 所以这里用两层：一条对光标那个点的**性质断言**（它必须落在所有控件之外，否则
// 钉了也白钉），加一条列出每个 knob 的源码护栏（少钉一个就红）。
void testDeterminismKnobs() {
    // 性质：钉住的那个点在任何画布、任何缩放档下都不落在任何一个主菜单控件上。
    // 这才是"钉光标"要达到的效果——不是"钉住就行"，而是"钉住之后没有控件悬停"。
    const mc::ui::UiPoint pinned{mc::render::kUiCaptureCursorX, mc::render::kUiCaptureCursorY};
    for (const auto canvas : {std::pair{1280.0F, 720.0F}, std::pair{854.0F, 480.0F},
                              std::pair{1281.0F, 721.0F}, std::pair{640.0F, 480.0F}}) {
        for (int guiScale = 0; guiScale <= 4; ++guiScale) {
            const mc::ui::HudLayout layout{canvas.first, canvas.second, guiScale};
            for (std::size_t index = 0; index < mc::ui::kTitleWidgetCount; ++index) {
                const auto rect = mc::ui::frontendButtonRect(layout, mc::ui::PageId::Title, index,
                                                             mc::ui::kTitleWidgetCount);
                CHECK(!rect.contains(pinned.x, pinned.y));
            }
        }
    }
}

// 源码护栏：拍摄路径必须钉住下面每一个逐帧变化的量。
// 少钉一个不改变任何函数的返回值，也（在本环境下）不改变图片——只能这么抓。
void testKnobsAreAllPinned() {
    std::ifstream input{MC_REBEDROCK_RENDERER_SRC, std::ios::binary};
    if (!input) {
        std::printf("ui_capture_test: cannot open %s\n", MC_REBEDROCK_RENDERER_SRC);
        ++failures;
        return;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    const auto signature = source.find("void applyUiCaptureDeterminism()");
    if (signature == std::string::npos) {
        std::printf("ui_capture_test: applyUiCaptureDeterminism not found — if it was renamed, "
                    "move this guard with it rather than deleting it\n");
        ++failures;
        return;
    }
    const auto open = source.find('{', signature);
    std::string body;
    int depth = 0;
    for (std::size_t index = open; index < source.size() && open != std::string::npos; ++index) {
        if (source[index] == '{') {
            ++depth;
        } else if (source[index] == '}') {
            --depth;
            if (depth == 0) {
                body = source.substr(open, index - open + 1U);
                break;
            }
        }
    }
    CHECK(!body.empty());

    // 每一条都写清它不钉会怎样，删掉哪一条这里就红哪一条。
    // UI 时钟：全景相机的偏航与俯仰、文本光标的闪烁相位都是它的函数
    CHECK(body.find("uiTimeSeconds = kUiCaptureClockSeconds") != std::string::npos);
    // 光标：按钮的悬停高亮读它
    CHECK(body.find("pinnedCursor = ui::UiPoint{kUiCaptureCursorX, kUiCaptureCursorY}") !=
          std::string::npos);
    // 按下态：上一次输入留下的按下按钮会被画成按下的样子
    CHECK(body.find("pressedMenuButton = ui::WidgetId::None") != std::string::npos);
    // 天气与视角摇晃：两者都经 HUD 通道影响画面
    CHECK(body.find("setWeather(") != std::string::npos);
    CHECK(body.find("options.viewBobbing = false") != std::string::npos);
    // 插值权重：世界静止而它不是，且它喂给视图矩阵
    CHECK(body.find("renderInterpolationAlpha = 0.0F") != std::string::npos);
    // 提示条与聊天：两者都带时间戳，会随运行时刻淡出
    CHECK(body.find("toastQueue_.clear()") != std::string::npos);
    CHECK(body.find("chatHistory.clear()") != std::string::npos);

    // ★ 导出**从不写 options.properties**。
    //
    // 这条不是洁癖：UI-2 落地后确实发生过——一次 1280x720 的界面截图把 `window.width`
    // 从 640 改成了 1280（隐藏窗口的尺寸经 `noteWindowSizeChanged` 进了 options，退出时存盘），
    // 于是下一次拍摄读到的是上一次留下的窗口尺寸，视频设置页的 "Fullscreen Resolution"
    // 标签跟着变，两组基线不可比。拍摄是一次测量，不是一场游戏。
    const auto persist = source.find("void persistOptions() noexcept");
    CHECK(persist != std::string::npos);
    if (persist != std::string::npos) {
        const std::string persistBody = source.substr(persist, 1200U);
        const auto guard = persistBody.find("if (uiCapture.has_value() ||");
        CHECK(guard != std::string::npos);
        // 豁免必须在**写任何字段之前**：先改 options 再 return 一样会留下脏值
        const auto firstWrite = persistBody.find("options.guiScale =");
        CHECK(firstWrite != std::string::npos && guard < firstWrite);
    }

    // 各向异性 / 抗锯齿 / 垂直同步是**初始化期读一次**的，钉在 initialize() 的开头，
    // 不在这个函数里。它们决定采样器与管线，晚一步钉就没用了。
    const auto initialize = source.find("void initialize()");
    CHECK(initialize != std::string::npos);
    const auto capturePin = source.find("if (uiCapture.has_value()) {", initialize);
    CHECK(capturePin != std::string::npos && capturePin - initialize < 1500U);
    const std::string prologue = source.substr(initialize, 1500U);
    CHECK(prologue.find("options.anisotropy = 1") != std::string::npos);
    CHECK(prologue.find("options.antiAliasing = false") != std::string::npos);
    CHECK(prologue.find("options.vsync = false") != std::string::npos);
}

} // namespace

int main() {
    testAbsent();
    testDefaults();
    testPageNames();
    testScales();
    testSize();
    testPaths();
    testDeterminismKnobs();
    testKnobsAreAllPinned();
    if (failures != 0) {
        std::printf("ui_capture_test: %d checks failed\n", failures);
        return 1;
    }
    return 0;
}

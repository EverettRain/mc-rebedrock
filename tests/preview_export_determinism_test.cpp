// 出图必须钉死每一档会改变画面的视频设置——包括**间接**生效的那些。
//
// 现场：所有八角出图都是糊的。整张 512x512 的图里相邻像素最大跃变只有 **5/255**，
// 连天空与石地板的交界都要 25 个像素才过渡完；关掉罪魁之后同一张图是 **78/255**。
// 症状不像「模糊」，像「渲染得比较柔」，所以它在一批又一批出图里活了下来。
//
// 链条是这样接起来的：导出为了冻住世界（顺带藏掉 HUD）把 `paused` 置真 →
// `HudRenderer::screenOpen()` 把 `paused` 也算作「有界面打开」→
// `screenBackground(PageId::Game, worldOpen=true, …)` 落到 `InWorldBlur` →
// `recordMenuBackground` 对整帧跑六趟 box blur，半径取用户设置里的默认值 5。
//
// `applyPreviewDeterminism()` 逐条列过视频设置（各向异性、抗锯齿、垂直同步、视角摇晃、
// 实体贴花……），唯独漏了这一档——因为它不是被渲染世界那一趟读走的，是被**界面**那一趟
// 读走的，而导出「没有界面」。
//
// 后果不只是难看：凡是问「边缘」的判断——阴影边、AO 边、UV 接缝、图标朝向——在这样的
// 图上一律得不出结论，而这个仓库的视觉验收全建立在这些图上。
//
// 这条测试把那条链子的三节各钉一次，而不是只断言「源码里有那一行」。
//
// TAA-1 之后它钉的是**两条**链，而且第二条的失效方式与第一条不同：
// menuBackgroundBlurriness 漏掉是因为它**间接**生效；抗锯齿漏掉是因为它**早于钉死时刻**
// 生效（初始化期读一次，交换链与管线都按它建）。两次的表现同样是「看起来没问题」。
// 下半场那一节因此钉的是位置——「在 glfwInit 之前」——而不是「在某个函数体里」。

#include "config/GameOptions.hpp"
#include "render/TemporalAntiAliasing.hpp"
#include "ui/OptionCycle.hpp"
#include "ui/ScreenBackground.hpp"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#ifndef MC_REBEDROCK_RENDERER_SRC
#error "MC_REBEDROCK_RENDERER_SRC must point at src/render/vulkan/VulkanRenderer.cpp"
#endif

namespace {

int failures = 0;

void require(bool condition, std::string_view what) {
    if (!condition) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream{path};
    if (!stream) {
        std::cerr << "FAILED: cannot read " << path.string() << "\n";
        ++failures;
        return {};
    }
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

[[nodiscard]] std::string stripComments(std::string_view source) {
    std::string out;
    out.reserve(source.size());
    for (std::size_t i = 0; i < source.size();) {
        if (source.compare(i, 2, "//") == 0) {
            while (i < source.size() && source[i] != '\n') ++i;
        } else if (source.compare(i, 2, "/*") == 0) {
            i += 2;
            while (i + 1 < source.size() && source.compare(i, 2, "*/") != 0) ++i;
            i = i + 2 < source.size() ? i + 2 : source.size();
        } else {
            out.push_back(source[i]);
            ++i;
        }
    }
    return out;
}

[[nodiscard]] std::string bodyOf(std::string_view cleanSource, std::string_view marker) {
    const auto start = cleanSource.find(marker);
    if (start == std::string_view::npos) {
        std::cerr << "FAILED: could not find `" << marker << "`\n";
        ++failures;
        return {};
    }
    const auto open = cleanSource.find('{', start);
    if (open == std::string_view::npos) {
        return {};
    }
    int depth = 0;
    for (std::size_t i = open; i < cleanSource.size(); ++i) {
        if (cleanSource[i] == '{') ++depth;
        if (cleanSource[i] == '}') {
            --depth;
            if (depth == 0) {
                return std::string{cleanSource.substr(open + 1, i - open - 1)};
            }
        }
    }
    std::cerr << "FAILED: unbalanced braces after `" << marker << "`\n";
    ++failures;
    return {};
}

} // namespace

int main() {
    // ---- 第一节：导出所处的那个状态，本身是会模糊的一档 -------------------
    //
    // 导出把 `paused` 置真而页面留在 Game，`screenOpen()` 因此为真，
    // `currentBackgroundKind()` 只看页面与「世界开着没有」。
    const auto exportKind =
        mc::ui::screenBackground(mc::ui::PageId::Game, /*worldOpen=*/true, /*containerOpen=*/false);
    const bool stateBlurs = mc::ui::backgroundIsBlurred(exportKind);

    // ---- 第二节：默认设置下这一档是开着的 ---------------------------------
    const mc::config::GameOptions defaults{};
    const bool defaultBlurs = mc::ui::menuBlurEnabled(defaults.menuBackgroundBlurriness);
    require(!mc::ui::menuBlurEnabled(0), "0 必须是「不模糊」，否则下面钉的那个值没有意义");

    // ---- 第三节：因此导出必须显式把它按到 0 -------------------------------
    const std::string clean = stripComments(readFile(MC_REBEDROCK_RENDERER_SRC));
    const std::string body = bodyOf(clean, "void applyPreviewDeterminism()");
    require(!body.empty(), "必须解析出 applyPreviewDeterminism 的函数体");

    if (stateBlurs && defaultBlurs) {
        require(body.find("options.menuBackgroundBlurriness = 0;") != std::string::npos,
                "导出必须把 menuBackgroundBlurriness 钉成 0——它所处的状态是 InWorldBlur，"
                "而默认设置下这一档是开着的，于是每一张出图都会被整帧模糊");
        std::cout << "  导出状态会模糊、默认档开着 ⇒ 已检查那一行确实在\n";
    } else {
        // 链条断了也要说话：断在哪一节，决定了那一行还需不需要留着。
        std::cout << "  注意：导出状态是否模糊=" << stateBlurs << "，默认档是否开着="
                  << defaultBlurs << "——链条已断，重新审视那条钉死\n";
    }

    // ---- 顺带把同一函数里其余几档也钉住 -----------------------------------
    //
    // 它们本来就在，这里列出来是为了让「哪些设置被钉死」有一个能被读的清单：
    // 少一档就是又一次「图片取决于跑它的那台机器」。
    for (const auto* pinned : {"options.viewBobbing = false;", "options.entityShadows = true;",
                               "options.menuBackgroundBlurriness = 0;"}) {
        require(body.find(pinned) != std::string::npos,
                std::string{"applyPreviewDeterminism 必须钉死 "} + pinned);
    }

    // ================= TAA-1：同一个陷阱的第二次现身 =========================
    //
    // menuBackgroundBlurriness 漏掉，是因为它**间接**生效（被界面那一趟读走，
    // 而"导出没有界面"）。TAA 漏掉的方式不同也更狠：它让画面成为「拍之前跑了几帧」
    // 的函数，而导出正是先跑 kPreviewWarmupFrames 帧再截图。
    // 实测（离屏，八角夹具，512²）：TAA 开着时 warmup 8 与 warmup 24 拍出来的同一张
    // 图有 5.10% 的像素不同（最大 Δ=24）；关着时两者**逐字节相同**。
    //
    // 而且它的钉死位置也和上一次不同：抗锯齿是**初始化期读一次**的（交换链、
    // 渲染通道、管线都按它建），钉在 applyPreviewDeterminism 里是一句空话——
    // 那个函数跑的时候东西早就建好了。这一节因此钉的是「在 glfwInit 之前」。
    {
        // ---- 第一节：TAA 这一档确实吃历史 ---------------------------------
        mc::render::TemporalFrameState state;
        state.advance();
        const bool eatsHistory = state.historyWeight() > 0.0F;
        require(eatsHistory,
                "TAA 一旦有了上一帧就必须真的吃历史，否则下面两节钉的东西不存在");

        // ---- 第二节：导出确实要先跑好几帧再拍 -----------------------------
        int warmupFrames = 0;
        const auto warmupAt = clean.find("kPreviewWarmupFrames = ");
        require(warmupAt != std::string::npos, "必须找得到导出的预热帧数");
        if (warmupAt != std::string::npos) {
            const char* first = clean.data() + warmupAt + std::string_view{"kPreviewWarmupFrames = "}.size();
            std::from_chars(first, clean.data() + clean.size(), warmupFrames);
        }
        require(warmupFrames > 1,
                "导出要跑不止一帧再截图——正是这一点让「吃历史」变成「图片取决于跑了几帧」");

        // ---- 第三节：TAA 是用户真能选到的一档 -----------------------------
        const mc::ui::OptionDesc* antiAliasing =
            mc::ui::findCyclingOption(mc::ui::WidgetId::AntiAliasing);
        require(antiAliasing != nullptr, "抗锯齿必须还是一个循环选项");
        bool reachableTaa = false;
        if (antiAliasing != nullptr) {
            for (const mc::ui::OptionValue& value : antiAliasing->values) {
                reachableTaa = reachableTaa ||
                               value.value == static_cast<int>(mc::config::AntiAliasingMode::Taa);
            }
        }
        require(reachableTaa, "TAA 必须是取值表里够得着的一档，否则不必钉");

        // ---- 结论：导出必须在**建 Vulkan 之前**把它钉成 Off ---------------
        const auto initialize = clean.find("void initialize()");
        const auto glfwInitCall = clean.find("glfwInit()", initialize);
        require(initialize != std::string::npos && glfwInitCall != std::string::npos,
                "必须找得到 initialize() 与它里面的 glfwInit()");
        if (eatsHistory && warmupFrames > 1 && reachableTaa && initialize != std::string::npos &&
            glfwInitCall != std::string::npos) {
            const std::string prologue = clean.substr(initialize, glfwInitCall - initialize);
            // RN-44：钉死的含义是「取值是命令行的函数，与机器上的 options.properties
            // 无关」，不是「只能是 Off」。TAA 吃历史没错，但导出从固定初态跑固定帧数，
            // 同参数两次运行仍然逐字节相同；而**档位进目录名**（test_scene_test 钉着），
            // 所以 TAA 那一版不会静默覆盖 off 那一版
            require(prologue.find(
                        "options.antiAliasing = testScene.has_value() ? testScene->antiAliasing") !=
                        std::string::npos,
                    "导出必须在 glfwInit 之前把抗锯齿钉成命令行给的那一档——它是初始化期"
                    "读一次的，钉晚了一个字节都改不动已经建好的交换链与管线");
            require(prologue.find("options.antiAliasing = options.") == std::string::npos,
                    "抗锯齿档绝不能取自持久化的那份配置");
            require(prologue.find(": config::AntiAliasingMode::Off;") != std::string::npos,
                    "没有 testScene 的那条路（界面截图）仍然必须落在 Off");
            // 光有那一行不够：它必须真的管到**方块预览导出**，而不是只管界面截图。
            // 那正是本轮查出来的缺陷——导出从来没进过这个条件
            require(prologue.find("testScene->exportPreview") != std::string::npos,
                    "那一段必须覆盖方块预览导出，不能只覆盖界面截图");
            std::cout << "  TAA 吃历史、导出跑 " << warmupFrames
                      << " 帧再拍、这一档用户够得着 ⇒ 已检查它在 glfwInit 之前被钉成 Off\n";
        } else {
            std::cout << "  注意：吃历史=" << eatsHistory << "，预热帧数=" << warmupFrames
                      << "，TAA 可达=" << reachableTaa << "——链条已断，重新审视那条钉死\n";
        }
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "preview_export_determinism ok\n";
    return 0;
}

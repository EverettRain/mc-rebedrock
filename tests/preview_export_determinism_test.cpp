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

#include "config/GameOptions.hpp"
#include "ui/ScreenBackground.hpp"

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

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "preview_export_determinism ok\n";
    return 0;
}

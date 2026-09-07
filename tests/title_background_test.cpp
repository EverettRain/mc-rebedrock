// UI-2：主菜单背景里**不允许有代码写死的变暗**。
//
// 26.1 的主菜单是清晰的：`TitleScreen.extractBackground()` 是空实现，全景之后只有
// `Panorama.extractRenderState` 那一次 panorama_overlay 全屏 blit，而那张图在 26.1 的
// 资源包里是 1x1、alpha 恒 0 的全透明图。也就是说变暗要么来自资源包给的纹理，要么
// 根本不存在——它绝不该来自代码里的一个常数。
//
// 本仓从前恰恰有一个：drawTitleCarousel 的未模糊分支画一块 30% 的全屏黑四边形，
// 注释理由是"保证白色标题和菜单按钮在场景上仍然清晰"。GUI spec §6.3 明说的是
// "不模糊、不加菜单遮罩"。
//
// 这件事很难被断言抓住，因为"多画了一个四边形"不改变任何函数的返回值。所以这里用两层：
//   1. 配方本身是一个 constexpr 纯函数（titleBackgroundLayer），绘制侧只是照着铺。
//      改公式（比如把 tint 的 alpha 调下来）在第一组断言里红。
//   2. 一条源码护栏：绘制函数体内不得再出现整屏的纯色四边形。绕过公式、在循环外
//      另加一次绘制调用，在第二组里红。RN-25 用过同样的手法（它那条护栏是 :139）。

#include "render/vulkan/HudTypes.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef MC_REBEDROCK_HUD_RENDERER_SRC
#error "MC_REBEDROCK_HUD_RENDERER_SRC must point at src/render/vulkan/HudRenderer.hpp"
#endif

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        std::printf("title_background_test line %d: %s\n", line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

// --- 1. 配方 -----------------------------------------------------------------
void testRecipe() {
    using mc::render::titleBackgroundLayer;
    const auto clear = titleBackgroundLayer(/*blurred=*/false);
    const auto blurred = titleBackgroundLayer(/*blurred=*/true);

    // 主菜单：panorama_overlay，整层拉满，白色不透明。
    CHECK(clear.guiLayer == mc::render::kPanoramaOverlayGuiLayer);
    CHECK(!clear.tiled);
    // 二级界面：menu_background，按 32 逻辑像素平铺。
    CHECK(blurred.guiLayer == mc::render::kMenuBackgroundGuiLayer);
    CHECK(blurred.tiled);
    CHECK(clear.guiLayer != blurred.guiLayer);

    // ★ 两条分支的 tint 都必须是白色不透明。
    //   把任何一个分量调低，就是"用代码而不是用纹理去变暗"，也就是被删掉的那个缺陷。
    for (const auto& layer : {clear, blurred}) {
        CHECK(layer.tint.r == 1.0F);
        CHECK(layer.tint.g == 1.0F);
        CHECK(layer.tint.b == 1.0F);
        CHECK(layer.tint.a == 1.0F);
    }

    // 层号不能和别的层撞车：撞了就会铺出另一张图，而画面上看起来只是"颜色不太对"。
    CHECK(mc::render::kPanoramaOverlayGuiLayer != mc::render::kVignetteGuiLayer);
    CHECK(mc::render::kPanoramaOverlayGuiLayer != mc::render::kScreenDimGuiLayer);
    CHECK(mc::render::kPanoramaOverlayGuiLayer != mc::render::kMenuListBackgroundGuiLayer);
    CHECK(mc::render::kPanoramaOverlayGuiLayer != mc::render::kTooltipGuiLayer);
    CHECK(mc::render::kMenuBackgroundGuiLayer != mc::render::kMenuListBackgroundGuiLayer);
}

// --- 2. 源码护栏 --------------------------------------------------------------
[[nodiscard]] std::string readHudRenderer() {
    std::ifstream input{MC_REBEDROCK_HUD_RENDERER_SRC, std::ios::binary};
    if (!input) {
        std::printf("title_background_test: cannot open %s\n", MC_REBEDROCK_HUD_RENDERER_SRC);
        ++failures;
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

// 去掉 `//` 行注释，免得只在说明文字里提到的调用被当成一次真的绘制。
[[nodiscard]] std::string stripLineComments(const std::string& source) {
    std::string result;
    result.reserve(source.size());
    std::istringstream lines{source};
    std::string line;
    while (std::getline(lines, line)) {
        const auto comment = line.find("//");
        result += comment == std::string::npos ? line : line.substr(0, comment);
        result += '\n';
    }
    return result;
}

// drawTitleCarousel 的函数体，从签名到与它同缩进的收尾大括号。
[[nodiscard]] std::string carouselBody(const std::string& source) {
    const auto signature = source.find("void drawTitleCarousel(");
    if (signature == std::string::npos) {
        std::printf("title_background_test: drawTitleCarousel not found — did it get renamed? "
                    "This guard must be moved with it, not deleted.\n");
        ++failures;
        return {};
    }
    const auto open = source.find('{', signature);
    if (open == std::string::npos) {
        ++failures;
        return {};
    }
    int depth = 0;
    for (std::size_t index = open; index < source.size(); ++index) {
        if (source[index] == '{') {
            ++depth;
        } else if (source[index] == '}') {
            --depth;
            if (depth == 0) {
                return source.substr(open, index - open + 1U);
            }
        }
    }
    ++failures;
    return {};
}

void testNoHandWrittenDim() {
    const std::string body = carouselBody(stripLineComments(readHudRenderer()));
    if (body.empty()) {
        return;
    }
    // 全景之后铺的那一层必须经配方来，而不是自己拼一个层号或一个颜色。
    CHECK(body.find("titleBackgroundLayer(") != std::string::npos);

    // ★ 核心护栏：函数体里不得再有 drawHudQuad。
    //   那是画纯色四边形的入口，被删掉的 30% 黑遮罩就是它的一次调用；
    //   背景该走的是 drawGuiSprite（贴资源包给的纹理）。
    CHECK(body.find("drawHudQuad(") == std::string::npos);

    // 顺带钉住"整屏 + 纯色"这个组合：即使换个函数名，写死的 alpha 常量也不该出现在这里。
    CHECK(body.find("0.30F") == std::string::npos);
    CHECK(body.find("0.3F") == std::string::npos);
}

} // namespace

int main() {
    testRecipe();
    testNoHandWrittenDim();
    if (failures != 0) {
        std::printf("title_background_test: %d checks failed\n", failures);
        return 1;
    }
    return 0;
}

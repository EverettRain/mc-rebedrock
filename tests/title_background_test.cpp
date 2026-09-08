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

#include <array>
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
    using Kind = mc::ui::ScreenBackgroundKind;
    const auto clear = titleBackgroundLayer(Kind::PanoramaClear);
    const auto blurred = titleBackgroundLayer(Kind::PanoramaBlur);
    const auto inworld = titleBackgroundLayer(Kind::InWorldBlur);

    // 主菜单：panorama_overlay，整层拉满，白色不透明。
    CHECK(clear.guiLayer == mc::render::kPanoramaOverlayGuiLayer);
    CHECK(!clear.tiled);
    // 无世界的二级界面：menu_background，按 32 逻辑像素平铺。
    CHECK(blurred.guiLayer == mc::render::kMenuBackgroundGuiLayer);
    CHECK(blurred.tiled);
    CHECK(clear.guiLayer != blurred.guiLayer);
    // ★ UI-5：有世界时铺的是 inworld 那张，不是同一张
    // （Screen.java:450 `level == null ? MENU_BACKGROUND : INWORLD_MENU_BACKGROUND`）。
    // 两张都平铺、都白色不透明，差别只在像素——所以只有层号能把它们分开。
    CHECK(inworld.guiLayer == mc::render::kInworldMenuBackgroundGuiLayer);
    CHECK(inworld.tiled);
    CHECK(inworld.guiLayer != blurred.guiLayer);
    // 渐变两档不铺遮罩：真走到这里也必须落在全透明的 panorama_overlay 上
    CHECK(titleBackgroundLayer(Kind::Transparent).guiLayer ==
          mc::render::kPanoramaOverlayGuiLayer);
    CHECK(titleBackgroundLayer(Kind::RedGradient).guiLayer ==
          mc::render::kPanoramaOverlayGuiLayer);

    // ★ 三条分支的 tint 都必须是白色不透明。
    //   把任何一个分量调低，就是"用代码而不是用纹理去变暗"，也就是被删掉的那个缺陷。
    for (const auto& layer : {clear, blurred, inworld}) {
        CHECK(layer.tint.r == 1.0F);
        CHECK(layer.tint.g == 1.0F);
        CHECK(layer.tint.b == 1.0F);
        CHECK(layer.tint.a == 1.0F);
    }

    // 列表底衬与分隔线同样分 inworld 与非 inworld 两套
    CHECK(mc::render::menuListBackgroundLayer(false) ==
          mc::render::kMenuListBackgroundGuiLayer);
    CHECK(mc::render::menuListBackgroundLayer(true) ==
          mc::render::kInworldMenuListBackgroundGuiLayer);
    CHECK(mc::render::headerSeparatorSpriteY(false) != mc::render::headerSeparatorSpriteY(true));
    CHECK(mc::render::footerSeparatorSpriteY(false) != mc::render::footerSeparatorSpriteY(true));
    // 四张分隔精灵在同一层里各占 2 行，不重叠
    CHECK(mc::render::kFooterSeparatorSpriteY ==
          mc::render::kHeaderSeparatorSpriteY + mc::render::kListSeparatorSpriteHeight);
    CHECK(mc::render::kInworldHeaderSeparatorSpriteY ==
          mc::render::kFooterSeparatorSpriteY + mc::render::kListSeparatorSpriteHeight);
    CHECK(mc::render::kInworldFooterSeparatorSpriteY ==
          mc::render::kInworldHeaderSeparatorSpriteY + mc::render::kListSeparatorSpriteHeight);
}

// 层号不能撞车：撞了就会铺出另一张图，而画面上看起来只是"颜色不太对"。
//
// UI-5 把这一条从"挑几对来比"改成**全部两两比**。上一版列的是四对；
// 这一轮新增三个层号，任何一个写成已被占用的数字都不在那四对里。
void testLayerNumbersAreDistinct() {
    const std::array named{
        mc::render::kMenuBackgroundGuiLayer,
        mc::render::kVignetteGuiLayer,
        mc::render::kInworldMenuBackgroundGuiLayer,
        mc::render::kMenuListBackgroundGuiLayer,
        mc::render::kEnchantingGuiLayer,
        mc::render::kAnvilGuiLayer,
        mc::render::kTooltipGuiLayer,
        mc::render::kPanoramaOverlayGuiLayer,
        mc::render::kInworldMenuListBackgroundGuiLayer,
        mc::render::kListSeparatorGuiLayer,
    };
    for (std::size_t a = 0; a < named.size(); ++a) {
        // 每一层都必须落在图集里。createGuiTexture() 的 static_assert 只钉住
        // 「数组长度 == kGuiLayerCount」，钉不住「某个常量指到了数组之外」。
        CHECK(named[a] >= 0.0F);
        CHECK(named[a] < 20.0F);   // kGuiLayerCount
        for (std::size_t b = a + 1; b < named.size(); ++b) {
            check(named[a] != named[b],
                  "GUI layer numbers collide: " + std::to_string(named[a]), __LINE__);
        }
    }
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

// 某个成员函数的函数体，从签名到配对的收尾大括号。
[[nodiscard]] std::string functionBody(const std::string& source, const std::string& signature_) {
    const auto signature = source.find(signature_);
    if (signature == std::string::npos) {
        std::printf("title_background_test: %s not found — did it get renamed? "
                    "This guard must be moved with it, not deleted.\n", signature_.c_str());
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
    const std::string source = stripLineComments(readHudRenderer());
    // UI-5 把 drawTitleCarousel 拆成了两半：全景（模糊之前）与遮罩层（模糊之后）。
    // 护栏跟着搬到这两半上，再加上按档位表分派的那一层——三处都不许自己拼一块纯色。
    const std::string tile = functionBody(source, "void drawMenuBackgroundTile(");
    const std::string panorama = functionBody(source, "void drawMenuPanorama(");
    const std::string dispatch = functionBody(source, "void drawScreenBackground(");
    if (tile.empty() || panorama.empty() || dispatch.empty()) {
        return;
    }
    // 模糊之后铺的那一层必须经配方来，而不是自己拼一个层号或一个颜色。
    CHECK(tile.find("titleBackgroundLayer(") != std::string::npos);
    // 分派必须问档位表，而不是自己按页面 id 判断。
    CHECK(dispatch.find("backgroundTilesMenuTexture(") != std::string::npos);
    CHECK(dispatch.find("backgroundGradient(") != std::string::npos);

    // ★ 核心护栏：这三个函数体里都不得有 drawHudQuad。
    //   那是画纯色四边形的入口，被删掉的 30% 黑遮罩就是它的一次调用；
    //   背景该走的是 drawGuiSprite（贴资源包给的纹理）或 drawVerticalGradient
    //   （vanilla 的 fillGradient，两个色标来自 ui::backgroundGradient）。
    for (const auto* body : {&tile, &panorama, &dispatch}) {
        CHECK(body->find("drawHudQuad(") == std::string::npos);
        // 顺带钉住"整屏 + 纯色"这个组合：写死的 alpha 常量不该出现在这里。
        CHECK(body->find("0.30F") == std::string::npos);
        CHECK(body->find("0.3F") == std::string::npos);
        // ★ UI-5：死亡屏那块 rgba(0.25, 0, 0, 0.58) 的平色也不许回来。
        CHECK(body->find("0.58F") == std::string::npos);
    }

    // ★ UI-5：全景那一趟不得再自己做模糊。从前 PanoramaPush 带一个 blur 半径，
    //   panorama.frag 里是个 5x5 盒式近似——它只能糊全景自己，糊不了世界。
    //   真正的模糊是 MenuBlur 那六趟整帧后处理。
    CHECK(panorama.find("blur") == std::string::npos);
    CHECK(panorama.find("Blur") == std::string::npos);
}

} // namespace

int main() {
    testRecipe();
    testLayerNumbersAreDistinct();
    testNoHandWrittenDim();
    if (failures != 0) {
        std::printf("title_background_test: %d checks failed\n", failures);
        return 1;
    }
    return 0;
}

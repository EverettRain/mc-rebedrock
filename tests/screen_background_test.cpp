// UI-5：背景档位表与渐变色标（GUI spec §3）。
//
// 这里钉住的是三件在画面上一眼看不出、退回去也不会崩的事：
//
//   1. 26.1 的 `Screen.extractBackground()` 是**三分支**，spec §3.2 只写了两分支。
//      容器/背包走 `extractTransparentBackground` —— 一层灰渐变、**不模糊**。
//      照 spec 实现会把背包背景也做成模糊，而那看起来"挺像回事"。
//   2. 死亡屏的第二个色标是 `0xA0803030`，spec §9.3 写的是 `0x80500000`。
//      两个都是暗红，肉眼分不出，但一个是 vanilla 一个不是。
//   3. 模糊强度是**整数档 0..10、默认 5**，0 那一档等于关闭。
//
// 出处（Mojang 映射，`/workspace/mc-26.1-java/minecraft-src`）：
//   Screen.java:419-429 / 434-440 / 467
//   DeathScreen.java:134-136   fillGradient(0,0,w,h, 1615855616, -1602211792)
//   Options.java:271-278       IntRange(0,10)，默认 5
//   GameRenderer.java:104      MAX_BLUR_RADIUS = 10

#include "ui/PageStack.hpp"
#include "ui/ScreenBackground.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef MC_REBEDROCK_MENU_BLUR_SRC
#error "MC_REBEDROCK_MENU_BLUR_SRC must point at src/render/vulkan/MenuBlur.cpp"
#endif

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        std::printf("screen_background_test line %d: %s\n", line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

using mc::ui::PageId;
using Kind = mc::ui::ScreenBackgroundKind;

// --- 1. 三分支，不是两分支 ---------------------------------------------------
void testKindTable() {
    // 主菜单：全景清晰。26.1 的 TitleScreen.extractBackground() 是空实现。
    CHECK(mc::ui::screenBackground(PageId::Title, false, false) == Kind::PanoramaClear);

    // 无世界的二级界面：全景 + 模糊 + menu_background
    CHECK(mc::ui::screenBackground(PageId::Options, false, false) == Kind::PanoramaBlur);
    CHECK(mc::ui::screenBackground(PageId::Language, false, false) == Kind::PanoramaBlur);
    CHECK(mc::ui::screenBackground(PageId::WorldList, false, false) == Kind::PanoramaBlur);

    // 有世界的界面：世界画面 + 模糊 + inworld_menu_background
    CHECK(mc::ui::screenBackground(PageId::Pause, true, false) == Kind::InWorldBlur);
    CHECK(mc::ui::screenBackground(PageId::Options, true, false) == Kind::InWorldBlur);

    // ★ 容器类是第三条分支：灰渐变、**不模糊**。这是 spec §3 完全没写的一档。
    CHECK(mc::ui::screenBackground(PageId::Game, true, true) == Kind::Transparent);
    CHECK(!mc::ui::backgroundIsBlurred(Kind::Transparent));

    // ★ 暂停屏**不是** in-game UI：26.1 只有 AbstractContainerScreen 与命令方块编辑屏
    // 返回 true。把暂停也判成容器，暂停菜单就永远不会模糊。
    CHECK(!mc::ui::isInGameUi(PageId::Pause, true));
    CHECK(mc::ui::screenBackground(PageId::Pause, true, true) == Kind::InWorldBlur);

    // 死亡屏覆写整个 extractBackground：红渐变，不模糊、不铺遮罩。
    CHECK(mc::ui::screenBackground(PageId::Death, true, false) == Kind::RedGradient);
    CHECK(mc::ui::screenBackground(PageId::Death, true, true) == Kind::RedGradient);
    CHECK(!mc::ui::backgroundIsBlurred(Kind::RedGradient));
    CHECK(!mc::ui::backgroundTilesMenuTexture(Kind::RedGradient));
}

// --- 2. 每一档的三个开关互不矛盾 ---------------------------------------------
void testKindPredicates() {
    // 模糊的两档都铺遮罩；铺遮罩的两档都模糊——26.1 里这两件事出自同一个 else 分支
    for (const auto kind : {Kind::PanoramaClear, Kind::PanoramaBlur, Kind::InWorldBlur,
                            Kind::Transparent, Kind::RedGradient}) {
        CHECK(mc::ui::backgroundIsBlurred(kind) == mc::ui::backgroundTilesMenuTexture(kind));
    }
    // 有世界就不画全景（背后是世界本身）
    CHECK(!mc::ui::backgroundDrawsPanorama(Kind::InWorldBlur));
    CHECK(mc::ui::backgroundDrawsPanorama(Kind::PanoramaClear));
    CHECK(mc::ui::backgroundDrawsPanorama(Kind::PanoramaBlur));
    // 渐变档不画全景也不铺遮罩：它们**盖在**已有画面上
    CHECK(!mc::ui::backgroundDrawsPanorama(Kind::Transparent));
    CHECK(!mc::ui::backgroundDrawsPanorama(Kind::RedGradient));
    // 只有渐变档有色标
    CHECK(mc::ui::backgroundGradient(Kind::PanoramaClear) == mc::ui::GradientStops{});
    CHECK(mc::ui::backgroundGradient(Kind::PanoramaBlur) == mc::ui::GradientStops{});
    CHECK(mc::ui::backgroundGradient(Kind::InWorldBlur) == mc::ui::GradientStops{});
}

// --- 3. 色标：逐位对上 26.1 的那两个 int -------------------------------------
void testGradientStops() {
    // Screen.java:467  fillGradient(..., -1072689136, -804253680)
    CHECK(mc::ui::kTransparentBackgroundStops.top == 0xC0101010U);
    CHECK(mc::ui::kTransparentBackgroundStops.bottom == 0xD0101010U);
    CHECK(static_cast<std::uint32_t>(-1072689136) == 0xC0101010U);
    CHECK(static_cast<std::uint32_t>(-804253680) == 0xD0101010U);

    // DeathScreen.java:136  fillGradient(..., 1615855616, -1602211792)
    CHECK(mc::ui::kDeathBackgroundStops.top == 0x60500000U);
    CHECK(mc::ui::kDeathBackgroundStops.bottom == 0xA0803030U);
    CHECK(static_cast<std::uint32_t>(1615855616) == 0x60500000U);
    CHECK(static_cast<std::uint32_t>(-1602211792) == 0xA0803030U);
    // ★ 不是 spec §9.3 写的 0x80500000。这条断言存在的唯一理由就是防止改回去。
    CHECK(mc::ui::kDeathBackgroundStops.bottom != 0x80500000U);
    // 底端比顶端更不透明、也更亮——渐变方向反了会得到"上红下透"，那是另一个屏
    CHECK((mc::ui::kDeathBackgroundStops.bottom >> 24U) >
          (mc::ui::kDeathBackgroundStops.top >> 24U));
    CHECK((mc::ui::kTransparentBackgroundStops.bottom >> 24U) >
          (mc::ui::kTransparentBackgroundStops.top >> 24U));
    // 灰渐变两端 RGB 相同，只有 alpha 在变
    CHECK((mc::ui::kTransparentBackgroundStops.top & 0x00FFFFFFU) ==
          (mc::ui::kTransparentBackgroundStops.bottom & 0x00FFFFFFU));
}

// --- 4. ARGB → 0..1 分量，按字节除以 255（编码值，不做伽马转换）--------------
void testUnpack() {
    const auto opaqueWhite = mc::ui::unpackArgb(0xFFFFFFFFU);
    CHECK(opaqueWhite.r == 1.0F && opaqueWhite.g == 1.0F && opaqueWhite.b == 1.0F &&
          opaqueWhite.a == 1.0F);
    CHECK(mc::ui::unpackArgb(0x00000000U) == mc::ui::GradientColor{});

    // 0xA0803030：alpha 0xA0、红 0x80、绿蓝 0x30。分量顺序错位是这里最容易发生的事，
    // 而 "红" 与 "偏紫的红" 在截图上很难分辨。
    const auto death = mc::ui::unpackArgb(mc::ui::kDeathBackgroundStops.bottom);
    CHECK(death.a == 160.0F / 255.0F);
    CHECK(death.r == 128.0F / 255.0F);
    CHECK(death.g == 48.0F / 255.0F);
    CHECK(death.b == 48.0F / 255.0F);
    CHECK(death.r > death.g);

    // ★ 编码值：0x80 是 0.502，不是它对应的线性值 0.2159。
    // GUI 混合发生在 sRGB 编码值上（已登记的护栏），这里加一个转换就是把它改坏。
    CHECK(death.r > 0.50F && death.r < 0.51F);

    const auto dim = mc::ui::unpackArgb(mc::ui::kTransparentBackgroundStops.top);
    CHECK(dim.r == dim.g && dim.g == dim.b);
    CHECK(dim.r == 16.0F / 255.0F);
    CHECK(dim.a == 192.0F / 255.0F);
}

// --- 5. 模糊档位 -------------------------------------------------------------
void testBlurriness() {
    CHECK(mc::ui::kMenuBlurDefault == 5);       // Options.java:271 BLURRINESS_DEFAULT_VALUE
    CHECK(mc::ui::kMenuBlurMaximum == 10);      // GameRenderer.java:104 MAX_BLUR_RADIUS

    // 0 是 OFF：Screen.java:436 `blurRadius >= 1.0F` 才触发
    CHECK(!mc::ui::menuBlurEnabled(0));
    CHECK(mc::ui::menuBlurEnabled(1));
    CHECK(mc::ui::menuBlurEnabled(5));
    CHECK(mc::ui::menuBlurEnabled(10));

    // 半径就是档位本身，两端夹住
    CHECK(mc::ui::menuBlurRadius(5) == 5);
    CHECK(mc::ui::menuBlurRadius(0) == 0);
    CHECK(mc::ui::menuBlurRadius(-3) == 0);
    CHECK(mc::ui::menuBlurRadius(99) == 10);

    // 采样数：box_blur.fsh 靠双线性采样，步长 2，共 r 次 + 末尾一次半权重。
    // 换成"每像素采一次"的 2r+1 会让每一趟贵一倍多，而画面看不出区别——
    // 正因为看不出，它需要一条断言。
    CHECK(mc::ui::menuBlurTapCount(5) == 6);
    CHECK(mc::ui::menuBlurTapCount(10) == 11);
    CHECK(mc::ui::menuBlurTapCount(0) == 1);
    CHECK(mc::ui::menuBlurTapCount(5) != 2 * 5 + 1);
}

// --- 6. 模糊那六趟的排布 -----------------------------------------------------
//
// 趟数与方向都退回去也不会崩：两趟仍然是模糊，只是更硬；全横向也仍然是模糊，
// 只是变成横向拖影。MenuBlur 住在一个没有测试链接的翻译单元里，所以这两件事
// 提到了这里才有地方变红。
void testBlurSchedule() {
    // blur.json 的三对 H/V
    CHECK(mc::ui::kMenuBlurPassCount == 6);
    // ★ 必须是偶数：ping-pong 从 scene_color 出发，奇数趟会把结果留在临时靶上，
    //   屏幕上是上一趟的画面。
    CHECK(mc::ui::kMenuBlurPassCount % 2 == 0);

    int horizontal = 0;
    int vertical = 0;
    for (int pass = 0; pass < mc::ui::kMenuBlurPassCount; ++pass) {
        const auto direction = mc::ui::menuBlurPassDirection(pass);
        // 每一趟只沿一个方向：可分离卷积的前提就是这个
        CHECK((direction.x == 0.0F) != (direction.y == 0.0F));
        horizontal += direction.x != 0.0F ? 1 : 0;
        vertical += direction.y != 0.0F ? 1 : 0;
        // 相邻两趟方向必须不同
        if (pass > 0) {
            CHECK(!(mc::ui::menuBlurPassDirection(pass - 1) == direction));
        }
    }
    CHECK(horizontal == 3);
    CHECK(vertical == 3);
    // 第一趟是横向（blur.json 的第一个 pass 是 BlurDir = [1, 0]）
    constexpr mc::ui::MenuBlurDirection kHorizontal{1.0F, 0.0F};
    CHECK(mc::ui::menuBlurPassDirection(0) == kHorizontal);
}

// --- 7. 源码护栏：模糊采样器必须是 LINEAR -------------------------------------
//
// box_blur.fsh 靠双线性把采样次数减半（步长 2，在像素之间取样）。换成 NEAREST，
// 同样的循环会每隔一个像素漏掉一个：结果是竖条纹，而不是模糊。
// 这件事无法用返回值断言——它是一个 VkSamplerCreateInfo 字段，住在一个没有测试
// 链接的翻译单元里。所以照 title_background_test 的办法读源码。
void testBlurSamplerIsLinear() {
    std::ifstream input{MC_REBEDROCK_MENU_BLUR_SRC, std::ios::binary};
    if (!input) {
        std::printf("screen_background_test: cannot open %s\n", MC_REBEDROCK_MENU_BLUR_SRC);
        ++failures;
        return;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    // 去掉行注释，免得只在说明文字里提到的 NEAREST 被当成一次真的设置
    std::string source;
    {
        std::istringstream lines{buffer.str()};
        std::string line;
        while (std::getline(lines, line)) {
            const auto comment = line.find("//");
            source += comment == std::string::npos ? line : line.substr(0, comment);
            source += '\n';
        }
    }
    CHECK(source.find("samplerInfo.magFilter = VK_FILTER_LINEAR;") != std::string::npos);
    CHECK(source.find("samplerInfo.minFilter = VK_FILTER_LINEAR;") != std::string::npos);
    CHECK(source.find("VK_FILTER_NEAREST") == std::string::npos);
    // 采样点会越过屏幕边缘（±半径）。REPEAT 会把对侧画面卷进来，四边出现鬼影。
    CHECK(source.find("VK_SAMPLER_ADDRESS_MODE_REPEAT") == std::string::npos);
    CHECK(source.find("VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE") != std::string::npos);
    // 六趟之间是**替换**不是叠加：混合开着每一趟都会把上一趟的结果再叠一次。
    CHECK(source.find("colorAttachment.blendEnable = VK_FALSE;") != std::string::npos);
}

} // namespace

int main() {
    testKindTable();
    testKindPredicates();
    testGradientStops();
    testUnpack();
    testBlurriness();
    testBlurSchedule();
    testBlurSamplerIsLinear();
    if (failures != 0) {
        std::printf("screen_background_test: %d checks failed\n", failures);
        return 1;
    }
    return 0;
}

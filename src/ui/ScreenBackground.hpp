#pragma once

// UI-5：每一屏用哪一档背景（GUI spec §3）。
//
// ★ 26.1 的 `Screen.extractBackground()` 是**三分支**，不是 spec §3.2 写的两分支：
//
//   if (isInGameUi())                    // 容器 / 背包 / 命令方块编辑
//       extractTransparentBackground()   //   0xC0101010 -> 0xD0101010 竖直渐变，**不模糊**
//   else:
//       if (level == null) extractPanorama()
//       extractBlurredBackground()       //   整帧模糊
//       extractMenuBackground()          //   menu_background / inworld_menu_background 平铺 32
//
// 那条灰渐变是 1.20.5 之前 `renderBackground` 的遗物，**专为容器类保留了下来**
// （`Screen.java:419-429,467`）。spec §3 完全没提它——照 spec 实现会把背包背景也做成模糊。
//
// 死亡屏另有一档：它整个覆写了 `extractBackground`，画一层红色竖直渐变、不模糊
// （`DeathScreen.java:135`，`0x60500000 -> 0xA0803030`——spec §9.3 给的第二个色标 0x80500000 是错的）。
//
// 这里只有"哪一屏配哪一档"这张表和它的颜色，不碰 Vulkan，因此可以被无头断言。
// 绘制侧照着这张表画，于是"某一屏的背景换错了档"这件事有地方变红。

#include "ui/PageStack.hpp"

#include <cstdint>

namespace mc::ui {

enum class ScreenBackgroundKind : std::uint8_t {
    // 全景，不模糊、不加遮罩（主菜单本体是清晰的）
    PanoramaClear,
    // 全景 -> 整帧模糊 -> gui/menu_background 平铺
    PanoramaBlur,
    // 世界画面 -> 整帧模糊 -> gui/inworld_menu_background 平铺
    InWorldBlur,
    // 容器类：一层竖直灰渐变，**不模糊**
    Transparent,
    // 死亡屏：一层竖直红渐变，**不模糊**
    RedGradient,
};

// 一档竖直渐变的两个色标，ARGB 编码值（与 vanilla 的 `fillGradient` 取同一种写法）。
struct GradientStops final {
    std::uint32_t top = 0U;
    std::uint32_t bottom = 0U;

    [[nodiscard]] constexpr bool operator==(const GradientStops&) const = default;
};

// `Screen.extractTransparentBackground`：1.20.5 前那条渐变，26.1 为容器类保留。
inline constexpr GradientStops kTransparentBackgroundStops{0xC0101010U, 0xD0101010U};
// `DeathScreen.extractDeathBackground`。★ 第二个色标是 0xA0803030，不是 spec §9.3 的 0x80500000。
inline constexpr GradientStops kDeathBackgroundStops{0x60500000U, 0xA0803030U};

// 遮罩贴图按 32 逻辑像素平铺（`Screen.extractMenuBackgroundTexture` 里那个 `int size = 32`）。
inline constexpr int kMenuBackgroundTileSize = 32;

// 这一屏是不是 26.1 意义上的 "in-game UI"。
// 26.1 里只有 `AbstractContainerScreen` 与命令方块编辑屏返回 true；本作对应的是容器界面。
// 注意**暂停屏不算**——它走模糊那条。
[[nodiscard]] constexpr bool isInGameUi(PageId page, bool containerOpen) {
    return page == PageId::Game && containerOpen;
}

// 某一屏该用哪一档。
//
// `worldOpen` 是"背后有没有世界"，`containerOpen` 是"当前开着的是不是容器/背包"。
// 两个都是玩法事实，由渲染器提供；这张表本身不知道世界或容器长什么样。
[[nodiscard]] constexpr ScreenBackgroundKind screenBackground(PageId page, bool worldOpen,
                                                              bool containerOpen) {
    if (page == PageId::Death) {
        return ScreenBackgroundKind::RedGradient;
    }
    if (isInGameUi(page, containerOpen)) {
        return ScreenBackgroundKind::Transparent;
    }
    if (page == PageId::Title) {
        return ScreenBackgroundKind::PanoramaClear;
    }
    return worldOpen ? ScreenBackgroundKind::InWorldBlur : ScreenBackgroundKind::PanoramaBlur;
}

// 这一档要不要跑整帧模糊。
[[nodiscard]] constexpr bool backgroundIsBlurred(ScreenBackgroundKind kind) {
    return kind == ScreenBackgroundKind::PanoramaBlur || kind == ScreenBackgroundKind::InWorldBlur;
}

// 这一档要不要画全景（有世界时全景不画，背后是世界本身）。
[[nodiscard]] constexpr bool backgroundDrawsPanorama(ScreenBackgroundKind kind) {
    return kind == ScreenBackgroundKind::PanoramaClear ||
           kind == ScreenBackgroundKind::PanoramaBlur;
}

// 这一档铺哪一张遮罩贴图；渐变档不铺。
[[nodiscard]] constexpr bool backgroundTilesMenuTexture(ScreenBackgroundKind kind) {
    return kind == ScreenBackgroundKind::PanoramaBlur || kind == ScreenBackgroundKind::InWorldBlur;
}

// 渐变档的两个色标；非渐变档返回全透明（画出来就是不画）。
[[nodiscard]] constexpr GradientStops backgroundGradient(ScreenBackgroundKind kind) {
    switch (kind) {
    case ScreenBackgroundKind::Transparent:
        return kTransparentBackgroundStops;
    case ScreenBackgroundKind::RedGradient:
        return kDeathBackgroundStops;
    case ScreenBackgroundKind::PanoramaClear:
    case ScreenBackgroundKind::PanoramaBlur:
    case ScreenBackgroundKind::InWorldBlur:
        break;
    }
    return GradientStops{0U, 0U};
}

// 一个 ARGB 色标拆成 0..1 的四个分量。
//
// 混合发生在 **sRGB 编码值**上（见 [[GUI 混合空间]] 那条护栏），所以这里就是按字节除以
// 255，不做任何伽马转换——加一个 sRGB→线性 的转换正是那条护栏说的"别修"。
struct GradientColor final {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 0.0F;

    [[nodiscard]] constexpr bool operator==(const GradientColor&) const = default;
};

[[nodiscard]] constexpr GradientColor unpackArgb(std::uint32_t argb) {
    return GradientColor{
        static_cast<float>((argb >> 16U) & 0xFFU) / 255.0F,
        static_cast<float>((argb >> 8U) & 0xFFU) / 255.0F,
        static_cast<float>(argb & 0xFFU) / 255.0F,
        static_cast<float>((argb >> 24U) & 0xFFU) / 255.0F,
    };
}

// GUI spec §3.2 / §7.11：模糊强度是**整数档**，默认 5，最低档（0）显示 OFF 即不模糊。
// 26.1 `Options.menuBackgroundBlurriness`，半径直接喂给 box_blur 的 `Radius`。
inline constexpr int kMenuBlurDefault = 5;
inline constexpr int kMenuBlurMaximum = 10;

// `Screen.extractBlurredBackground`：`blurRadius >= 1.0F` 才调 blurBeforeThisStratum()。
[[nodiscard]] constexpr bool menuBlurEnabled(int blurriness) { return blurriness >= 1; }

// 实际喂给 box_blur 的半径。26.1 的 blur.json 把 `Radius` 这个 uniform 写成 0，
// 于是 `box_blur.fsh` 里 `Radius >= 0.5` 不成立，取的是全局 `MenuBlurRadius`——
// 也就是选项值本身。夹到 [0, 10]：`GameRenderer.MAX_BLUR_RADIUS = 10`。
[[nodiscard]] constexpr int menuBlurRadius(int blurriness) {
    if (blurriness < 0) {
        return 0;
    }
    return blurriness > kMenuBlurMaximum ? kMenuBlurMaximum : blurriness;
}

// box_blur.fsh 的一趟要采多少次纹理。
//
// 那个着色器**依赖 GL_LINEAR**：以 2 为步长在像素之间采样，一次拿两个像素的平均，
// 最后再补一次半权重的边缘采样。所以采样数不是 `2r+1` 而是 `floor(r) + 1`——
// 循环从 -r+0.5 走到 r，步长 2，共 ceil(r) 次；加上末尾那次半权重。
// 写成一个纯函数，是为了让"把双线性采样器换成最近邻"这类改动能被断言抓住：
// 最近邻下同样的采样数会漏掉一半像素，画面变成条纹而不是模糊。
// 26.1 的 `post_effect/blur.json` 是**六趟** box_blur：横、竖、横、竖、横、竖。
//
// 不是一对。三对的结果明显更柔，而"糊得不够"这件事在画面上只是"看起来还行"，
// 没有任何自然的断言会红——所以趟数与方向排布写在这里，由 MenuBlur 照着跑。
//
// 六是**偶数**这一点也有意义：ping-pong 从 scene_color 出发，偶数趟才会回到
// scene_color。改成奇数趟，最后一趟的结果留在临时靶上，屏幕上是上一趟的画面。
inline constexpr int kMenuBlurPassCount = 6;

struct MenuBlurDirection final {
    float x = 0.0F;
    float y = 0.0F;

    [[nodiscard]] constexpr bool operator==(const MenuBlurDirection&) const = default;
};

[[nodiscard]] constexpr MenuBlurDirection menuBlurPassDirection(int pass) {
    return pass % 2 == 0 ? MenuBlurDirection{1.0F, 0.0F} : MenuBlurDirection{0.0F, 1.0F};
}

[[nodiscard]] constexpr int menuBlurTapCount(int radius) {
    if (radius <= 0) {
        return 1;
    }
    // a = -r+0.5, -r+2.5, ... <= r  →  共 r 次；再加末尾那次半权重
    return radius + 1;
}

} // namespace mc::ui

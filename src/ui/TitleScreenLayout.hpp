#pragma once

// UI-2：`SCREEN_TITLE` 的版面，按 GUI spec §6.3 与 26.1 的 `TitleScreen.init` /
// `LogoRenderer.extractRenderState` 逐项对齐。
//
// **一切都是逻辑像素上的整数运算。** spec §1.2 把这条写成了硬要求：所有"居中"都是整数
// 除法，复现时必须同样用整数除法，否则会与原版差 1px。本仓其余屏幕仍在帧缓冲像素上用
// 浮点算版面（`HudLayout::menuButton` 之类），那套算不出 `j = H/4 + 48` 这种以逻辑高度
// 做整数除法的基线，所以标题屏走这里，其余屏幕一像素不动（README 护栏第 4 条）。
//
// 逻辑画布 = ceil(帧缓冲 / GUI 缩放)（spec §1.1），由 `HudLayout::logicalWidth/Height`
// 给出；把这里的矩形乘以 scale 就回到帧缓冲像素。
//
// ★ spec §6.3 有四处几何是 1.20 的旧值，本文件按 26.1 源码取正确的那个，差异逐条记在
//   docs/content-dev/UI-gui-parity/UI-2-title-screen-and-capture.md 的落地记录里：
//   1. 图标行 y：spec 写 `j + 72`，26.1 是 `j + 84`
//      （`TitleScreen.java:110-118`：三个主按钮走完停在 `j+48`，随后 `topPos += 36`）。
//   2. edition 副标题 y：spec 写 74，26.1 是 67
//      （`LogoRenderer.java:42`：`heightOffset + 44 - EDITION_LOGO_OVERLAP`，overlap = 7）。
//   3. logo 不是"左右两个 128x44 半幅拼接"，而是一次 256x44 的绘制
//      （`LogoRenderer.java:39`）——这同时结掉 spec §13.2 待验证清单的第 12 条。
//   4. splash 的锚点与缩放公式也与 spec 不同（`SplashRenderer.java:31-38`）；
//      splash 本轮不实现，更正只记录不落地。

#include <cstddef>

namespace mc::ui {

// 逻辑像素矩形。spec §0.3 的坐标系：原点左上，+x 向右，+y 向下，单位是缩放后的 GUI 像素。
struct TitleRect final {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] bool operator==(const TitleRect&) const = default;
};

// 26.1 的常量，全部来自 `LogoRenderer` 与 `TitleScreen`，不是估值。
inline constexpr int kTitleLogoWidth = 256;         // LogoRenderer.LOGO_WIDTH
inline constexpr int kTitleLogoHeight = 44;         // LogoRenderer.LOGO_HEIGHT
inline constexpr int kTitleLogoTop = 30;            // LogoRenderer.DEFAULT_HEIGHT_OFFSET
inline constexpr int kTitleEditionWidth = 128;      // LogoRenderer.EDITION_WIDTH
inline constexpr int kTitleEditionHeight = 14;      // LogoRenderer.EDITION_HEIGHT
inline constexpr int kTitleEditionOverlap = 7;      // LogoRenderer.EDITION_LOGO_OVERLAP
inline constexpr int kTitleMenuBaseline = 48;       // j = H/4 + 48
inline constexpr int kTitleMenuSpacing = 24;        // TitleScreen.init 的 spacing
inline constexpr int kTitleIconRowDrop = 36;        // 主按钮组之后的 `topPos += 36`
inline constexpr int kTitleButtonWidth = 200;
inline constexpr int kTitleButtonHeight = 20;
inline constexpr int kTitleHalfButtonWidth = 98;    // options / quit 各 98，中间留 4
inline constexpr int kTitleIconButtonSize = 20;     // SpriteIconButton 默认 20x20
inline constexpr int kTitleFooterTextHeight = 10;   // version / copyright 行高

// 一整屏的版面。每个字段都是逻辑像素矩形，绘制侧乘 scale 即可。
//
// 按钮的**顺序**同时是 PageBuilder 的装配顺序与命中测试的下标顺序，所以它是版面的一部分，
// 不是绘制侧的自由：`kTitleWidgetOrder` 把这条钉住。
struct TitleScreenLayout final {
    TitleRect logo;
    TitleRect edition;
    TitleRect singleplayer;
    TitleRect multiplayer;
    TitleRect realms;
    TitleRect language;       // 图标钮，绘制留给 UI-4 的 IconButton，版面这里先解出来
    TitleRect options;
    TitleRect quit;
    TitleRect accessibility;  // 同上
    TitleRect version;        // 左下 "Minecraft <版本>"
    TitleRect copyright;      // 右下，可点击（26.1 是 PlainTextButton）

    [[nodiscard]] bool operator==(const TitleScreenLayout&) const = default;
};

// `logicalWidth` / `logicalHeight` 是逻辑画布（ceil(帧缓冲 / scale)）。
// `versionTextWidth` / `copyrightTextWidth` 是这两行文本按当前字体量出的逻辑像素宽度：
// 版权行是右对齐的（`x = W - 宽 - 2`），所以文本宽度是版面的输入而不是输出。
[[nodiscard]] TitleScreenLayout titleScreenLayout(int logicalWidth, int logicalHeight,
                                                  int versionTextWidth,
                                                  int copyrightTextWidth);

// 主菜单上真正参与命中与派发的控件个数（图标钮包含在内：它们在 26.1 里是可点的，
// 本轮只是不画图标）。PageBuilder 与绘制侧共用这个数，两边因此不会各数各的。
inline constexpr std::size_t kTitleWidgetCount = 7U;

// 版面里第 `index` 个可点控件的矩形，顺序与 PageBuilder 的装配顺序一致：
// singleplayer / multiplayer / realms / language / options / quit / accessibility。
[[nodiscard]] TitleRect titleWidgetRect(const TitleScreenLayout& layout, std::size_t index);

} // namespace mc::ui

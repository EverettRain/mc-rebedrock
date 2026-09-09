#pragma once

// UI-6a：页眉 / 内容 / 页脚三段式版面（GUI spec §2.8，26.1 的
// `net.minecraft.client.gui.layouts.HeaderAndFooterLayout`）。
//
// 26.1 的每一个设置子屏都是这个形状（`OptionsSubScreen.init()`）：
//     layout.addTitleHeader(title, font)          // 页眉：标题，居中
//     layout.addToContents(new OptionsList(...))  // 内容：滚动列表
//     layout.addToFooter(Button "Done" width 200) // 页脚：一个按钮，居中
//
// 本作在这之前没有这一层：每一页都是"一条按钮带"，几何由 `frontendButtonRect`
// 按控件序号直接给。那种形状撑不起 §7 的设置子树——标题、滚动内容、页脚三者
// 各有各的纵向归属，混在一条按钮带里就没有"内容区有多高"这个量可谈。
//
// 单位一律是**逻辑像素**整数（护栏 5）。乘 GUI 缩放是调用方最后一步的事。

#include "ui/HudLayout.hpp"

#include <algorithm>

namespace mc::ui {

// `HeaderAndFooterLayout.DEFAULT_HEADER_AND_FOOTER_HEIGHT`。页眉与页脚**同高**，
// 26.1 的三个构造函数最终都落到这一个数上。
inline constexpr int kHeaderAndFooterHeight = 33;
// `CONTENT_MARGIN_TOP`：固定高度的内容块距页眉底端的首选间距。
inline constexpr int kContentMarginTop = 30;
// `addToFooter(Button.builder(GUI_DONE, ...).width(200).build())`（OptionsSubScreen:52）
inline constexpr int kFooterButtonWidth = 200;
inline constexpr int kFooterButtonHeight = 20;

// UI-6f（D23）：**副页眉**——页眉里除标题之外的那一行控件。
//
// 26.1 `OptionsScreen.init()` 的页眉是 `LinearLayout.vertical().spacing(8)`：
//   标题（StringWidget，行高 9）
//   一行水平排布：fov 滑块 + （世界内 Difficulty / 世界外 Online），spacing 8
// 26.1 的 HeaderAndFooterLayout 会按内容把页眉撑高；本作的页眉是固定值，
// 所以这里给出"带一行副页眉时的页眉高"，由 optionsHubLayout 使用。
inline constexpr int kSubHeaderSpacing = 8;      // LinearLayout.vertical().spacing(8)
inline constexpr int kSubHeaderButtonWidth = 150;   // OptionInstance.createButton 的默认宽
inline constexpr int kSubHeaderButtonHeight = 20;
// 标题 9 + 间距 8 + 控件 20 = 37，再给上下各 6 的呼吸 = 49。
// ★ 它必须 ≥ kHeaderAndFooterHeight，否则带副页眉的页面反而比普通页面矮。
inline constexpr int kSubHeaderTotalHeight = 49;
static_assert(kSubHeaderTotalHeight >= kHeaderAndFooterHeight,
              "带副页眉的页眉不能比普通页眉矮");

struct HeaderAndFooterLayout final {
    // 逻辑画布。26.1 的 `getWidth()/getHeight()` 直接返回 screen 的宽高。
    int width = 0;
    int height = 0;
    int headerHeight = kHeaderAndFooterHeight;
    int footerHeight = kHeaderAndFooterHeight;

    // `getContentHeight() = screen.height - headerHeight - footerHeight`
    [[nodiscard]] constexpr int contentHeight() const {
        return std::max(height - headerHeight - footerHeight, 0);
    }

    [[nodiscard]] constexpr UiRect headerBox() const {
        return {0.0F, 0.0F, static_cast<float>(width), static_cast<float>(headerHeight)};
    }

    // 页脚贴底：`footerFrame.setY(screen.height - footerHeight)`
    [[nodiscard]] constexpr UiRect footerBox() const {
        return {0.0F, static_cast<float>(height - footerHeight), static_cast<float>(width),
                static_cast<float>(footerHeight)};
    }

    // 内容区 = 页眉与页脚之间的整块。
    //
    // ★ 这**不是** `contentsFrame` 的位置。26.1 的 `arrangeElements()` 把 contentsFrame
    // 放在 `min(headerHeight + 30, height - footerHeight - 内容高)`——那是给**固定高度**
    // 的内容块用的（居上但不越过页脚）。滚动列表走的是另一条：`OptionsList` 的构造直接
    // 取 `layout.getHeaderHeight()` 当视口 y、`layout.getContentHeight()` 当高
    // （`OptionsList.java:22`），也就是**占满**这一整块。两者都要，所以分成两个函数。
    [[nodiscard]] constexpr UiRect contentBox() const {
        return {0.0F, static_cast<float>(headerHeight), static_cast<float>(width),
                static_cast<float>(contentHeight())};
    }

    // 固定高度的内容块的 y：`min(headerHeight + 30, height - footerHeight - 内容高)`。
    // 两项都要：只取前者，内容会伸进页脚；只取后者，短内容会被推到页脚正上方而不是靠上。
    [[nodiscard]] constexpr int fixedContentY(int contentsHeight) const {
        return std::min(headerHeight + kContentMarginTop, height - footerHeight - contentsHeight);
    }

    // 页眉里的标题：`headerFrame.defaultChildLayoutSetting().align(0.5F, 0.5F)`，
    // 也就是在页眉那一整块里**双向居中**。整数除法（护栏 5）。
    [[nodiscard]] constexpr UiRect headerTitle(int textWidth, int textHeight) const {
        return {static_cast<float>(width / 2 - textWidth / 2),
                static_cast<float>(headerHeight / 2 - textHeight / 2),
                static_cast<float>(textWidth), static_cast<float>(textHeight)};
    }

    // 页脚里的一个按钮，同样双向居中。26.1 的 Done 宽 200。
    [[nodiscard]] constexpr UiRect footerButton(int buttonWidth = kFooterButtonWidth,
                                                int buttonHeight = kFooterButtonHeight) const {
        const auto footer = footerBox();
        return {static_cast<float>(width / 2 - buttonWidth / 2),
                footer.y + static_cast<float>(footerHeight / 2 - buttonHeight / 2),
                static_cast<float>(buttonWidth), static_cast<float>(buttonHeight)};
    }
};

// 页眉里那一行控件（第 `index` 个，共 `count` 个），水平居中、spacing 8。
[[nodiscard]] constexpr UiRect subHeaderButton(int logicalWidth, int headerHeight,
                                               std::size_t index, std::size_t count) {
    const int total = static_cast<int>(count) * kSubHeaderButtonWidth +
                      (static_cast<int>(count) - 1) * kSubHeaderSpacing;
    const int left = logicalWidth / 2 - total / 2;
    // 竖直方向：标题在上，这一行贴在页眉下部。
    const int top = headerHeight - kSubHeaderButtonHeight - 6;
    return {static_cast<float>(left + static_cast<int>(index) *
                                          (kSubHeaderButtonWidth + kSubHeaderSpacing)),
            static_cast<float>(top), static_cast<float>(kSubHeaderButtonWidth),
            static_cast<float>(kSubHeaderButtonHeight)};
}

// 由逻辑画布直接造一个默认三段式版面。
[[nodiscard]] constexpr HeaderAndFooterLayout headerAndFooterLayout(int logicalWidth,
                                                                    int logicalHeight) {
    return HeaderAndFooterLayout{logicalWidth, logicalHeight, kHeaderAndFooterHeight,
                                 kHeaderAndFooterHeight};
}

// 带副页眉的三段式（Options 主页）。页眉更高，内容区相应变矮。
[[nodiscard]] constexpr HeaderAndFooterLayout headerAndFooterLayoutWithSubHeader(
    int logicalWidth, int logicalHeight) {
    return HeaderAndFooterLayout{logicalWidth, logicalHeight, kSubHeaderTotalHeight,
                                 kHeaderAndFooterHeight};
}

} // namespace mc::ui

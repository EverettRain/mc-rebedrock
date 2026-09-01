#pragma once

// 提示框的**几何**：内容尺寸、逐行落位、贴边时往哪儿翻、底衬精灵铺多大
//
// I-2 把提示框的**文本**搬进了 ui/ItemTooltip（纯值、headless 可测），几何却还
// 留在 `render/vulkan/HudRenderer.hpp` 里，于是"多一行高多少""贴着右边缘往哪翻"
// 一条断言都没有。这里补上同一半：不碰 Vulkan、不读光标、不问屏幕，全是可断言
// 的算术，正如 ui/GuiNineSlice 之于九宫格。
//
// 单位一律是**未缩放的 GUI 像素**——精灵的边框宽度也按这套单位度量，混用会让
// 1px 的边框在不同 GUI 缩放下变粗变细。乘 scale 是调用方最后一步的事。
//
// 对标 26.1：`GuiGraphics#tooltip`（内容尺寸与逐行推进）、
// `DefaultTooltipPositioner#positionTooltip`（贴边）、
// `TooltipRenderUtil#extractTooltipBackground`（PADDING 3 + MARGIN 9）。
#include "ui/HudLayout.hpp"

#include <algorithm>
#include <cstddef>

namespace mc::ui {

// 一行文本占的高度。`ClientTextTooltip#getHeight` 恒为 10，本项目没有图像行。
inline constexpr float kTooltipLineHeight = 10.0F;
// 名称行与其后各行之间的那道缝。vanilla 在画完第 0 行后多推 2px，
// 而**不**把它算进框高——最后一行因此会略微探进下边的内边距里，这是原版行为。
inline constexpr float kTooltipTitleGap = 2.0F;
// TooltipRenderUtil 的 PADDING(3) + MARGIN(9)：底衬精灵比内容矩形每边大这么多。
// 精灵最外 8px 是透明的，可见的填充因此落在内容 +4、那 1px 边框落在内容 +3。
inline constexpr float kTooltipBackdropMargin = 12.0F;
// 光标到内容左上角的偏移。注意纵向是**负的**：提示框挂在光标的右上方。
inline constexpr float kTooltipCursorOffsetX = 12.0F;
inline constexpr float kTooltipCursorOffsetY = -12.0F;
// 翻到光标左侧后至少离屏幕左缘这么远（`Math.max(..., 4)`）。
inline constexpr float kTooltipScreenMargin = 4.0F;

// 框高。单行是 8 而不是 10——`lines.size() == 1 ? -2 : 0` 那句；
// 单行提示框（只有名字）因此比两行的一半还紧一点，正是原版的样子。
[[nodiscard]] constexpr float tooltipContentHeight(std::size_t lineCount) {
    if (lineCount == 0U) {
        return 0.0F;
    }
    const float lines = static_cast<float>(lineCount);
    return (lineCount == 1U ? -kTooltipTitleGap : 0.0F) + kTooltipLineHeight * lines;
}

// 第 index 行相对内容顶端的纵向偏移：0、12、22、32……
[[nodiscard]] constexpr float tooltipLineOffset(std::size_t index) {
    if (index == 0U) {
        return 0.0F;
    }
    return kTooltipLineHeight * static_cast<float>(index) + kTooltipTitleGap;
}

// 内容矩形的左上角。`DefaultTooltipPositioner`：光标 +12/-12，右边放不下就整个
// 翻到光标左侧（至少留 4px），下边放不下就顶到 `screenHeight - height - 3`。
//
// 与 vanilla 的一处**有意偏差**：原版不夹上边，贴着屏幕顶端的提示框在原版里就是
// 会被切掉上沿（原版的槽位从不贴顶，所以看不见）。本项目的提示框会出现在 HUD
// 快捷栏这类更贴边的地方，因此也夹上边。
[[nodiscard]] inline UiPoint positionTooltip(float screenWidth, float screenHeight, float cursorX,
                                             float cursorY, float contentWidth,
                                             float contentHeight) {
    float x = cursorX + kTooltipCursorOffsetX;
    float y = cursorY + kTooltipCursorOffsetY;
    if (x + contentWidth > screenWidth) {
        x = std::max(x - 2.0F * kTooltipCursorOffsetX - contentWidth, kTooltipScreenMargin);
    }
    // 原版这里加的是 PADDING_BOTTOM(3)，不是整个 MARGIN
    if (y + contentHeight + 3.0F > screenHeight) {
        y = screenHeight - contentHeight - 3.0F;
    }
    return UiPoint{x, std::max(y, kTooltipScreenMargin)};
}

// 底衬精灵（tooltip/background 与 tooltip/frame 共用同一个矩形）的位置与大小。
[[nodiscard]] inline UiRect tooltipBackdrop(const UiRect& content) {
    return UiRect{
        content.x - kTooltipBackdropMargin,
        content.y - kTooltipBackdropMargin,
        content.width + 2.0F * kTooltipBackdropMargin,
        content.height + 2.0F * kTooltipBackdropMargin,
    };
}

} // namespace mc::ui

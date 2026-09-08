#pragma once

// UI-6a：设置项的**双列**滚动列表（GUI spec §5 的范式 L2，26.1 的
// `net.minecraft.client.gui.components.OptionsList`）。
//
// 它是 ScrollList 之上的一层：视口与滚动条几何仍归 [[ui/ScrollList.hpp]]，这里只加
// "一行里横着摆两个设置项"这件事。26.1 的每一个设置子屏都用它排版
// （`OptionsSubScreen.addOptions()` → `list.addSmall(...)` / `list.addBig(...)`）。
//
// 本作在这之前一个消费者都没有：所有设置页都是"一条按钮带"，每行一个按钮。
// 于是 Video Settings 的十几项要么挤成很长一列、要么被删到放得下为止——后者正是
// 本作 12 项对 26.1 二十八项的由来之一。
//
// ★ 数值全部取自 26.1 源码，逐条带出处：
//   BIG_BUTTON_WIDTH = 310         OptionsList.java:17
//   DEFAULT_ITEM_HEIGHT = 25       OptionsList.java:18
//   小按钮宽 150                    OptionInstance.java:117（createButton 的默认宽）
//   第二列偏移 X_OFFSET = 160       OptionsList.java:107
//   起点 x = screen.width/2 - 155  OptionsList.java:148
//   格子内缩 2                      AbstractSelectionList.Entry.getContentX/Y():471,475
//
// 310 = 150 + 10 + 150：两列加中间 10 的缝，正好等于行宽。这不是巧合，是**闭合关系**，
// 下面有一条 static_assert 钉住它——改任一个数而不改其余，两列就不再对齐行的两端。

#include "ui/ScrollList.hpp"

#include <span>

namespace mc::ui {

// 一行的高度（`DEFAULT_ITEM_HEIGHT`）。注意它比控件本身高：控件高 20，格子高 25，
// 差出来的 5 是行距。
inline constexpr int kOptionsRowHeight = 25;
// 独占一行的控件宽度（`addBig`），与行宽相同。
inline constexpr int kOptionsBigWidth = kOptionsRowWidth;
// 双列时每一列的控件宽度（`OptionInstance.createButton(options)` 的默认 150）。
inline constexpr int kOptionsSmallWidth = 150;
// 第二列相对第一列的偏移（`X_OFFSET`）。
inline constexpr int kOptionsColumnOffset = 160;
// 控件在格子里的内缩（`Entry.getContentX() = getX() + 2`）。
inline constexpr int kListEntryPadding = 2;
// 设置项控件本身的高度。26.1 的按钮与滑条都是 20 高。
inline constexpr int kOptionsWidgetHeight = 20;

// ★ 闭合关系：两列加中缝正好铺满行宽。
static_assert(kOptionsSmallWidth * 2 + 10 == kOptionsBigWidth,
              "the two small columns plus their gap must fill the row width");
static_assert(kOptionsSmallWidth + 10 == kOptionsColumnOffset,
              "the second column starts one small width plus the gap to the right");

// 设置列表的视口：铺满三段式版面的内容区，行宽 310、行高 25。
[[nodiscard]] constexpr ScrollList optionsScrollList(const UiRect& contentBox) {
    return ScrollList{static_cast<int>(contentBox.x),     static_cast<int>(contentBox.y),
                      static_cast<int>(contentBox.width), static_cast<int>(contentBox.height),
                      kOptionsRowWidth,                   kOptionsRowHeight};
}

// `addSmall(a, b)` 两个一行：n 个设置项占 ceil(n/2) 行。
[[nodiscard]] constexpr std::size_t optionsSmallRowCount(std::size_t optionCount) {
    return (optionCount + 1U) / 2U;
}

// 第 `visibleIndex` 个可见行里、第 `column`（0 或 1）列那个控件的矩形。
//
// 26.1 的 `Entry.extractContent`：`x = screen.width/2 - 155`，然后 `xOffset += 160`；
// y 是 `entry.getContentY()`，也就是行顶 + 2。
//
// 起点用 `rowLeft()` 而不是照抄 `width/2 - 155`：当视口铺满画布时两者相等
// （rowLeft = 0 + W/2 - 310/2 = W/2 - 155），而列表若被摆在别处，跟着行走才是对的。
[[nodiscard]] constexpr UiRect optionsSmallCell(const ScrollList& list, std::size_t visibleIndex,
                                                int column) {
    const auto row = scrollListRow(list, visibleIndex);
    return {row.x + static_cast<float>(column * kOptionsColumnOffset),
            row.y + static_cast<float>(kListEntryPadding),
            static_cast<float>(kOptionsSmallWidth),
            static_cast<float>(kOptionsWidgetHeight)};
}

// 一个设置项落在第几行、第几列。
struct OptionsSlot final {
    std::size_t row = 0;
    int column = 0;

    [[nodiscard]] constexpr bool operator==(const OptionsSlot&) const = default;
};

// 26.1 的 `addSmall(...)` 的分组行为：**每次调用从新行起**，组内两两配对。
//
//   list.addSmall(a, b);        // 行 0：a b
//   list.addSmall(c, d, e);     // 行 1：c d   行 2：e
//   list.addSmall(f);           // 行 3：f
//
// 这不是"把所有项摊平了两两配对"——那样上例的 e 会和 f 挤在同一行，而 26.1 里它们
// 分属两次调用、必须分行。ControlsScreen 就是这个形状：一次 addSmall 放两个跳转按钮，
// 再一次 addSmall 放七个设置项，中间那道行边界是**语义分组**，不是排版巧合。
//
// `groupSizes` 是各次 addSmall 的项数，按调用顺序。越界返回最后一行之后的位置而不抛：
// 调用方通常已经用控件数夹过，这里再抛一次只会把一个排版问题变成崩溃。
[[nodiscard]] constexpr OptionsSlot optionsGroupedSlot(std::span<const std::size_t> groupSizes,
                                                       std::size_t index) {
    std::size_t row = 0;
    std::size_t seen = 0;
    for (const std::size_t size : groupSizes) {
        if (index < seen + size) {
            const std::size_t withinGroup = index - seen;
            return OptionsSlot{row + withinGroup / 2U, static_cast<int>(withinGroup % 2U)};
        }
        seen += size;
        row += (size + 1U) / 2U;   // 这一组占的行数，落单的一项也占一整行
    }
    return OptionsSlot{row, 0};
}

// 各组一共占多少行。
[[nodiscard]] constexpr std::size_t optionsGroupedRowCount(std::span<const std::size_t> groupSizes) {
    std::size_t rows = 0;
    for (const std::size_t size : groupSizes) {
        rows += (size + 1U) / 2U;
    }
    return rows;
}

// `addBig`：独占一行，宽度等于行宽。
[[nodiscard]] constexpr UiRect optionsBigCell(const ScrollList& list, std::size_t visibleIndex) {
    const auto row = scrollListRow(list, visibleIndex);
    return {row.x, row.y + static_cast<float>(kListEntryPadding),
            static_cast<float>(kOptionsBigWidth), static_cast<float>(kOptionsWidgetHeight)};
}

} // namespace mc::ui

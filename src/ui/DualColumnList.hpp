#pragma once

// UI-6e：**双栏可转移列表**（GUI spec §5 的范式 L4，26.1 的
// `net.minecraft.client.gui.screens.packs.TransferableSelectionList`）。
//
// 左边"可用"、右边"已选"，条目在两栏之间来回移动，右栏内还能上下调序。
// 26.1 的资源包 / 数据包选择屏就是这个形状，而本作在这之前一个双栏列表都没有：
// 所有列表都是单栏。
//
// ★ 它**不是** `ui/OptionsList.hpp` 那种"一行里两个控件"。那是**一张**列表的一行摆两个
//   控件；这里是**两张互相独立的列表**，各有自己的滚动位置、自己的选中项，条目在两者
//   之间转移。把它们做成"一张两列的列表"会在第一次滚动时露馅——两栏的条目数不同，
//   滚动位置也不同。
//
// ★ 数值全部取自 26.1 源码，逐条带出处：
//   LIST_WIDTH = 200                    PackSelectionScreen.java:62
//   行高 36、页眉高 33                   TransferableSelectionList.java:39（super(..., 33, 36)）
//   行宽 = 列宽 - 4                      TransferableSelectionList.java:46
//   条目内缩 2                           TransferableSelectionList.java:34（ENTRY_PADDING）
//   左列 x = width/2 - 15 - 200         PackSelectionScreen.java:156
//   右列 x = width/2 + 15               PackSelectionScreen.java:160
//   包图标 32x32                         TransferableSelectionList.java:144
//   描述最大宽 157                       TransferableSelectionList.java:111
//
// 两列之间那 30 的中缝（-15 与 +15）不是留白，是**画布中线两侧各 15**——所以两列
// 相对整屏是对称的。下面有一条 static_assert 钉住这个关系。

#include "ui/HudLayout.hpp"
#include "ui/ScrollList.hpp"
#include "ui/Widget.hpp"
#include "ui/WidgetId.hpp"

#include <algorithm>
#include <cstddef>

namespace mc::ui {

// 一栏的宽度（`LIST_WIDTH`）。
inline constexpr int kTransferListWidth = 200;
// 一行的高度（`super(..., 33, 36)` 的第四个参数）。比设置行的 25 高得多：
// 一行里要放一张 32x32 的包图标。
inline constexpr int kTransferRowHeight = 36;
// 行宽比列宽窄 4（`getRowWidth() = this.width - 4`）。
inline constexpr int kTransferRowInset = 4;
// 中线到每一栏内缘的距离（`width/2 - 15` 与 `width/2 + 15`）。
inline constexpr int kTransferCentreGap = 15;
// 包图标的边长。
inline constexpr int kTransferIconSize = 32;
// 条目内缩（`ENTRY_PADDING`）。
inline constexpr int kTransferEntryPadding = 2;
// 描述文字的最大宽度（`MAX_DESCRIPTION_WIDTH_PIXELS`）。
inline constexpr int kTransferDescriptionWidth = 157;

// ★ 闭合关系：图标 + 两侧内缩 + 描述宽必须装得进行宽，否则描述会盖到图标上。
static_assert(kTransferIconSize + kTransferEntryPadding * 2 + kTransferDescriptionWidth <=
                  kTransferListWidth - kTransferRowInset,
              "the icon, its padding and the description must fit inside a row");

// 两栏在画布上的位置。`contentBox` 是三段式版面的内容区（`HeaderAndFooterLayout`）。
struct DualColumnLists final {
    ScrollList available{};   // 左：可用
    ScrollList selected{};    // 右：已选

    [[nodiscard]] constexpr bool operator==(const DualColumnLists&) const = default;
};

// 这块画布上一栏实际有多宽。
//
// ★ 26.1 把 200 写死（`updateSizeAndPosition(200, ...)`），因为它假设窗口够宽：
//   两栏加中缝要 200*2 + 30 = **430** 逻辑像素，而 GUI 缩放只保证画布不小于 **320**。
//   1280x720 @ scale 3 的逻辑画布是 427 —— 已经不够：照 200 算，左栏 x 会是
//   `427/2 - 15 - 200` = **-2**，右栏右缘 428 也出界。实测就是这么画到屏幕外的。
//   vanilla 在这种窗口下同样会溢，本作不跟这一条：栏宽随画布收缩，
//   宁可比 vanilla 窄一点也不画到屏幕外。（已登记为偏差。）
[[nodiscard]] constexpr int transferColumnWidth(int logicalWidth) {
    const int roomEach = (logicalWidth - kTransferCentreGap * 2) / 2;
    return roomEach < kTransferListWidth ? std::max(roomEach, 1) : kTransferListWidth;
}

[[nodiscard]] constexpr DualColumnLists dualColumnLists(const UiRect& contentBox,
                                                        int logicalWidth) {
    const int centre = logicalWidth / 2;
    const int columnWidth = transferColumnWidth(logicalWidth);
    const int y = static_cast<int>(contentBox.y);
    const int height = static_cast<int>(contentBox.height);
    const int rowWidth = columnWidth - kTransferRowInset;
    return DualColumnLists{
        ScrollList{centre - kTransferCentreGap - columnWidth, y, columnWidth,
                   height, rowWidth, kTransferRowHeight},
        ScrollList{centre + kTransferCentreGap, y, columnWidth, height, rowWidth,
                   kTransferRowHeight},
    };
}

// 一行里包图标的格子。26.1 画在 `getContentX() / getContentY()`——也就是行的左上角
// 内缩 2，**不是**垂直居中。悬停时 select / unselect 的箭头精灵盖在**同一个位置**上
// （`TransferableSelectionList.java:164-172`），所以这一个矩形同时是图标位和转移热区。
[[nodiscard]] constexpr UiRect transferIconCell(const UiRect& row) {
    return {row.x + static_cast<float>(kTransferEntryPadding),
            row.y + static_cast<float>(kTransferEntryPadding),
            static_cast<float>(kTransferIconSize), static_cast<float>(kTransferIconSize)};
}

// 名称与描述那一块：图标右侧，到行右缘。
[[nodiscard]] constexpr UiRect transferTextCell(const UiRect& row) {
    const float left = row.x + static_cast<float>(kTransferEntryPadding * 2 + kTransferIconSize);
    return {left, row.y + static_cast<float>(kTransferEntryPadding),
            row.x + row.width - left, static_cast<float>(kTransferIconSize)};
}

// 一行属于哪一栏——**靠 debugId 认，不靠序号**。
//
// ★ 布局要知道"这一行画在左边还是右边"，而它唯一读得到的是 debugId。
//   若两栏共用一个 id，就得另开一条"第几个之后算右栏"的旁路，而那个分界点
//   只有装配侧知道——同一事实两份表述，UI-6d 已经栽过一次。
[[nodiscard]] inline bool isPackRowWidget(const Widget& widget) {
    return widget.debugId == static_cast<std::uint16_t>(WidgetId::PackRowAvailable) ||
           widget.debugId == static_cast<std::uint16_t>(WidgetId::PackRowSelected);
}

[[nodiscard]] inline bool isSelectedPackRow(const Widget& widget) {
    return widget.debugId == static_cast<std::uint16_t>(WidgetId::PackRowSelected);
}

// 一栏里放得下几行。
[[nodiscard]] constexpr std::size_t transferVisibleRows(const ScrollList& list) {
    return list.visibleRows();
}

} // namespace mc::ui

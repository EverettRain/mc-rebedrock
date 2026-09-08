#pragma once

// UI-4：滚动列表的**唯一**几何来源（GUI spec §2.7 的 `AbstractSelectionList` 家族，
// 也就是 §5 的范式 L3）。
//
// 在这之前本仓有三份各写各的：语言、按键绑定、世界列表。三份的行宽（270 / 300 / 300）、
// 滚动条宽度与位置、可见行数各不相同，而且都不是 26.1 的值。这里把它们收成一处。
//
// 单位一律是**逻辑像素**（GUI 像素），整数。乘 GUI 缩放是调用方最后一步的事——
// 见 [[界面版面]] 那条护栏：版面在整数网格上解，绘制才乘 scale。
//
// ★ 数值取自 26.1 源码，不是 spec §2.7 的正文。spec 那一节有两处是 1.20.2 之前的旧状态：
//   H. 「滚动条贴在视口右侧 `x1 - 6`」→ 实际是 `行右缘 + 宽 + 2`，贴的是**行**不是视口
//      （`AbstractSelectionList.scrollBarX():283`）。
//   I. 「轨道 0xFF000000、滑块 0xFF808080 + 亮边 0xFFC0C0C0」→ 实际用**精灵**画
//      （`widget/scroller` + `widget/scroller_background`，`AbstractScrollArea:116-130`）。
//   照 spec 写死那两个颜色就是自造，和主菜单那块 30% 黑遮罩是同一类错误。

#include "ui/HudLayout.hpp"

#include <algorithm>
#include <cstddef>

namespace mc::ui {

// `AbstractSelectionList.getRowWidth()` 的默认值。各屏覆写，值见下面的常量。
inline constexpr int kScrollListDefaultRowWidth = 220;
// 各屏在 26.1 里的实际覆写值。写成具名常量而不是散在调用点，是因为它们是**从源码查来的
// 事实**，每一个都带出处；散出去以后没人知道 300 是查来的还是拍的。
inline constexpr int kLanguageRowWidth = 270;   // LanguageSelectScreen:141（220 + 50）
inline constexpr int kWorldSelectionRowWidth = 270; // WorldSelectionList:251
inline constexpr int kKeyBindsRowWidth = 340;   // KeyBindsList:59
inline constexpr int kOptionsRowWidth = 310;    // OptionsList:59

// `AbstractScrollArea.defaultSettings`：滚动条宽 6，滑块最小高 32。
inline constexpr int kScrollbarWidth = 6;
inline constexpr int kScrollbarMinimumThumb = 32;
// `scrollBarX() = getRowRight() + scrollbarWidth() + 2`（`AbstractSelectionList:283`）。
//
// ★ UI-6b 修：这里从前只加了 `kScrollbarRowGap`，**漏掉了 scrollbarWidth**，
//   于是三张列表的滚动条都比 vanilla 靠左 6 个逻辑像素。注释里的公式一直是对的，
//   错的是照它写出来的那行代码——而 `scroll_list_test` 当时是照**代码**写的断言，
//   等于把这个偏差钉住了。按键绑定行的两个按钮以这个 x 为基准（UI-6b），
//   偏差因此会一路传下去，这才撞出来。
inline constexpr int kScrollbarRowGap = 2;
// 视口上下缘那两条分隔带的高度。
//
// ★ UI-5 更正：**26.1 画的不是渐隐带，是两张 2px 的分隔纹理**。
//   `AbstractSelectionList.extractListSeparators():218-222` blit 的是
//   `gui/header_separator.png` 与 `gui/footer_separator.png`（有世界时换成
//   `inworld_` 那两张），尺寸 32x2，横向平铺，画在 `getY() - 2` 与 `getBottom()`。
//   4px 竖直渐隐是 1.20.2 之前的做法；偏差表 D9 当时按旧 spec 记成"渐隐带缺绘制"，
//   照它实现会画出一个 26.1 根本没有的元素。
inline constexpr int kScrollListSeparatorHeight = 2;

// 一个滚动列表的静态几何：视口、行宽、行高。
// 滚动位置**不在这里**——它是屏幕的状态，按需传进下面各函数。
struct ScrollList final {
    // 视口，逻辑像素。x 通常是整宽（行自己在里面居中）。
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int rowWidth = kScrollListDefaultRowWidth;
    int rowHeight = 20;

    [[nodiscard]] constexpr int right() const { return x + width; }
    [[nodiscard]] constexpr int bottom() const { return y + height; }

    // `getRowLeft() = getX() + width/2 - getRowWidth()/2`，整数除法。
    [[nodiscard]] constexpr int rowLeft() const { return x + width / 2 - rowWidth / 2; }
    [[nodiscard]] constexpr int rowRight() const { return rowLeft() + rowWidth; }

    // 视口里完整放得下几行。至少 1——高度为零的列表画不出东西，但也不该让除法炸掉。
    [[nodiscard]] constexpr std::size_t visibleRows() const {
        if (rowHeight <= 0 || height <= 0) {
            return 1U;
        }
        return static_cast<std::size_t>(std::max(height / rowHeight, 1));
    }

    // 内容总高，用于滑块比例。
    [[nodiscard]] constexpr int contentHeight(std::size_t itemCount) const {
        return static_cast<int>(itemCount) * rowHeight;
    }

    // 第一行下标的合法上界：再往下滚就露出列表末尾之后的空白。
    [[nodiscard]] constexpr std::size_t maximumFirstRow(std::size_t itemCount) const {
        const std::size_t window = visibleRows();
        return itemCount > window ? itemCount - window : 0U;
    }

    [[nodiscard]] constexpr bool scrollable(std::size_t itemCount) const {
        return itemCount > visibleRows();
    }
};

// 第 `visibleIndex` 个**可见**行的矩形（0 是视口最上面那一行）。
// 传的是可见序号而不是绝对序号：滚动偏移已经由调用方折算过，列表因此不必知道它。
[[nodiscard]] constexpr UiRect scrollListRow(const ScrollList& list, std::size_t visibleIndex) {
    return {
        static_cast<float>(list.rowLeft()),
        static_cast<float>(list.y + static_cast<int>(visibleIndex) * list.rowHeight),
        static_cast<float>(list.rowWidth),
        static_cast<float>(list.rowHeight),
    };
}

// 滚动条轨道。★ 贴的是**行的右缘**，不是视口右缘（spec §2.7 写错了那一条），
// 而且距离是 `宽 + 2` 不是 `2`。
[[nodiscard]] constexpr UiRect scrollListScrollbar(const ScrollList& list) {
    return {
        static_cast<float>(list.rowRight() + kScrollbarWidth + kScrollbarRowGap),
        static_cast<float>(list.y),
        static_cast<float>(kScrollbarWidth),
        static_cast<float>(list.height),
    };
}

// 滑块高度：`Mth.clamp(h * h / contentHeight, 32, h - 8)`（`AbstractScrollArea.scrollerHeight`）。
// 两端都要夹：内容极多时滑块不能细成一条线，内容刚好超一点时也不能几乎撑满轨道。
[[nodiscard]] constexpr int scrollListThumbHeight(const ScrollList& list, std::size_t itemCount) {
    const int content = list.contentHeight(itemCount);
    if (content <= 0 || list.height <= 0) {
        return kScrollbarMinimumThumb;
    }
    const int upper = std::max(list.height - 8, kScrollbarMinimumThumb);
    const int proportional = list.height * list.height / content;
    return std::clamp(proportional, kScrollbarMinimumThumb, upper);
}

// 滑块矩形，按第一行下标定位。
[[nodiscard]] constexpr UiRect scrollListThumb(const ScrollList& list, std::size_t itemCount,
                                               std::size_t firstRow) {
    const auto track = scrollListScrollbar(list);
    const int thumbHeight = scrollListThumbHeight(list, itemCount);
    const std::size_t maximum = list.maximumFirstRow(itemCount);
    if (maximum == 0U) {
        return {track.x, track.y, track.width, static_cast<float>(thumbHeight)};
    }
    const int travel = std::max(list.height - thumbHeight, 1);
    const auto clamped = std::min(firstRow, maximum);
    const int offset =
        static_cast<int>(static_cast<std::size_t>(travel) * clamped / maximum);
    return {track.x, static_cast<float>(list.y + offset), track.width,
            static_cast<float>(thumbHeight)};
}

// 光标在轨道上的位置 → 第一行下标。拖动滚动条走这条。
[[nodiscard]] inline std::size_t scrollListRowFromScrollbar(const ScrollList& list,
                                                            std::size_t itemCount,
                                                            float cursorLogicalY) {
    const std::size_t maximum = list.maximumFirstRow(itemCount);
    if (maximum == 0U) {
        return 0U;
    }
    const int thumbHeight = scrollListThumbHeight(list, itemCount);
    const float travel = static_cast<float>(std::max(list.height - thumbHeight, 1));
    // 抓住滑块中心：光标落在轨道顶端时应当是第 0 行，而不是"滑块顶端对齐光标"。
    const float offset = cursorLogicalY - static_cast<float>(list.y) -
                         static_cast<float>(thumbHeight) * 0.5F;
    const float fraction = std::clamp(offset / travel, 0.0F, 1.0F);
    return static_cast<std::size_t>(fraction * static_cast<float>(maximum) + 0.5F);
}

// 视口上/下缘那两条 2px 分隔带（`extractListSeparators`）。
//
// 注意它们**落在视口之外**：header 在 `y - 2`，footer 在 `bottom()`——分隔线是
// 列表与页眉/页脚之间的那道缝，不是盖在第一行/最后一行上的遮罩。从前按渐隐带的写法
// 画在视口**内部**，会盖掉第一行文字的上两像素。
[[nodiscard]] constexpr UiRect scrollListHeaderSeparator(const ScrollList& list) {
    return {static_cast<float>(list.x),
            static_cast<float>(list.y - kScrollListSeparatorHeight),
            static_cast<float>(list.width), static_cast<float>(kScrollListSeparatorHeight)};
}

[[nodiscard]] constexpr UiRect scrollListFooterSeparator(const ScrollList& list) {
    return {static_cast<float>(list.x), static_cast<float>(list.bottom()),
            static_cast<float>(list.width), static_cast<float>(kScrollListSeparatorHeight)};
}

} // namespace mc::ui

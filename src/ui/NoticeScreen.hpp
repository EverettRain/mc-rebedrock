#pragma once

// UI-11 / A5：全屏提示屏（26.1 `WarningScreen` + `SafetyScreen`）与它用的复选框
// （26.1 `Checkbox`）的**纯几何**。单位一律是逻辑像素，乘 GUI 缩放是调用方最后一步的事。
//
// 26.1 里这一屏是首次进入多人游戏时挡在前面的那块警告：标题、一段可换行的正文、
// 一个「不再显示」复选框，以及 Proceed / Back 两个按钮。本作把同一块屏用在**首次进入
// 高级图形设置**上——那一屏里的每一项都能把帧时间打下来一个数量级，值得先说一声。
//
// ★ 这里的每一条算术都照抄 26.1 的布局引擎，**不做代数化简**：
//   `FrameLayout` / `GridLayout` / `AbstractChildWrapper` 三层里，横向定位用的是
//   `(int)Mth.lerp(...)`（截断），纵向定位用的是 `Math.round(Mth.lerp(...))`（四舍五入）。
//   两者在奇数余量上差 1 像素，而"差 1 像素"正是这条线一直在抓的东西。
//
// ★ spec §2.5 关于复选框的两个数都是**旧值**，已按 26.1 源码更正：
//     - "尺寸 20x20（含 box）" —— 精灵美术确实是 20x20，但 `Checkbox.extractContents`
//       blit 的边长是 `getBoxSize(font)` = `9 + BOX_PADDING(8)` = **17**。
//     - "整体宽 = 24 + font.width(text)" —— `getDefaultWidth` 是
//       `getBoxSize(font) + 4 + font.width(message)` = **21 + 文字宽**。
//   两条都登记在 README 的偏差表里。

#include "ui/HudLayout.hpp"
#include "ui/TextMetrics.hpp"

#include <algorithm>
#include <cmath>

namespace mc::ui {

// ---- 26.1 `Checkbox` ----

// `Checkbox.SPACING`（盒子与右侧文字之间）。★ 26.1 的绘制代码里写的是字面量 4，
// 这个常量本身只用在别处；两个值相同，所以这里就一个。
inline constexpr int kCheckboxSpacing = 4;
// `Checkbox.BOX_PADDING`。
inline constexpr int kCheckboxBoxPadding = 8;
// `Checkbox.getBoxSize(font)` = `9 + BOX_PADDING`。**这就是 blit 出去的边长**，
// 不是精灵美术的 20——见文件头那段更正。
inline constexpr int kCheckboxBoxSize =
    static_cast<int>(kFontLineHeight) + kCheckboxBoxPadding;

// `Checkbox.getDefaultWidth`：盒子 + 4 + 文字宽。
[[nodiscard]] constexpr int checkboxWidth(int textWidth) {
    return kCheckboxBoxSize + kCheckboxSpacing + textWidth;
}

// `Checkbox.getAdjustedHeight`：盒子与文字块取高的那个。单行文字高 9 < 17，
// 所以一行的复选框高就是 17；这个函数存在是为了两行的那一档不静默塌回 17。
[[nodiscard]] constexpr int checkboxHeight(int textHeight) {
    return std::max(kCheckboxBoxSize, textHeight);
}

// 复选框内部：左边那个方盒，以及右侧文字的落点。
//
// ★ `textY` 的两次整数除法照抄 `Checkbox.extractContents`：
//     `getY() + boxSize / 2 - textWidget.getHeight() / 2`
//   先各自除 2 再相减，**不能**写成 `(boxSize - textHeight) / 2`——
//   17/2 - 9/2 = 8 - 4 = 4，而 (17-9)/2 = 4，这一组恰好相同；
//   换成两行文字（18）就是 8 - 9 = -1 与 (17-18)/2 = 0，差 1 像素。
struct CheckboxParts final {
    UiRect box{};
    float textX = 0.0F;
    float textY = 0.0F;
};

[[nodiscard]] constexpr CheckboxParts checkboxParts(const UiRect& rect, int textHeight) {
    const int boxSize = kCheckboxBoxSize;
    return {
        {rect.x, rect.y, static_cast<float>(boxSize), static_cast<float>(boxSize)},
        rect.x + static_cast<float>(boxSize + kCheckboxSpacing),
        rect.y + static_cast<float>(boxSize / 2 - textHeight / 2),
    };
}

// ---- 26.1 的布局引擎：两种取整 ----

// `AbstractChildWrapper.setX` / `FrameLayout.alignInDimension`：`(int)Mth.lerp(a, least, most)`。
// 居中时 a = 0.5，Java 的 `(int)` 是**朝零截断**。
[[nodiscard]] constexpr int layoutOffsetX(int least, int most) {
    return static_cast<int>(static_cast<float>(least) +
                            0.5F * static_cast<float>(most - least));
}

// `AbstractChildWrapper.setY`：同一条 lerp，外面套的却是 `Math.round`——
// Java 的 `Math.round(float)` 是 `floor(x + 0.5)`。横竖两个方向取整方式不同，
// 这不是笔误，是 26.1 的原样。
[[nodiscard]] inline int layoutOffsetY(int least, int most) {
    const float value =
        static_cast<float>(least) + 0.5F * static_cast<float>(most - least);
    return static_cast<int>(std::floor(value + 0.5F));
}

// ---- 26.1 `WarningScreen` ----

// `WarningScreen.MESSAGE_PADDING`：正文框比屏幕窄/矮的那个量。
inline constexpr int kNoticeMessagePadding = 100;
// 内容列与页脚列的 `LinearLayout.spacing(8)`，以及页脚两个按钮之间的横向间距。
inline constexpr int kNoticeSpacing = 8;
// `content.addChild(messageWidget, s -> s.padding(12))`：正文那一格四周的留白。
inline constexpr int kNoticeMessageCellPadding = 12;
// `AbstractTextAreaWidget.innerPadding()`：正文框内壁到文字的距离。
inline constexpr int kNoticeTextInnerPadding = 4;
// `Button.DEFAULT_WIDTH` / `DEFAULT_HEIGHT`。
inline constexpr int kNoticeButtonWidth = 150;
inline constexpr int kNoticeButtonHeight = 20;

// 正文框的外宽（`this.width - 100`）。
[[nodiscard]] constexpr int noticeMessageBoxWidth(int screenWidth) {
    return screenWidth - kNoticeMessagePadding;
}

// 正文换行用的宽度：外宽减去左右两侧的内壁留白
// （`new MultiLineTextWidget(...).setMaxWidth(getWidth() - totalInnerPadding())`）。
//
// ★ 装配侧（把正文拆成一行行 Label）与绘制侧必须用**这一个**函数换行，
//   两处各写各的就是"框里放得下三行、却按四行算了高度"。
[[nodiscard]] constexpr int noticeMessageWrapWidth(int screenWidth) {
    return noticeMessageBoxWidth(screenWidth) - kNoticeTextInnerPadding * 2;
}

// 正文框的外高。`minimizeHeight()` 只在**不滚动**时收缩：内容比 `height - 100`
// 还高时框保持原高（那时它是可滚的）。照抄那个条件，不写成 min——两者取值相同，
// 但条件式说得出"为什么"。
[[nodiscard]] constexpr int noticeMessageBoxHeight(int screenHeight, int lineCount) {
    const int maximum = screenHeight - kNoticeMessagePadding;
    const int content =
        lineCount * static_cast<int>(kFontLineHeight) + kNoticeTextInnerPadding * 2;
    return content > maximum ? maximum : content;
}

// 版面需要、而 `ui::` 这一层量不出来的两个文字宽度。
//
// ★ 它们**只**通过 `max` 参与内容列的宽度：26.1 的内容列宽是
//   `max(标题宽, 正文格宽, 页脚宽)`，而页脚宽又是 `max(复选框宽, 按钮行宽)`。
//   画布窄到 `screenWidth - 76 < 308` 时页脚才是最宽的那一个，那时这两个数就说了算。
struct NoticeMetrics final {
    int titleWidth = 0;
    int checkTextWidth = 0;
};

// 一屏提示的全部矩形，逻辑像素。
struct NoticeLayout final {
    UiRect title{};    // 标题文字（`StringWidget`，宽就是文字宽）
    UiRect message{};  // 正文框的**外框**；文字从 (x+4, y+4) 起逐行 9 像素
    UiRect check{};    // 复选框整体（盒子 + 右侧文字）
    UiRect proceed{};
    UiRect back{};
};

[[nodiscard]] inline NoticeLayout noticeLayout(int screenWidth, int screenHeight,
                                               int messageLineCount,
                                               const NoticeMetrics& metrics) {
    const int titleHeight = static_cast<int>(kFontLineHeight);
    const int messageWidth = noticeMessageBoxWidth(screenWidth);
    const int messageHeight = noticeMessageBoxHeight(screenHeight, messageLineCount);
    // 正文那一格带 12 的四周留白，所以格子比控件大 24。
    const int messageCellWidth = messageWidth + kNoticeMessageCellPadding * 2;
    const int messageCellHeight = messageHeight + kNoticeMessageCellPadding * 2;
    const int checkWidth = checkboxWidth(metrics.checkTextWidth);
    const int checkHeight = checkboxHeight(static_cast<int>(kFontLineHeight));
    const int buttonRowWidth = kNoticeButtonWidth * 2 + kNoticeSpacing;
    const int footerWidth = std::max(checkWidth, buttonRowWidth);
    const int footerHeight = checkHeight + kNoticeSpacing + kNoticeButtonHeight;
    const int columnWidth =
        std::max(metrics.titleWidth, std::max(messageCellWidth, footerWidth));
    const int columnHeight = titleHeight + kNoticeSpacing + messageCellHeight +
                             kNoticeSpacing + footerHeight;
    // `FrameLayout.centerInRectangle(layout, getRectangle())`：整块内容在**整屏**里居中。
    const int originX = layoutOffsetX(0, screenWidth - columnWidth);
    const int originY = layoutOffsetY(0, screenHeight - columnHeight);
    // 内容列是一列三行的 GridLayout，行距 8。
    const int messageRowY = originY + titleHeight + kNoticeSpacing;
    const int footerRowY = messageRowY + messageCellHeight + kNoticeSpacing;
    const int footerX = originX + layoutOffsetX(0, columnWidth - footerWidth);
    const int buttonRowX = footerX + layoutOffsetX(0, footerWidth - buttonRowWidth);
    const int buttonRowY = footerRowY + checkHeight + kNoticeSpacing;
    const auto rect = [](int x, int y, int width, int height) {
        return UiRect{static_cast<float>(x), static_cast<float>(y),
                      static_cast<float>(width), static_cast<float>(height)};
    };
    return {
        rect(originX + layoutOffsetX(0, columnWidth - metrics.titleWidth), originY,
             metrics.titleWidth, titleHeight),
        rect(originX + layoutOffsetX(kNoticeMessageCellPadding,
                                     columnWidth - messageWidth - kNoticeMessageCellPadding),
             messageRowY + layoutOffsetY(kNoticeMessageCellPadding,
                                         messageCellHeight - messageHeight -
                                             kNoticeMessageCellPadding),
             messageWidth, messageHeight),
        rect(footerX + layoutOffsetX(0, footerWidth - checkWidth), footerRowY, checkWidth,
             checkHeight),
        rect(buttonRowX, buttonRowY, kNoticeButtonWidth, kNoticeButtonHeight),
        rect(buttonRowX + kNoticeButtonWidth + kNoticeSpacing, buttonRowY, kNoticeButtonWidth,
             kNoticeButtonHeight),
    };
}

// 正文框里第 `line` 行文字的矩形。
//
// 26.1 `FittingMultiLineTextWidget.extractContents` 把画笔平移到
// `(getInnerLeft(), getInnerTop())` = `(x + 4, y + 4)`，再由 `MultiLineTextWidget`
// 以 `lineHeight = 9` 逐行往下走；`centered` 是 false，所以每行**左对齐**。
[[nodiscard]] constexpr UiRect noticeMessageLineRect(const UiRect& box, int line) {
    const auto inner = static_cast<float>(kNoticeTextInnerPadding);
    return {
        box.x + inner,
        box.y + inner + static_cast<float>(line) * kFontLineHeight,
        box.width - inner * 2.0F,
        kFontLineHeight,
    };
}

} // namespace mc::ui

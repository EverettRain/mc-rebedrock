#pragma once

// UI-6b：**一行里放得下多个控件**（26.1 的 `ContainerObjectSelectionList`）。
//
// 在这之前本作的一行**就是一个控件**：`ui::Page` 是扁平的 `vector<Widget>`，
// `RectProvider` 按控件序号给矩形，一个序号一个矩形。按键绑定那一行因此被画成
// 一整块底衬加一行 `"动作: 按键"` 文本、整行一次点击——而 26.1 那一行是
// **一段文本加两个按钮**（`KeyBindsList.KeyEntry.children()` 返回 changeButton 与
// resetButton 两个 GuiEventListener，键盘焦点在它们之间走）。
//
// 补这个能力**不需要改数据结构**：Page 仍是扁平列表，只是相邻的几个 Widget 映射到
// 同一行、各自拿到行内的一个格子。焦点遍历与点击派发本来就按 Widget 走，于是
// Tab 在行内两个控件之间移动这件事自动成立——正是 26.1 `children()` 的语义。
// 要补的只有**行内几何**，也就是这个文件。
//
// 为什么必须补：按键绑定以后还要不断加行（每加一个可绑定动作就是一行）。若行的形状
// 写死在渲染器的绘制函数里，每加一个按键都要动界面代码；把格子做成纯函数之后，
// 加一行只是往 `input::keyBindRows()` 里加一项。
//
// 单位一律逻辑像素整数（护栏 5）。数值逐条带 26.1 出处。

#include "ui/ScrollList.hpp"
#include "ui/Widget.hpp"
#include "ui/WidgetId.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mc::ui {

// 行内容相对行矩形的内缩（`AbstractSelectionList.Entry.getContentX/Y() = getX/Y() + 2`，
// `getContentWidth/Height() = getWidth/Height() - 4`）。
inline constexpr int kListRowPadding = 2;

// 一行的内容矩形。文本的左对齐基准与竖直居中都从它算。
[[nodiscard]] constexpr UiRect listRowContent(const UiRect& row) {
    return {row.x + static_cast<float>(kListRowPadding), row.y + static_cast<float>(kListRowPadding),
            row.width - static_cast<float>(2 * kListRowPadding),
            row.height - static_cast<float>(2 * kListRowPadding)};
}

// ---------------------------------------------------------------------------
// 按键绑定行（`KeyBindsList.KeyEntry`，26.1 controls/KeyBindsList.java:127-138）
// ---------------------------------------------------------------------------
//
//   resetButtonX  = scrollBarX() - resetButton.getWidth() - 10
//   buttonY       = getContentY() - 2          （也就是行顶本身）
//   changeButtonX = resetButtonX - 5 - changeButton.getWidth()
//   名称          = (getContentX(), getContentYMiddle() - 9/2)
//   冲突竖条      = fill(changeButton.getX() - 6, getContentY() - 1, +3, getContentBottom())
//
// ★ 两个按钮的基准是**滚动条的 x**，不是行右缘。两者差 `6 + 2`（滚动条宽加缝），
//   照行右缘算会把两个按钮整体右移 8 逻辑像素——在截图上看着"差不多对"。

// 一行按键绑定占几个控件序号：名称 Label + 改键 Button + 重置 Button = 3
// （26.1 `KeyBindsList.KeyEntry.children()` 交出 changeButton 与 resetButton 两个，
//  名称是它旁边那段文本）。
//
// 写成常量而不是散落的 `* 3`，是因为改这个数要同时改两处（页面装配与矩形映射），
// 而漏改一处的症状是"每一行的按钮都画在下一行的位置上"。
inline constexpr std::size_t kKeyBindWidgetsPerRow = 3U;

// 一个控件是不是绑定行的行内控件（名称 / 改键 / 重置）。
//
// 布局靠它把"列表里的控件"与"按钮网格里的控件"分开。用 debugId 判断而不是靠序号
// 区间，是因为序号区间要外部先告诉它"前几个是列表控件"——那又是一份可以和装配
// 不一致的信息。
[[nodiscard]] inline bool isKeyBindRowWidget(const Widget& widget) {
    return widget.debugId == static_cast<std::uint16_t>(WidgetId::KeyBindRow) ||
           widget.debugId == static_cast<std::uint16_t>(WidgetId::ResetKeyBind);
}

// UI-11 / A6：一个控件是不是**滚动列表的一行**（世界列表 / 语言列表），
// 或者跟在世界行后面的那张缩略图。
//
// ★ 它存在的理由是一个真实的隐患：这两种行此前落在 `frontendButtonRect` 的
//   `BottomBandTwoColumn` 分支上，也就是**拿到的是底部按钮的矩形**。绘制与命中
//   两侧都绕过 `Widget::rect`、各自去调 `worldListRow()` / `languageRow()`，
//   所以画面上看不出来；但 `bottomMenuButton` 对 `buttonCount > 20` 会抛
//   `menu button index or count is invalid`——画布一高、可见行一多（scale 1 的
//   720 逻辑高能放 28 行）就是闪退。收进这里之后行的矩形是真的，
//   "控件不越界"那条通用护栏也终于管得到它们。
[[nodiscard]] inline bool isScrollListRowWidget(const Widget& widget) {
    return widget.debugId == static_cast<std::uint16_t>(WidgetId::WorldRow) ||
           widget.debugId == static_cast<std::uint16_t>(WidgetId::LanguageRow) ||
           widget.debugId == static_cast<std::uint16_t>(WidgetId::WorldIcon);
}

inline constexpr int kKeyBindRowHeight = 20;      // KeyBindsList.ITEM_HEIGHT
inline constexpr int kKeyBindChangeWidth = 75;    // changeButton.bounds(0, 0, 75, 20)
inline constexpr int kKeyBindResetWidth = 50;     // resetButton.bounds(0, 0, 50, 20)
inline constexpr int kKeyBindButtonHeight = 20;
inline constexpr int kKeyBindResetGap = 10;       // resetButtonX 距 scrollBarX
inline constexpr int kKeyBindButtonGap = 5;       // 两个按钮之间
inline constexpr int kKeyBindStripeGap = 6;       // 竖条距 changeButton 左缘
inline constexpr int kKeyBindStripeWidth = 3;

// "Reset" 按钮：从滚动条的 x 往左退 50 + 10。
[[nodiscard]] constexpr UiRect keyBindResetCell(const ScrollList& list, const UiRect& row) {
    const float scrollbarX = scrollListScrollbar(list).x;
    return {scrollbarX - static_cast<float>(kKeyBindResetWidth + kKeyBindResetGap), row.y,
            static_cast<float>(kKeyBindResetWidth), static_cast<float>(kKeyBindButtonHeight)};
}

// 改键按钮：再往左退 5 + 75。
[[nodiscard]] constexpr UiRect keyBindChangeCell(const ScrollList& list, const UiRect& row) {
    const auto reset = keyBindResetCell(list, row);
    return {reset.x - static_cast<float>(kKeyBindButtonGap + kKeyBindChangeWidth), row.y,
            static_cast<float>(kKeyBindChangeWidth), static_cast<float>(kKeyBindButtonHeight)};
}

// 动作名称的锚点：行内容左缘，竖直居中于内容高（`getContentYMiddle() - 9/2`，
// 9 是 `ui::kFontLineHeight`）。返回的是文本框而不是一个点，居中因此是整数运算。
[[nodiscard]] constexpr UiRect keyBindNameCell(const UiRect& row, int textHeight) {
    const auto content = listRowContent(row);
    return {content.x, content.y + static_cast<float>(static_cast<int>(content.height) / 2 -
                                                      textHeight / 2),
            content.width, static_cast<float>(textHeight)};
}

// 冲突时画在改键按钮左侧的那条黄竖条（`graphics.fill(..., -256)`，-256 = 0xFFFFFF00）。
// 从行顶上方 1 像素起，到行内容底端。
[[nodiscard]] constexpr UiRect keyBindConflictStripe(const UiRect& changeCell, const UiRect& row) {
    const auto content = listRowContent(row);
    return {changeCell.x - static_cast<float>(kKeyBindStripeGap), content.y - 1.0F,
            static_cast<float>(kKeyBindStripeWidth),
            content.y + content.height - (content.y - 1.0F)};
}

// 冲突竖条与"正在等待按键"这两种状态下，改键按钮的文本装饰。
//
// 26.1 用 Component 的样式做（`[ … ]` 黄色 / `> … <` 黄色带下划线），本作的文本绘制
// 只有一个颜色参数，所以装饰是**加在字符串上**的。写在这里而不是绘制函数里，是因为
// "冲突时没有加括号"与"正在绑定时没有加尖括号"都不改变任何返回值，只改画面。
enum class KeyBindDecoration : std::uint8_t { None, Conflict, Capturing };

[[nodiscard]] inline std::string decorateKeyBindLabel(std::string_view keyName,
                                                      KeyBindDecoration decoration) {
    switch (decoration) {
    case KeyBindDecoration::Conflict:
        return "[ " + std::string{keyName} + " ]";
    case KeyBindDecoration::Capturing:
        return "> " + std::string{keyName} + " <";
    case KeyBindDecoration::None:
        break;
    }
    return std::string{keyName};
}

// 那两种装饰的文字颜色：26.1 都用 `ChatFormatting.YELLOW`（0xFFFFFF55）。
inline constexpr float kKeyBindDecoratedColorR = 1.0F;
inline constexpr float kKeyBindDecoratedColorG = 1.0F;
inline constexpr float kKeyBindDecoratedColorB = 85.0F / 255.0F;

} // namespace mc::ui

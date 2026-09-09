#pragma once

// UI-9：标签页导航栏（spec §5 的 L5，26.1 `TabNavigationBar` + `TabButton`）。
//
// 它是本作第一条标签栏。第一个消费者是创建世界那一屏（26.1 `CreateWorldScreen` 用
// `TabManager` 挂 Game / World / More 三页），而创造背包顶上那 11 个页签是**另一套**
// 画法——两处页签今天各写各的，这里立的是共用的那一份几何。
//
// ★ 它同时**取代页眉**：26.1 `CreateWorldScreen.repositionElements` 把
//   `layout.setHeaderHeight(tabNavigationBar.getRectangle().bottom())`——标签栏就是
//   这一屏的页眉，没有另一行标题。
//
// ## 整数除法的顺序照抄，不代数化简（README 护栏 23）
//
// 26.1 `TabNavigationBar.arrangeElements`（:165-176）：
//
//     tabsWidth = min(400, width) - 28
//     tabWidth  = Mth.roundToward(tabsWidth / tabs.size(), 2)   // 先整除，再向上取到偶数
//     layout.x  = Mth.roundToward((width - tabsWidth) / 2, 2)
//     layout.y  = 0
//
// `tabsWidth / tabs.size()` 是**整数除法**，它的结果才去向上取到 2 的倍数。把两步合成
// 一个式子（比如先乘再除）在实数上相等、在整数下差 1——而差 1 的页签宽会让三个页签
// 累计差 3 像素，最后一个页签与右边缘对不齐。

#include "ui/HudLayout.hpp"

#include <algorithm>
#include <cstddef>

namespace mc::ui {

// 26.1 `TabNavigationBar` 的三个常量。
inline constexpr int kTabBarHeight = 24;
inline constexpr int kTabBarMaxWidth = 400;
// 两侧各 14 —— 源码里是一个 `- 28`，这里写成"边距 × 2"好让它自己解释自己。
inline constexpr int kTabBarMargin = 14;

// 向上取整到 `factor` 的倍数（`Mth.roundToward`）。只用于正数。
[[nodiscard]] constexpr int roundToward(int value, int factor) {
    return ((value + factor - 1) / factor) * factor;
}

struct TabBarLayout final {
    // 整条栏（逻辑像素）。y 恒为 0：它贴着画布顶。
    UiRect bar{};
    int tabWidth = 0;
    std::size_t tabCount = 0;

    // 第 `index` 个页签。越界返回空矩形——调用方本来就该按 tabCount 循环。
    [[nodiscard]] constexpr UiRect tab(std::size_t index) const {
        if (index >= tabCount) {
            return {};
        }
        return {bar.x + static_cast<float>(static_cast<int>(index) * tabWidth), bar.y,
                static_cast<float>(tabWidth), bar.height};
    }

    [[nodiscard]] constexpr bool operator==(const TabBarLayout&) const = default;
};

[[nodiscard]] constexpr TabBarLayout tabBarLayout(int logicalWidth, std::size_t tabCount) {
    TabBarLayout out;
    out.tabCount = tabCount;
    if (tabCount == 0U) {
        return out;
    }
    const int tabsWidth = std::min(kTabBarMaxWidth, logicalWidth) - kTabBarMargin * 2;
    // ★ 先整除再向上取到偶数，两步分开——见文件头。
    out.tabWidth = roundToward(tabsWidth / static_cast<int>(tabCount), 2);
    const int barLeft = roundToward((logicalWidth - tabsWidth) / 2, 2);
    out.bar = {static_cast<float>(barLeft), 0.0F, static_cast<float>(tabsWidth),
               static_cast<float>(kTabBarHeight)};
    return out;
}

// 选中那个页签底下那条下划线（26.1 `TabButton.extractFocusUnderline`）：
// 宽 = min(文字宽, 页签宽 - 4)，水平居中，高 1，压在页签底边**上方 2 像素**处。
[[nodiscard]] constexpr UiRect tabUnderline(const UiRect& tab, int labelWidth) {
    const int width = std::min(labelWidth, static_cast<int>(tab.width) - 4);
    const int left = static_cast<int>(tab.x) + (static_cast<int>(tab.width) - width) / 2;
    return {static_cast<float>(left), tab.y + tab.height - 2.0F, static_cast<float>(width), 1.0F};
}

// 页签上那行字的可用横向范围与基线（26.1 `TabButton.extractLabel`）。
// ★ **选中的页签文字往上挪 3 像素**（`top = y + (selected ? 0 : 3)`）——选中的那张
//   精灵比未选中的高出一截，文字跟着贴上去，这是"选中的页签连着内容区"的视觉来源。
[[nodiscard]] constexpr UiRect tabLabelBox(const UiRect& tab, bool selected) {
    const float top = tab.y + (selected ? 0.0F : 3.0F);
    return {tab.x + 1.0F, top, tab.width - 2.0F, tab.y + tab.height - top};
}

} // namespace mc::ui

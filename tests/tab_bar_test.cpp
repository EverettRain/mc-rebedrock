// UI-9：标签页导航栏（spec §5 的 L5）的几何。
//
// ★ 断言钉的是 **26.1 `TabNavigationBar.arrangeElements`（:165-176）与
//   `TabButton`（:38-70）那几条式子算出来的数**，不是本作实现的抄本。
//   照实现写断言等于给缺陷发通行证（README 护栏 23 那一族）。

#include "ui/TabBar.hpp"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        std::printf("tab_bar_test line %d: %s\n", line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

// --- 1. Mth.roundToward -------------------------------------------------------
void testRoundToward() {
    CHECK(mc::ui::roundToward(0, 2) == 0);
    CHECK(mc::ui::roundToward(1, 2) == 2);
    CHECK(mc::ui::roundToward(2, 2) == 2);
    CHECK(mc::ui::roundToward(3, 2) == 4);
    CHECK(mc::ui::roundToward(123, 2) == 124);
}

// --- 2. 宽度分配：逐个照 vanilla 的式子手算 ----------------------------------
void testTabWidths() {
    // 画布 427（1280 @ scale 3 的逻辑宽），三个页签：
    //   tabsWidth = min(400, 427) - 28 = 372
    //   tabWidth  = roundToward(372 / 3, 2) = roundToward(124, 2) = 124
    //   barLeft   = roundToward((427 - 372) / 2, 2) = roundToward(27, 2) = 28
    const auto wide = mc::ui::tabBarLayout(427, 3U);
    CHECK(wide.bar.width == 372.0F);
    CHECK(wide.tabWidth == 124);
    CHECK(wide.bar.x == 28.0F);
    CHECK(wide.bar.y == 0.0F);
    CHECK(wide.bar.height == 24.0F);

    // 画布 320（spec §1.1 的最小逻辑画布），三个页签：
    //   tabsWidth = min(400, 320) - 28 = 292
    //   tabWidth  = roundToward(292 / 3, 2) = roundToward(97, 2) = 98
    //   barLeft   = roundToward((320 - 292) / 2, 2) = roundToward(14, 2) = 14
    const auto narrow = mc::ui::tabBarLayout(320, 3U);
    CHECK(narrow.bar.width == 292.0F);
    CHECK(narrow.tabWidth == 98);
    CHECK(narrow.bar.x == 14.0F);

    // ★ 宽画布下栏宽被 400 夹住——它不会跟着画布一起长。
    const auto huge = mc::ui::tabBarLayout(1920, 3U);
    CHECK(huge.bar.width == 372.0F);
    CHECK(huge.tabWidth == 124);
    // 夹住之后仍然居中：(1920 - 372) / 2 = 774，已是偶数。
    CHECK(huge.bar.x == 774.0F);

    // ★ **tabWidth 是先整除再向上取偶**，不是 tabsWidth 平均分。
    //   两个页签时 372/2 = 186 → 186（已是偶数）；四个时 372/4 = 93 → 94。
    CHECK(mc::ui::tabBarLayout(427, 2U).tabWidth == 186);
    CHECK(mc::ui::tabBarLayout(427, 4U).tabWidth == 94);
    // 而 4 × 94 = 376 > 372：**页签总宽可以超过栏宽**，这是 vanilla 向上取偶的结果，
    // 不是缺陷。把它"修"成正好铺满，就与 26.1 差开了。
    CHECK(mc::ui::tabBarLayout(427, 4U).tabWidth * 4 > 372);
}

// --- 3. 逐个页签的位置 --------------------------------------------------------
void testTabRects() {
    const auto bar = mc::ui::tabBarLayout(427, 3U);
    for (std::size_t i = 0; i < 3U; ++i) {
        const auto tab = bar.tab(i);
        check(tab.x == bar.bar.x + static_cast<float>(static_cast<int>(i) * bar.tabWidth),
              "tabs sit side by side from the bar's left edge", __LINE__);
        check(tab.y == 0.0F && tab.height == 24.0F, "every tab is 24 tall at the top",
              __LINE__);
        check(tab.width == static_cast<float>(bar.tabWidth), "every tab is the same width",
              __LINE__);
    }
    // 越界给空矩形而不是越界读。
    CHECK(bar.tab(3U).width == 0.0F);
    CHECK(mc::ui::tabBarLayout(427, 0U).tabCount == 0U);
}

// --- 4. 选中页签的下划线 ------------------------------------------------------
void testUnderline() {
    const auto bar = mc::ui::tabBarLayout(427, 3U);
    const auto tab = bar.tab(1U);
    // 文字比页签窄：下划线取文字宽，居中。
    const auto narrow = mc::ui::tabUnderline(tab, 40);
    CHECK(narrow.width == 40.0F);
    CHECK(narrow.x == tab.x + (tab.width - 40.0F) / 2.0F);
    CHECK(narrow.height == 1.0F);
    // ★ 压在页签底边**上方 2 像素**处（`top = y + height - 2`）。
    CHECK(narrow.y == tab.y + tab.height - 2.0F);

    // 文字比页签宽：被夹到 tabWidth - 4。
    const auto clamped = mc::ui::tabUnderline(tab, 1000);
    CHECK(clamped.width == tab.width - 4.0F);
    CHECK(clamped.x == tab.x + 2.0F);
}

// --- 5. 标签文字的盒子：选中的往上挪 3 -------------------------------------
void testLabelBox() {
    const auto bar = mc::ui::tabBarLayout(427, 3U);
    const auto tab = bar.tab(0U);
    const auto unselected = mc::ui::tabLabelBox(tab, false);
    const auto selected = mc::ui::tabLabelBox(tab, true);
    // ★ 这 3 像素是"选中的页签连着内容区"的视觉来源：选中那张精灵比未选中的高出
    //   一截，文字跟着贴上去（26.1 `TabButton.extractLabel`：`y + (selected ? 0 : 3)`）。
    CHECK(selected.y == tab.y);
    CHECK(unselected.y == tab.y + 3.0F);
    CHECK(selected.height == tab.height);
    CHECK(unselected.height == tab.height - 3.0F);
    // 左右各留 1 像素。
    CHECK(selected.x == tab.x + 1.0F);
    CHECK(selected.width == tab.width - 2.0F);
}

} // namespace

int main() {
    testRoundToward();
    testTabWidths();
    testTabRects();
    testUnderline();
    testLabelBox();
    if (failures != 0) {
        std::printf("tab_bar_test: %d checks failed\n", failures);
        return 1;
    }
    return 0;
}

// UI-4：滚动列表的几何（GUI spec §2.7 / §5 的范式 L3）。
//
// 在这之前本仓有三份各写各的列表几何，行宽 270 / 300 / 300、滚动条各是各的。这里断言的是
// **从 26.1 源码查来的数**，不是 spec §2.7 的正文——那一节有两处是 1.20.2 之前的旧状态：
//   H. 「滚动条贴在视口右侧 x1-6」→ 实际 `行右缘 + 6 + 2`（`AbstractSelectionList:283`）
//   I. 「轨道 0xFF000000、滑块 0xFF808080 + 亮边」→ 实际用精灵画（`AbstractScrollArea:143`）
// 行宽的覆写值也是查来的：语言 270、按键 340、世界列表 270、Options 310。

#include "ui/HudLayout.hpp"
#include "ui/MenuGeometry.hpp"
#include "ui/ScrollList.hpp"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        std::printf("scroll_list_test line %d: %s\n", line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

// 一个 320 宽、200 高的视口，行宽 220（vanilla 默认），行高 20 —— 十行正好放满。
[[nodiscard]] mc::ui::ScrollList sample() {
    return mc::ui::ScrollList{0, 30, 320, 200, mc::ui::kScrollListDefaultRowWidth, 20};
}

// --- 1. 行的水平位置：`x + width/2 - rowWidth/2`，整数除法 --------------------
void testRowGeometry() {
    const auto list = sample();
    CHECK(list.rowLeft() == 50);    // 0 + 160 - 110
    CHECK(list.rowRight() == 270);
    CHECK(list.visibleRows() == 10U);

    const auto first = mc::ui::scrollListRow(list, 0U);
    CHECK(first.x == 50.0F);
    CHECK(first.y == 30.0F);
    CHECK(first.width == 220.0F);
    CHECK(first.height == 20.0F);
    // 行按行高步进，没有额外缝隙——缝是条目自己画出来的，不是列表的事
    CHECK(mc::ui::scrollListRow(list, 3U).y == 90.0F);

    // 奇数差：整数除法向下取整（spec §1.2 的规矩一路管到列表）
    const mc::ui::ScrollList odd{0, 0, 321, 100, 220, 20};
    CHECK(odd.rowLeft() == 50);     // 321/2 = 160, 220/2 = 110
    const mc::ui::ScrollList oddRow{0, 0, 320, 100, 221, 20};
    CHECK(oddRow.rowLeft() == 50);  // 160 - 110（221/2 截断成 110）
}

// --- 2. 滚动量的钳制 ---------------------------------------------------------
void testScrollBounds() {
    const auto list = sample();
    CHECK(!list.scrollable(10U));            // 正好放满就不该有滚动条
    CHECK(list.maximumFirstRow(10U) == 0U);
    CHECK(list.scrollable(11U));
    CHECK(list.maximumFirstRow(11U) == 1U);
    CHECK(list.maximumFirstRow(100U) == 90U);
    // 条目比一屏还少
    CHECK(!list.scrollable(3U));
    CHECK(list.maximumFirstRow(3U) == 0U);
    CHECK(list.maximumFirstRow(0U) == 0U);

    // 高度或行高为零不该把除法炸掉
    const mc::ui::ScrollList degenerate{0, 0, 100, 0, 220, 0};
    CHECK(degenerate.visibleRows() == 1U);
}

// --- 3. 滚动条：位置、宽度、滑块高度公式 --------------------------------------
void testScrollbar() {
    const auto list = sample();
    const auto track = mc::ui::scrollListScrollbar(list);
    // ★ 贴的是**行的右缘**，公式是 `getRowRight() + scrollbarWidth() + 2`
    //   （`AbstractSelectionList:283`）。视口右缘是 320，行右缘是 270 → 270 + 6 + 2。
    //
    //   UI-6b 更正：这条断言原本写的是 272，也就是**漏掉了 scrollbarWidth 的那个实现**
    //   ——注释里的公式一直是对的，代码与断言是照着错的那行写的。按断言写断言就是这样
    //   把一个偏差钉住的，所以这里把三项都摊开：
    CHECK(track.x == static_cast<float>(list.rowRight() + mc::ui::kScrollbarWidth +
                                        mc::ui::kScrollbarRowGap));
    CHECK(track.x == 278.0F);
    CHECK(track.x != 272.0F);                  // 漏掉 scrollbarWidth 会给出这个数
    CHECK(track.x != 314.0F);                  // spec §2.7 的 `x1 - 6` 会给出这个数
    CHECK(track.width == 6.0F);
    CHECK(track.y == 30.0F);
    CHECK(track.height == 200.0F);

    // 滑块高度 = clamp(h*h/内容高, 32, h-8)
    // 内容 100 行 = 2000：200*200/2000 = 20 -> 被下限 32 顶住
    CHECK(mc::ui::scrollListThumbHeight(list, 100U) == 32);
    // 内容 12 行 = 240：200*200/240 = 166 -> 落在区间内
    CHECK(mc::ui::scrollListThumbHeight(list, 12U) == 166);
    // 内容 11 行 = 220：200*200/220 = 181 -> 被上限 h-8 = 192 放行
    CHECK(mc::ui::scrollListThumbHeight(list, 11U) == 181);
    // 内容刚好一屏：200*200/200 = 200 -> 被上限 192 夹住
    CHECK(mc::ui::scrollListThumbHeight(list, 10U) == 192);

    // 滑块位置：第 0 行贴顶，最后一行贴底
    const auto top = mc::ui::scrollListThumb(list, 100U, 0U);
    CHECK(top.y == 30.0F);
    CHECK(top.width == 6.0F);
    CHECK(top.height == 32.0F);
    const auto bottom = mc::ui::scrollListThumb(list, 100U, 90U);
    CHECK(bottom.y + bottom.height == 30.0F + 200.0F);
    // 超出上界也钳住，不会画到轨道外面
    const auto beyond = mc::ui::scrollListThumb(list, 100U, 999U);
    CHECK(beyond.y == bottom.y);
    // 不可滚动时滑块贴顶（vanilla 干脆不画它，但几何仍要是良定义的）
    const auto still = mc::ui::scrollListThumb(list, 5U, 0U);
    CHECK(still.y == 30.0F);
}

// --- 4. 拖拽滚动条 → 第一行下标 ----------------------------------------------
void testScrollbarDrag() {
    const auto list = sample();
    // 抓住滑块中心：光标在轨道顶端就是第 0 行
    CHECK(mc::ui::scrollListRowFromScrollbar(list, 100U, 30.0F) == 0U);
    CHECK(mc::ui::scrollListRowFromScrollbar(list, 100U, 0.0F) == 0U);       // 轨道之上
    CHECK(mc::ui::scrollListRowFromScrollbar(list, 100U, 500.0F) == 90U);    // 轨道之下
    const auto middle = mc::ui::scrollListRowFromScrollbar(list, 100U, 130.0F);
    CHECK(middle > 0U && middle < 90U);
    // 不可滚动的列表永远停在第 0 行
    CHECK(mc::ui::scrollListRowFromScrollbar(list, 5U, 200.0F) == 0U);
}

// --- 5. 分隔带 ---------------------------------------------------------------
//
// ★ UI-5 更正：26.1 画的是两张 2px 分隔纹理，不是 4px 竖直渐隐带
// （`AbstractSelectionList.extractListSeparators():218-222`）。偏差表 D9 原按旧 spec
// 记成"渐隐带缺绘制"，那是 1.20.2 之前的元素。这里钉住两件事：高度是 2，
// 以及它们落在视口**之外**——画进视口里会盖掉第一行文字的上两像素。
void testSeparators() {
    const auto list = sample();
    const auto header = mc::ui::scrollListHeaderSeparator(list);
    const auto footer = mc::ui::scrollListFooterSeparator(list);
    CHECK(mc::ui::kScrollListSeparatorHeight == 2);
    CHECK(header.height == 2.0F && footer.height == 2.0F);
    // header 在 y-2，footer 在 bottom()——都在视口外
    CHECK(header.y == 28.0F);
    CHECK(header.y + header.height == 30.0F);
    CHECK(footer.y == 230.0F);
    CHECK(header.width == 320.0F && footer.width == 320.0F);
    // 第一行与最后一行都不被压住
    const auto first = mc::ui::scrollListRow(list, 0U);
    CHECK(header.y + header.height <= first.y);
    CHECK(footer.y >= list.bottom());
}

// --- 6. 三张屏用的是查来的行宽，不是自造的 ------------------------------------
//
// 这一组是本轮最容易悄悄退回去的地方：行宽是三个字面量，改回 300 不会有任何东西崩。
void testScreenRowWidths() {
    CHECK(mc::ui::kScrollListDefaultRowWidth == 220);  // AbstractSelectionList:383
    CHECK(mc::ui::kLanguageRowWidth == 270);           // LanguageSelectScreen:141（220+50）
    CHECK(mc::ui::kKeyBindsRowWidth == 340);           // KeyBindsList:59
    CHECK(mc::ui::kWorldSelectionRowWidth == 270);     // WorldSelectionList:251
    CHECK(mc::ui::kOptionsRowWidth == 310);            // OptionsList:59
    // 三张屏各不相同——这正是"一个共同基座加逐屏覆写"的形状，
    // 而不是"三份代码各写各的"（后者才是 UI-4 之前的状态）
    CHECK(mc::ui::kKeyBindsRowWidth != mc::ui::kLanguageRowWidth);

    constexpr float kWidth = 1280.0F;
    const mc::ui::HudLayout layout{kWidth, 720.0F, 3};   // 逻辑 427x240
    CHECK(mc::ui::languageScrollList(layout, kWidth).rowWidth == mc::ui::kLanguageRowWidth);
    CHECK(mc::ui::keyBindsScrollList(layout, kWidth).rowWidth == mc::ui::kKeyBindsRowWidth);
    CHECK(mc::ui::worldScrollList(layout, kWidth).rowWidth == mc::ui::kWorldSelectionRowWidth);
    // 三张列表都铺满逻辑画布的宽度，行在里面居中
    CHECK(mc::ui::languageScrollList(layout, kWidth).width == 427);
    CHECK(mc::ui::keyBindsScrollList(layout, kWidth).width == 427);
}

} // namespace

int main() {
    testRowGeometry();
    testScrollBounds();
    testScrollbar();
    testScrollbarDrag();
    testSeparators();
    testScreenRowWidths();
    if (failures != 0) {
        std::printf("scroll_list_test: %d checks failed\n", failures);
        return 1;
    }
    return 0;
}

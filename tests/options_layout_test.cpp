// UI-6a / UI-6b：三段式版面、设置项双列列表、以及"一行里放得下多个控件"。
//
// 这三样是 §7 设置子树的地基。它们在这一轮之前**一个都不存在**：本作每一页都是
// "一条按钮带"，每行一个按钮，几何由 frontendButtonRect 按控件序号直接给。
//
// 断言的全部是从 26.1 源码查来的数，逐条带出处（`/workspace/mc-26.1-java/minecraft-src`）：
//   HeaderAndFooterLayout.java:10-11,102-104   页眉/页脚 33、内容首选间距 30
//   OptionsSubScreen.java:52                   页脚 Done 宽 200
//   OptionsList.java:17-18,107,148             行宽 310、行高 25、列偏移 160、起点 W/2-155
//   OptionInstance.java:117                    小按钮宽 150
//   AbstractSelectionList.java:471-488         行内容内缩 2
//   controls/KeyBindsList.java:21,101-103,127-138  按键行高 20、按钮 75/50、间距 10/5、竖条 6/3

#include "ui/HeaderAndFooterLayout.hpp"
#include "ui/ListRow.hpp"
#include "ui/OptionsList.hpp"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        std::printf("options_layout_test line %d: %s\n", line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

// 1280x720 配 GUI scale 3 的逻辑画布：ceil(1280/3) x ceil(720/3) = 427 x 240。
// 非整除是常态（护栏 5），所以基准夹具用它而不是一个整除的尺寸。
constexpr int kLogicalWidth = 427;
constexpr int kLogicalHeight = 240;

// --- 1. 三段式版面 -----------------------------------------------------------
void testHeaderAndFooter() {
    const auto layout = mc::ui::headerAndFooterLayout(kLogicalWidth, kLogicalHeight);
    CHECK(mc::ui::kHeaderAndFooterHeight == 33);   // DEFAULT_HEADER_AND_FOOTER_HEIGHT
    CHECK(mc::ui::kContentMarginTop == 30);        // CONTENT_MARGIN_TOP
    CHECK(mc::ui::kFooterButtonWidth == 200);      // OptionsSubScreen:52

    // getContentHeight() = height - header - footer
    CHECK(layout.contentHeight() == 240 - 33 - 33);
    CHECK(layout.contentHeight() == 174);

    // 页眉贴顶、页脚贴底，两者之间正好是内容区——三块首尾相接、不重叠、不留缝。
    const auto header = layout.headerBox();
    const auto content = layout.contentBox();
    const auto footer = layout.footerBox();
    CHECK(header.y == 0.0F && header.height == 33.0F);
    CHECK(content.y == header.y + header.height);
    CHECK(content.y + content.height == footer.y);
    CHECK(footer.y + footer.height == 240.0F);
    CHECK(header.width == 427.0F && content.width == 427.0F && footer.width == 427.0F);

    // ★ 内容区**占满**页眉与页脚之间的整块（OptionsList 的构造就是这么取的），
    //   这与 contentsFrame 那个 min() 是两回事——后者只管固定高度的内容块。
    CHECK(content.height == 174.0F);

    // fixedContentY：两项取小。内容矮时是 header + 30；内容高到会顶进页脚时改用后者。
    CHECK(layout.fixedContentY(20) == 63);          // min(33 + 30, 240 - 33 - 20) = min(63, 187)
    CHECK(layout.fixedContentY(160) == 47);         // min(63, 240 - 33 - 160) = min(63, 47)
    // 只取前者，高内容会伸进页脚；只取后者，矮内容会被贴到页脚正上方
    CHECK(layout.fixedContentY(160) != 63);
    CHECK(layout.fixedContentY(20) != 187);

    // 标题在页眉里双向居中，整数除法
    const auto title = layout.headerTitle(100, 9);
    CHECK(title.x == static_cast<float>(427 / 2 - 100 / 2));   // 213 - 50 = 163
    CHECK(title.x == 163.0F);
    CHECK(title.y == static_cast<float>(33 / 2 - 9 / 2));      // 16 - 4 = 12
    CHECK(title.y == 12.0F);

    // Done 按钮在页脚里双向居中
    const auto done = layout.footerButton();
    CHECK(done.width == 200.0F && done.height == 20.0F);
    CHECK(done.x == static_cast<float>(427 / 2 - 200 / 2));    // 213 - 100 = 113
    CHECK(done.y == footer.y + static_cast<float>(33 / 2 - 20 / 2));  // +6
    CHECK(done.y + done.height <= footer.y + footer.height);   // 不越出页脚
}

// --- 2. 设置项双列列表 -------------------------------------------------------
void testOptionsList() {
    CHECK(mc::ui::kOptionsRowHeight == 25);        // DEFAULT_ITEM_HEIGHT
    CHECK(mc::ui::kOptionsBigWidth == 310);        // BIG_BUTTON_WIDTH
    CHECK(mc::ui::kOptionsSmallWidth == 150);      // createButton 的默认宽
    CHECK(mc::ui::kOptionsColumnOffset == 160);    // X_OFFSET
    // ★ 行高 25 比控件高 20 高出 5，那 5 是行距。把行高写成 20 会让整列挤在一起，
    //   而每一行单看都"没问题"。
    CHECK(mc::ui::kOptionsRowHeight > mc::ui::kOptionsWidgetHeight);

    const auto layout = mc::ui::headerAndFooterLayout(kLogicalWidth, kLogicalHeight);
    const auto list = mc::ui::optionsScrollList(layout.contentBox());
    CHECK(list.rowWidth == 310);
    CHECK(list.rowHeight == 25);
    CHECK(list.y == 33);                            // 视口从页眉底端起
    CHECK(list.height == 174);
    // 174 / 25 = 6 行完整可见
    CHECK(list.visibleRows() == 6U);

    // ★ 起点与 26.1 的 `screen.width/2 - 155` 相等：rowLeft = 0 + 427/2 - 310/2 = 213 - 155
    CHECK(list.rowLeft() == kLogicalWidth / 2 - 155);
    CHECK(list.rowLeft() == 58);

    const auto left = mc::ui::optionsSmallCell(list, 0U, 0);
    const auto right = mc::ui::optionsSmallCell(list, 0U, 1);
    CHECK(left.x == 58.0F);
    CHECK(right.x == 58.0F + 160.0F);
    CHECK(left.width == 150.0F && right.width == 150.0F);
    // 控件在格子里下移 2（Entry.getContentY）
    CHECK(left.y == static_cast<float>(list.y + mc::ui::kListEntryPadding));
    CHECK(left.height == 20.0F);

    // ★ 闭合：两列加中缝正好铺满行宽。第二列的右缘必须落在行右缘上。
    CHECK(right.x + right.width == static_cast<float>(list.rowRight()));
    CHECK(left.x == static_cast<float>(list.rowLeft()));
    // 两列不重叠，中间正好 10
    CHECK(right.x - (left.x + left.width) == 10.0F);

    // addBig 独占一行，宽度等于行宽
    const auto big = mc::ui::optionsBigCell(list, 0U);
    CHECK(big.x == static_cast<float>(list.rowLeft()));
    CHECK(big.width == 310.0F);
    CHECK(big.width == static_cast<float>(list.rowWidth));

    // 行按行高步进
    CHECK(mc::ui::optionsSmallCell(list, 3U, 0).y - left.y == 3.0F * 25.0F);

    // n 个设置项占 ceil(n/2) 行
    CHECK(mc::ui::optionsSmallRowCount(0U) == 0U);
    CHECK(mc::ui::optionsSmallRowCount(1U) == 1U);   // 落单的一项自己占一行
    CHECK(mc::ui::optionsSmallRowCount(2U) == 1U);
    CHECK(mc::ui::optionsSmallRowCount(7U) == 4U);
    CHECK(mc::ui::optionsSmallRowCount(28U) == 14U); // 26.1 Video Settings 的规模
}

// --- 3. 一行里的多个控件（按键绑定行） ---------------------------------------
void testKeyBindRowCells() {
    CHECK(mc::ui::kKeyBindRowHeight == 20);      // KeyBindsList.ITEM_HEIGHT
    CHECK(mc::ui::kKeyBindChangeWidth == 75);
    CHECK(mc::ui::kKeyBindResetWidth == 50);
    CHECK(mc::ui::kListRowPadding == 2);

    // 按键列表：行宽 340（UI-4 已查），行高 20
    const auto layout = mc::ui::headerAndFooterLayout(kLogicalWidth, kLogicalHeight);
    const auto box = layout.contentBox();
    const mc::ui::ScrollList list{static_cast<int>(box.x),     static_cast<int>(box.y),
                                  static_cast<int>(box.width), static_cast<int>(box.height),
                                  mc::ui::kKeyBindsRowWidth,   mc::ui::kKeyBindRowHeight};
    const auto row = mc::ui::scrollListRow(list, 0U);
    const auto reset = mc::ui::keyBindResetCell(list, row);
    const auto change = mc::ui::keyBindChangeCell(list, row);
    const auto scrollbar = mc::ui::scrollListScrollbar(list);

    // ★ 两个按钮的基准是**滚动条的 x**，不是行右缘。两者差 6 + 2。
    CHECK(reset.x == scrollbar.x - 50.0F - 10.0F);
    CHECK(reset.x != static_cast<float>(list.rowRight()) - 50.0F - 10.0F);
    CHECK(scrollbar.x - static_cast<float>(list.rowRight()) == 8.0F);

    CHECK(change.x == reset.x - 5.0F - 75.0F);
    CHECK(change.width == 75.0F && reset.width == 50.0F);
    CHECK(change.height == 20.0F && reset.height == 20.0F);
    // 两个按钮都贴行顶（buttonY = getContentY() - 2 = 行顶）
    CHECK(change.y == row.y && reset.y == row.y);
    // 不重叠，改键在左、重置在右
    CHECK(change.x + change.width < reset.x);

    // 名称文本：行内容左缘，竖直居中
    const auto name = mc::ui::keyBindNameCell(row, 9);
    const auto content = mc::ui::listRowContent(row);
    CHECK(name.x == row.x + 2.0F);
    CHECK(name.x == content.x);
    CHECK(name.y == content.y + static_cast<float>(static_cast<int>(content.height) / 2 - 9 / 2));
    // 名称不会伸到改键按钮底下（列表够宽时）
    CHECK(name.x < change.x);

    // 冲突竖条在改键按钮左侧 6，宽 3
    const auto stripe = mc::ui::keyBindConflictStripe(change, row);
    CHECK(stripe.x == change.x - 6.0F);
    CHECK(stripe.width == 3.0F);
    CHECK(stripe.x + stripe.width < change.x);   // 不盖住按钮
    CHECK(stripe.y == content.y - 1.0F);
    CHECK(stripe.y + stripe.height == content.y + content.height);

    // 行内容矩形四边各内缩 2
    CHECK(content.x == row.x + 2.0F && content.y == row.y + 2.0F);
    CHECK(content.width == row.width - 4.0F && content.height == row.height - 4.0F);
}

// --- 4. 改键按钮的文本装饰 ---------------------------------------------------
//
// 26.1 用 Component 样式做（`[ … ]` 黄 / `> … <` 黄带下划线）；本作的文本绘制只有一个
// 颜色参数，所以装饰加在字符串上。这两件事都只改画面、不改任何返回值——把它们做成
// 纯函数正是为了让它们有地方变红（护栏 13）。
void testKeyBindDecoration() {
    using mc::ui::KeyBindDecoration;
    CHECK(mc::ui::decorateKeyBindLabel("W", KeyBindDecoration::None) == "W");
    CHECK(mc::ui::decorateKeyBindLabel("W", KeyBindDecoration::Conflict) == "[ W ]");
    CHECK(mc::ui::decorateKeyBindLabel("W", KeyBindDecoration::Capturing) == "> W <");
    // 两种装饰必须互相区分得开：都用方括号的话，"这个键冲突了"与"正在等你按键"
    // 在屏幕上就是同一个样子。
    CHECK(mc::ui::decorateKeyBindLabel("W", KeyBindDecoration::Conflict) !=
          mc::ui::decorateKeyBindLabel("W", KeyBindDecoration::Capturing));
    CHECK(mc::ui::decorateKeyBindLabel("", KeyBindDecoration::Capturing) == ">  <");
}

} // namespace

int main() {
    testHeaderAndFooter();
    testOptionsList();
    testKeyBindRowCells();
    testKeyBindDecoration();
    if (failures != 0) {
        std::printf("options_layout_test: %d checks failed\n", failures);
        return 1;
    }
    return 0;
}

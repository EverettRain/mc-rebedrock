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

#include "input/InputNaming.hpp"
#include "ui/HeaderAndFooterLayout.hpp"
#include "ui/KeyBindList.hpp"
#include "ui/MenuGeometry.hpp"
#include "ui/PageBuilder.hpp"
#include "ui/PageLayoutKind.hpp"
#include "ui/PageTitles.hpp"
#include "ui/ListRow.hpp"
#include "ui/CreateWorldLayout.hpp"
#include "ui/DualColumnList.hpp"
#include "ui/OptionsList.hpp"
#include "ui/OptionSlider.hpp"
#include "ui/SliderGeometry.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>

#ifndef MC_REBEDROCK_RENDERER_SRC
#error "MC_REBEDROCK_RENDERER_SRC must point at src/render/vulkan/VulkanRenderer.cpp"
#endif
#ifndef MC_REBEDROCK_HUD_RENDERER_SRC
#error "MC_REBEDROCK_HUD_RENDERER_SRC must point at src/render/vulkan/HudRenderer.hpp"
#endif
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

// --- 5. addSmall 的分组：每次调用从新行起 ------------------------------------
//
// 26.1 的 `list.addSmall(...)` **每次调用从新行开始**，组内才两两配对
// （`OptionsList.java:31-36`）。摊平成一组两两配对会让两次调用之间那道语义边界消失：
// ControlsScreen 的 `addSmall(mouse, keybinds)` 与 `addSmall(七个设置项)` 之间正是
// 这样一道边界，合并后 Key Binds… 会和 Sneak 挤在同一行。
//
// 这只改排版、不改任何返回值以外的东西——所以它必须在这里有断言。
void testGroupedSlots() {
    constexpr std::array<mc::ui::OptionsGroup, 2> kTwoThenThree{{{2U, mc::ui::OptionsGroupKind::Small}, {3U, mc::ui::OptionsGroupKind::Small}}};
    const auto slot = [&](std::size_t i) {
        return mc::ui::optionsGroupedSlot(kTwoThenThree, i);
    };
    // 组 0（两项）占行 0
    CHECK(slot(0) == (mc::ui::OptionsSlot{0U, 0}));
    CHECK(slot(1) == (mc::ui::OptionsSlot{0U, 1}));
    // ★ 组 1 从**新行**起，即使组 0 的那一行还空着右边也不许续上
    CHECK(slot(2) == (mc::ui::OptionsSlot{1U, 0}));
    CHECK(slot(3) == (mc::ui::OptionsSlot{1U, 1}));
    CHECK(slot(4) == (mc::ui::OptionsSlot{2U, 0}));
    CHECK(mc::ui::optionsGroupedRowCount(kTwoThenThree) == 3U);

    // 落单的一项占一整行，下一组仍从新行起
    constexpr std::array<mc::ui::OptionsGroup, 2> kOneThenSeven{{{1U, mc::ui::OptionsGroupKind::Small}, {7U, mc::ui::OptionsGroupKind::Small}}};
    CHECK(mc::ui::optionsGroupedSlot(kOneThenSeven, 0) == (mc::ui::OptionsSlot{0U, 0}));
    // ★ 这一条就是 Controls 枢纽：Key Binds… 独占行 0，七个设置项从行 1 起
    CHECK(mc::ui::optionsGroupedSlot(kOneThenSeven, 1) == (mc::ui::OptionsSlot{1U, 0}));
    CHECK(mc::ui::optionsGroupedSlot(kOneThenSeven, 2) == (mc::ui::OptionsSlot{1U, 1}));
    // index 7 是组 1 的第 6 项：行 1 + 6/2 = 行 4 的左列（七项占四行，最后一项落单）
    CHECK(mc::ui::optionsGroupedSlot(kOneThenSeven, 7) == (mc::ui::OptionsSlot{4U, 0}));
    CHECK(mc::ui::optionsGroupedRowCount(kOneThenSeven) == 5U);
    // 摊平成一组会给出不同的答案——那正是这条断言要挡住的写法
    constexpr std::array<mc::ui::OptionsGroup, 1> kFlat{{{8U, mc::ui::OptionsGroupKind::Small}}};
    CHECK(mc::ui::optionsGroupedSlot(kFlat, 1) != mc::ui::optionsGroupedSlot(kOneThenSeven, 1));
}

// --- 6. Controls 枢纽的实际排版 ----------------------------------------------
//
// 走**生产路径**：装配一遍、布局一遍，然后看控件的矩形。
// 第一行只有左列有东西，第二行两列都有，Done 在页脚。
void testControlsHubLayout() {
    const mc::ui::HudLayout layout{1280.0F, 720.0F, 3};
    mc::ui::MenuBuildContext ctx;
    const mc::ui::MenuCallbacks cb;
    mc::ui::Page page;
    mc::ui::buildPageInto(page, mc::ui::PageId::Controls, ctx, cb);
    mc::ui::layoutPageInto(page, mc::ui::PageId::Controls, layout, 1280.0F);
    // 两个跳转 + 七个设置项 + Done。★ 这个 10 是数出来的，不是另一张表说的。
    const std::size_t count = mc::ui::countPageButtons(page);
    CHECK(count == 10U);
    CHECK(page.size() == count);   // 枢纽页没有列表控件
    const auto rect = [&](std::size_t index) { return page[index].rect; };
    // ★ 第一行是**两个**跳转（`addSmall(mouse_settings, keybinds)`）：同一行、两列。
    //   Mouse Settings 那一屏本作没有，所以它是置灰按钮——少了它这一格就空着。
    CHECK(rect(1).y == rect(0).y);
    CHECK(rect(1).x > rect(0).x);
    CHECK(!page[0].enabled);
    CHECK(page[1].enabled);
    // ★ 第二组从**新行**起，即使第一组的行还满着也一样（addSmall 每次调用换行）
    CHECK(rect(2).y > rect(1).y);
    CHECK(rect(2).x == rect(0).x);
    // 组内两两配对
    CHECK(rect(3).y == rect(2).y);
    CHECK(rect(3).x > rect(2).x);
    CHECK(rect(4).y > rect(2).y);
    // 七项落单的那一个独占最后一行的左列
    CHECK(rect(8).x == rect(0).x);
    // Done 在页脚：比所有列表项都低，且宽 200 居中
    const auto done = rect(count - 1U);
    CHECK(done.y > rect(8).y);
    CHECK(done.width == 200.0F * 3.0F);
}

// --- 7. 每一屏的标题键 -------------------------------------------------------
//
// ★ 两组键只差一个后缀：`controls.keybinds.title`（"按键绑定"）是**标题**，
//   `controls.keybinds`（"按键绑定…"）是跳过来的**按钮**。带省略号的标题在画面上
//   看起来"也差不多对"，所以只能靠断言。
void testPageTitles() {
    using mc::ui::PageId;
    CHECK(mc::ui::pageTitle(PageId::KeyBinds).key == "controls.keybinds.title");
    CHECK(mc::ui::pageTitle(PageId::KeyBinds).key != "controls.keybinds");
    CHECK(mc::ui::pageTitle(PageId::Accessibility).key == "options.accessibility.title");
    CHECK(mc::ui::pageTitle(PageId::Accessibility).key != "options.accessibility");
    CHECK(mc::ui::pageTitle(PageId::Controls).key == "controls.title");
    CHECK(mc::ui::pageTitle(PageId::Options).key == "options.title");
    CHECK(mc::ui::pageTitle(PageId::Death).key == "deathScreen.title");
    // 主菜单画的是 logo 贴图，不是一行标题
    CHECK(mc::ui::pageTitle(PageId::Title).empty());
    CHECK(mc::ui::pageTitle(PageId::Game).empty());
    // 每个有标题的屏都必须有兜底文本：翻译缺失时不能是空白
    for (std::size_t raw = 0; raw <= static_cast<std::size_t>(PageId::Accessibility); ++raw) {
        const auto entry = mc::ui::pageTitle(static_cast<PageId>(raw));
        if (!entry.empty()) {
            check(!entry.fallback.empty(), "a titled page needs a fallback", __LINE__);
        }
    }
}

// --- 8. 绑定列表的行表：分类标题夹在中间 --------------------------------------
void testKeyBindListRows() {
    // 24 个动作 + 6 个用到的分类
    CHECK(mc::ui::kKeyBindListRowCount == mc::input::keyBindRows().size() +
                                              mc::ui::keyBindUsedCategoryCount());
    CHECK(mc::ui::keyBindUsedCategoryCount() == 6U);
    // 第一行是一条标题
    CHECK(mc::ui::keyBindListRow(0U).isCategory);
    CHECK(!mc::ui::keyBindListRow(1U).isCategory);
    // ★ 每个分类恰好一条标题：`keyBindRows()` 没按分类分组排序的话，同一个分类会被
    //   切成几段、每段前面都顶一条标题——画面上是"分类标题重复出现"。
    std::size_t headers = 0;
    mc::input::InputCategory seen[16]{};
    std::size_t seenCount = 0;
    for (std::size_t row = 0; row < mc::ui::kKeyBindListRowCount; ++row) {
        const auto entry = mc::ui::keyBindListRow(row);
        if (!entry.isCategory) {
            continue;
        }
        ++headers;
        for (std::size_t i = 0; i < seenCount; ++i) {
            check(seen[i] != entry.category, "a category heading appears twice", __LINE__);
        }
        seen[seenCount++] = entry.category;
    }
    CHECK(headers == mc::ui::keyBindUsedCategoryCount());

    // 标题行不产生控件，所以控件序号折回的行号要跳过它们
    CHECK(mc::ui::keyBindWidgetVisibleRow(0U, 0U, 3U) == 1U);   // 行 0 是标题
    CHECK(mc::ui::keyBindWidgetVisibleRow(0U, 3U, 3U) == 2U);
    CHECK(mc::ui::keyBindWidgetVisibleRow(1U, 0U, 3U) == 0U);   // 从行 1 起就没有标题在前
    // 一屏里的绑定行数少于行数——差额就是这一屏跨了几个分类
    CHECK(mc::ui::keyBindBindingRowsIn(0U, 8U) < 8U);
    CHECK(mc::ui::keyBindBindingRowsIn(0U, mc::ui::kKeyBindListRowCount) ==
          mc::input::keyBindRows().size());
}

// --- 9. 源码护栏：绑定列表页的 Escape 是**解绑** ------------------------------
//
// 26.1 在等待按键时按 Esc 是解除绑定（`KeyBindsScreen.java:71-86`：`event.isEscape()`
// → `setKey(InputConstants.UNKNOWN)`），不是"取消这次改键"、也不是退出这一屏。
// 那是 vanilla **唯一**的解绑入口——写成 cancelCapture()，`InputDevice::None` 这个
// 表示就永远没有人能产生。
//
// 这条判断住在渲染器的翻译单元里，没有测试看得见；它也不改变任何函数的返回值
// （护栏 15）。所以读源码守它，同 title_background / ui_capture 的做法。
void testEscapeUnbindsSourceGuard() {
    std::ifstream input{MC_REBEDROCK_RENDERER_SRC, std::ios::binary};
    if (!input) {
        std::printf("options_layout_test: cannot open %s\n", MC_REBEDROCK_RENDERER_SRC);
        ++failures;
        return;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    // 去掉行注释：下面那段说明文字里正好提到了要禁止的名字。
    std::string source;
    {
        std::istringstream lines{buffer.str()};
        std::string line;
        while (std::getline(lines, line)) {
            const auto comment = line.find("//");
            source += comment == std::string::npos ? line : line.substr(0, comment);
            source += '\n';
        }
    }
    const auto branch = source.find("case ui::PageId::KeyBinds:");
    CHECK(branch != std::string::npos);
    if (branch == std::string::npos) {
        return;
    }
    const auto end = source.find("break;", branch);
    CHECK(end != std::string::npos);
    const std::string body = source.substr(branch, end - branch);
    // ★ 正在捕获时必须走 applyUnbound()
    CHECK(body.find("applyUnbound") != std::string::npos);
    CHECK(body.find("capturing()") != std::string::npos);
    // ★ 而不是 cancelCapture()——那是 UI-6c 之前的写法，等于没有解绑入口
    CHECK(body.find("cancelCapture") == std::string::npos);

    // ★ 绑定列表的滚动上界必须按**行**数算（`kKeyBindListRowCount`，含分类标题行），
    //   不是动作数。用动作数，最后几行（正好是标题行数那么多）永远滚不进来，
    //   而画面上只表现为"到底了但还差几行"。
    const auto scroll = source.find("void scrollControlsList(");
    CHECK(scroll != std::string::npos);
    if (scroll != std::string::npos) {
        const std::string body2 = source.substr(scroll, 600U);
        CHECK(body2.find("kKeyBindListRowCount") != std::string::npos);
        CHECK(body2.find("keyBindRows().size()") == std::string::npos);
    }
}

// 同一条规矩也管滚动条本身：滑块长度与位置的分母是行数。
void testScrollbarUsesRowCountSourceGuard() {
    std::ifstream input{MC_REBEDROCK_HUD_RENDERER_SRC, std::ios::binary};
    if (!input) {
        std::printf("options_layout_test: cannot open %s\n", MC_REBEDROCK_HUD_RENDERER_SRC);
        ++failures;
        return;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::string source;
    {
        std::istringstream lines{buffer.str()};
        std::string line;
        while (std::getline(lines, line)) {
            const auto comment = line.find("//");
            source += comment == std::string::npos ? line : line.substr(0, comment);
            source += '\n';
        }
    }
    const auto fn = source.find("void drawKeyBindsScrollbar(");
    CHECK(fn != std::string::npos);
    if (fn == std::string::npos) {
        return;
    }
    const std::string body = source.substr(fn, 700U);
    // 分母是行数（含标题行），不是动作数——否则滑块偏长、滚到底还剩几行。
    CHECK(body.find("kKeyBindListRowCount") != std::string::npos);
    CHECK(body.find("keyBindRows().size()") == std::string::npos);
}

// --- 11. addBig：独占整行的那一项 --------------------------------------------
//
// ★ 从前的分组表只是一串**项数**，"这一组是 addBig 还是 addSmall"根本没地方存。
//   视频设置的 Preset 因此被摆成双列里 150 宽的左列一格——画面上是"第一行右边空着"，
//   而 26.1 那是一个铺满行宽（310）的大按钮。项数对、行数也对，只有宽度错，
//   所以除了对 `big` 断言之外没有任何东西会红。
void testBigGroups() {
    const auto video = mc::ui::optionsGroupsOf(mc::ui::PageId::VideoSettings);
    // 第 0 项是 Preset：独占行 0、左列、big
    const auto preset = mc::ui::optionsGroupedSlot(video, 0);
    CHECK(preset == (mc::ui::OptionsSlot{0U, 0, true}));
    // 第 1 项（Render Distance）从**新行**起，而且是小格
    CHECK(mc::ui::optionsGroupedSlot(video, 1) == (mc::ui::OptionsSlot{1U, 0, false}));
    CHECK(mc::ui::optionsGroupedSlot(video, 2) == (mc::ui::OptionsSlot{1U, 1, false}));
    // 1 + ceil(11/2) + ceil(4/2) = 1 + 6 + 2 = 9 行
    CHECK(mc::ui::optionsRowCountOf(mc::ui::PageId::VideoSettings) == 9U);
    CHECK(mc::ui::optionsCountOf(video) == 16U);

    // 一个**两项的 big 组**占两行，不是一行——big 与 small 的行数算法不同，
    // 照 small 的 (n+1)/2 算会让后面每一组都上移。
    constexpr std::array<mc::ui::OptionsGroup, 2> kBigThenSmall{{{2U, mc::ui::OptionsGroupKind::Big}, {2U, mc::ui::OptionsGroupKind::Small}}};
    CHECK(mc::ui::optionsGroupedSlot(kBigThenSmall, 0) == (mc::ui::OptionsSlot{0U, 0, true}));
    CHECK(mc::ui::optionsGroupedSlot(kBigThenSmall, 1) == (mc::ui::OptionsSlot{1U, 0, true}));
    CHECK(mc::ui::optionsGroupedSlot(kBigThenSmall, 2) == (mc::ui::OptionsSlot{2U, 0, false}));
    CHECK(mc::ui::optionsGroupedRowCount(kBigThenSmall) == 3U);

    // 几何上 big 真的比 small 宽：310 对 150。
    const mc::ui::ScrollList list{0, 0, 427, 174, mc::ui::kOptionsRowWidth,
                                  mc::ui::kOptionsRowHeight};
    CHECK(mc::ui::optionsBigCell(list, 0).width == static_cast<float>(mc::ui::kOptionsBigWidth));
    CHECK(mc::ui::optionsSmallCell(list, 0, 0).width ==
          static_cast<float>(mc::ui::kOptionsSmallWidth));
    CHECK(mc::ui::optionsBigCell(list, 0).width > mc::ui::optionsSmallCell(list, 0, 0).width);
}

// --- 12. 滚动窗口：装配序号 ≠ 设置项序号 --------------------------------------
//
// ★ 这条是 UI-6d 的核心不变量。装配只造窗口里的控件，所以 `Page` 里第 i 个控件不是
//   第 i 个设置项——被滚上去的那些**不占序号**。布局若把 i 直接喂给
//   `optionsGroupedSlot`，滚到第 k 行时每个控件都会画在它上面 k 行的位置：
//   画面上像"滚动条动了、内容没动"，而两侧各自都"自洽"，没有任何断言会红。
void testScrolledSlots() {
    const auto video = mc::ui::optionsGroupsOf(mc::ui::PageId::VideoSettings);
    // firstRow = 0 时它必须与不滚的那个函数逐项相同（否则不滚的页面也会错位）
    for (std::size_t i = 0; i < mc::ui::optionsCountOf(video); ++i) {
        check(mc::ui::optionsScrolledSlot(video, 0U, i) == mc::ui::optionsGroupedSlot(video, i),
              "firstRow = 0 must reduce to the unscrolled mapping", __LINE__);
    }
    // 滚到第 3 行：行 0..2 的那些项（Preset + 行1 两项 + 行2 两项 = 5 项）不再装配，
    // 于是装配序号 0 对应的是**第 5 个设置项**，画在可见行 0。
    const auto first = mc::ui::optionsScrolledSlot(video, 3U, 0U);
    CHECK(first.row == 0U);
    CHECK(first.column == 0);
    CHECK(mc::ui::optionsGroupedSlot(video, 5U).row == 3U);
    // 装配序号 1 是同一可见行的右列
    CHECK(mc::ui::optionsScrolledSlot(video, 3U, 1U) == (mc::ui::OptionsSlot{0U, 1, false}));
    // 再往后一格换行
    CHECK(mc::ui::optionsScrolledSlot(video, 3U, 2U).row == 1U);
    // ★ 滚动之后**每一个已装配的控件**都落在第 0 行之后、列表之内——把 firstRow
    //   多减一次会让首行跑到列表之前（无符号下折成天文数字），少减一次会让末行跑出去。
    //   注意上界是**已装配数**，不是设置项总数：越过它得到的是"没有这个控件"的哨兵值，
    //   对哨兵断言只会把测试写成噪音。
    const std::size_t totalRows = mc::ui::optionsRowCountOf(mc::ui::PageId::VideoSettings);
    for (std::size_t k = 0; k <= 3U; ++k) {
        std::size_t assembled = 0;
        for (std::size_t i = 0; i < mc::ui::optionsCountOf(video); ++i) {
            if (mc::ui::optionsGroupedSlot(video, i).row >= k) {
                ++assembled;
            }
        }
        for (std::size_t i = 0; i < assembled; ++i) {
            const auto slot = mc::ui::optionsScrolledSlot(video, k, i);
            check(slot.row < totalRows - k,
                  "a scrolled slot must stay inside the list", __LINE__);
        }
    }
}

// --- 13. 视频设置的真实排版：滚动窗口下不压页脚 -------------------------------
//
// 走**生产路径**（装配 → 布局），量的是 UI-6d 的起因本身：17 个控件占 9 行，而
// 1280x720 @ scale 3 的内容区只放得下 6 行——不滚的话最后两行会压在 Done 上。
void testVideoSettingsWindowedLayout() {
    const mc::ui::HudLayout layout{1280.0F, 720.0F, 3};
    const auto page = mc::ui::PageId::VideoSettings;
    const auto window = mc::ui::optionsWindowFor(layout, page, 0U);
    // 内容区 174 逻辑像素 / 每行 25 = 6 行；总共 9 行，所以确实滚得动。
    CHECK(window.rowCount == 6U);
    CHECK(mc::ui::optionsMaximumFirstRow(layout, page) == 3U);

    const auto build = [&](std::size_t firstRow) {
        mc::ui::MenuBuildContext ctx;
        ctx.optionsWindow = mc::ui::optionsWindowFor(layout, page, firstRow);
        const mc::ui::MenuCallbacks cb;
        mc::ui::Page built;
        mc::ui::buildPageInto(built, page, ctx, cb);
        mc::ui::layoutPageInto(built, page, layout, 1280.0F, 0U, ctx.optionsWindow.firstRow);
        return built;
    };

    const auto frame = mc::ui::headerAndFooterLayout(layout.logicalWidth(), layout.logicalHeight());
    const float contentBottom =
        static_cast<float>(frame.contentBox().y + frame.contentBox().height) * 3.0F;
    const float footerTop = static_cast<float>(frame.footerButton().y) * 3.0F;

    for (std::size_t firstRow = 0; firstRow <= 3U; ++firstRow) {
        const auto built = build(firstRow);
        const std::size_t count = mc::ui::countPageButtons(built);
        // 窗口 6 行 × 2 列最多 12 个设置项，加 Done。不滚时是 11 个设置项（含 big 那一行）。
        check(count <= 13U, "the window may not assemble more than it can lay out", __LINE__);
        check(built.size() == count, "the video screen has no list-row widgets", __LINE__);
        for (std::size_t i = 0; i + 1U < count; ++i) {
            const auto rect = built[i].rect;
            // ★ 起因本身：**没有任何**设置项越过内容区下缘、压到页脚的 Done 上。
            check(rect.y + rect.height <= contentBottom + 0.5F,
                  "a settings row escaped the content area", __LINE__);
            check(rect.y + rect.height <= footerTop,
                  "a settings row overlapped the footer button", __LINE__);
            check(rect.y >= static_cast<float>(frame.contentBox().y) * 3.0F - 0.5F,
                  "a settings row floated above the content area", __LINE__);
        }
        // Done 始终在页脚，不受滚动影响
        const auto done = built[count - 1U].rect;
        check(done.width == 200.0F * 3.0F, "Done stays a 200-wide footer button", __LINE__);
        check(done.y >= footerTop - 0.5F, "Done stays in the footer", __LINE__);
    }

    // ★ 具体控件落在具体格位上。这一条挡的是"布局侧拿装配序号当设置项序号"：
    //   滚到第 3 行时 Advanced Graphics… 是第 11 个设置项、第 6 个**已装配**控件，
    //   正确落点是可见行 3 的**左**列；照装配序号查表会查到第 6 个设置项，
    //   那是可见行 3 的**右**列。行号相同、只有列不同——所以只量"没压到页脚"看不出来。
    {
        const auto built = build(3U);
        const auto list = mc::ui::optionsScrollList(frame.contentBox());
        const auto expect = [&](mc::ui::WidgetId id, std::size_t visibleRow, int column) {
            for (const auto& widget : built) {
                if (widget.debugId != static_cast<std::uint16_t>(id)) {
                    continue;
                }
                const auto cell = mc::ui::optionsSmallCell(list, visibleRow, column);
                check(widget.rect.x == cell.x * 3.0F, "widget is in the wrong column", __LINE__);
                check(widget.rect.y == cell.y * 3.0F, "widget is in the wrong row", __LINE__);
                return;
            }
            check(false, "the expected widget was not assembled", __LINE__);
        };
        expect(mc::ui::WidgetId::AdvancedGraphics, 3U, 0);
        expect(mc::ui::WidgetId::FrameRateLimit, 4U, 0);
        expect(mc::ui::WidgetId::Vsync, 4U, 1);
        expect(mc::ui::WidgetId::Resolution, 5U, 1);
    }

    // ★ 走**生产路径**量 Preset 的宽度。testBigGroups 只证了 optionsBigCell 比
    //   optionsSmallCell 宽——那是"写了纯函数"，不是"布局真的用了它"。把布局侧改成
    //   永远走 small 格，上面那些断言一条都不会红：行号列号全对，只有宽度错。
    {
        const auto built = build(0U);
        bool sawPreset = false;
        for (const auto& widget : built) {
            if (widget.debugId != static_cast<std::uint16_t>(mc::ui::WidgetId::GraphicsPreset)) {
                continue;
            }
            sawPreset = true;
            check(widget.rect.width == static_cast<float>(mc::ui::kOptionsBigWidth) * 3.0F,
                  "the addBig row must span the full row width", __LINE__);
        }
        check(sawPreset, "the video screen starts with the big preset row", __LINE__);
        // 同一屏上的小格仍是 150：宽度不是被整体改大了。
        for (const auto& widget : built) {
            if (widget.debugId == static_cast<std::uint16_t>(mc::ui::WidgetId::ViewDistance)) {
                check(widget.rect.width == static_cast<float>(mc::ui::kOptionsSmallWidth) * 3.0F,
                      "an addSmall cell stays 150 wide", __LINE__);
            }
        }
    }

    // 滚过头必须被钳住：窗口起点不会越过最后一屏，否则整张列表滚成空白。
    CHECK(mc::ui::optionsWindowFor(layout, page, 99U).firstRow == 3U);
    CHECK(mc::ui::optionsWindowFor(layout, page, 3U).firstRow == 3U);

    // 滚到底那一屏必须**露出最后一项**（Resolution）——窗口算错一行的典型症状是
    // 滚到底了还差一行进不来。
    const auto bottom = build(3U);
    bool sawResolution = false;
    for (const auto& widget : bottom) {
        if (widget.debugId == static_cast<std::uint16_t>(mc::ui::WidgetId::Resolution)) {
            sawResolution = true;
        }
    }
    CHECK(sawResolution);
    // 反过来，滚到底之后 Preset 不该还在（它在行 0）
    bool sawPreset = false;
    for (const auto& widget : bottom) {
        if (widget.debugId == static_cast<std::uint16_t>(mc::ui::WidgetId::GraphicsPreset)) {
            sawPreset = true;
        }
    }
    CHECK(!sawPreset);
}

// --- 13b. 哪些三段式页面真的滚得动 --------------------------------------------
//
// ★ 实测：GUI 缩放会随画布变小而**降档**（26.1 `Window.calculateScale`），所以逻辑画布
//   永远不会小于 320x240，内容区因此永远至少 174 逻辑像素 = 6 行。Controls 只有 5 行、
//   高级图形只有 1 行——它们在**任何合法画布上都装得下**，窗口对它们恒等。
//
//   这条测试记的就是这个事实。它有两个用处：一是说明为什么这两页的 OptionCursor
//   只能靠源码守（把它去掉在今天的任何画布上都看不出变化，没有夹具抓得住）；
//   二是等哪天 Controls 长到 7 行，这里会先红——那时窗口对它就不再是恒等了。
void testWhichPagesActuallyScroll() {
    // 最小的合法逻辑画布（ui/UiCapture 的 320x240 下界）。
    const mc::ui::HudLayout smallest{320.0F, 240.0F, 1};
    const auto frame =
        mc::ui::headerAndFooterLayout(smallest.logicalWidth(), smallest.logicalHeight());
    const std::size_t rows = mc::ui::optionsScrollList(frame.contentBox()).visibleRows();
    CHECK(rows == 6U);
    // 装得下的两页：最小画布上也不滚。
    CHECK(mc::ui::optionsRowCountOf(mc::ui::PageId::Controls) <= rows);
    CHECK(mc::ui::optionsRowCountOf(mc::ui::PageId::AdvancedGraphics) <= rows);
    CHECK(mc::ui::optionsMaximumFirstRow(smallest, mc::ui::PageId::Controls) == 0U);
    CHECK(mc::ui::optionsMaximumFirstRow(smallest, mc::ui::PageId::AdvancedGraphics) == 0U);
    // 装不下的那一页：UI-6d 的起因。9 行 > 6 行，在**最大**的画布上也一样。
    CHECK(mc::ui::optionsRowCountOf(mc::ui::PageId::VideoSettings) > rows);
    CHECK(mc::ui::optionsMaximumFirstRow(smallest, mc::ui::PageId::VideoSettings) > 0U);
    // 非三段式的页面没有窗口：rowCount == 0 就是"不滚，全装配"。
    CHECK(mc::ui::optionsWindowFor(smallest, mc::ui::PageId::Options, 5U).rowCount == 0U);
    CHECK(mc::ui::optionsWindowFor(smallest, mc::ui::PageId::Title, 5U).rowCount == 0U);
}

// --- 14. 装配侧与布局侧读的是同一个窗口 ---------------------------------------
//
// ★ 源码护栏。装配（PageBuilder 的 optionVisible / OptionCursor）与布局
//   （MenuGeometry 的 optionsScrolledSlot）对"第 i 个控件是谁"必须有同一个说法。
//   两处各自都编译得过、各自都自洽，错开的表现是整屏错行——这与 UI-6b 那次
//   "点 Controls 底部按钮就闪退"是同一族缺陷（两处各自解释同一个下标）。
void testWindowSingleSourceGuard() {
    const auto read = [](const char* path) {
        std::ifstream file{path};
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    };
    // 三个三段式页面都必须用 OptionCursor 装配。只给"装不下"的那一页加窗口是不够的：
    // optionsWindowFor 给所有 HeaderFooterList 页面同一种窗口，哪一页装配了窗口外的项，
    // 那一页就整体错行。
    const std::string builder = read(MC_REBEDROCK_PAGE_BUILDER_SRC);
    std::size_t cursors = 0;
    for (std::size_t at = builder.find("OptionCursor add{ctx, id};");
         at != std::string::npos; at = builder.find("OptionCursor add{ctx, id};", at + 1U)) {
        ++cursors;
    }
    CHECK(cursors == 3U);   // VideoSettings / AdvancedGraphics / Controls

    // ★ 每一页**声明的设置项数**必须等于它实际装配的项数。
    //
    // 这两个数字长在两个文件里：`optionsGroupsOf()` 的那张表说「这一页有几项、怎么分组」，
    // PageBuilder 的那个 case 说「实际加哪几个」。加一个设置项而忘了改表，两边都编译得过、
    // 各自都自洽，症状是**多出来的那个控件叠在别人身上**——`optionsGroupedSlot` 走完所有
    // 组还没找到这个下标，就返回「最后一行之后」，而它下一个也返回同一个位置。
    // 屏幕上是两个按钮画在一处，测试里从前什么都不响。
    const auto caseBody = [&builder](const char* label) {
        const auto start = builder.find(label);
        CHECK(start != std::string::npos);
        if (start == std::string::npos) {
            return std::string{};
        }
        const auto end = builder.find("case PageId::", start + std::strlen(label));
        return builder.substr(start, end == std::string::npos ? std::string::npos : end - start);
    };
    const auto assembledCount = [](const std::string& body) {
        std::size_t count = 0;
        for (std::size_t at = body.find("add([&] {"); at != std::string::npos;
             at = body.find("add([&] {", at + 1U)) {
            ++count;
        }
        return count;
    };
    const auto declaredCount = [](mc::ui::PageId page) {
        std::size_t count = 0;
        for (const mc::ui::OptionsGroup& group : mc::ui::optionsGroupsOf(page)) {
            count += group.count;
        }
        return count;
    };
    struct PageCase final {
        const char* label;
        mc::ui::PageId page;
    };
    for (const PageCase entry : {PageCase{"case PageId::VideoSettings: {",
                                          mc::ui::PageId::VideoSettings},
                                 PageCase{"case PageId::AdvancedGraphics: {",
                                          mc::ui::PageId::AdvancedGraphics}}) {
        const std::size_t assembled = assembledCount(caseBody(entry.label));
        const std::size_t declared = declaredCount(entry.page);
        CHECK(assembled == declared);
        if (assembled != declared) {
            std::cerr << "  " << entry.label << " 装配 " << assembled << " 项，"
                      << "optionsGroupsOf 声明 " << declared << " 项\n";
        }
    }

    // 布局侧必须走 optionsScrolledSlot，不能直接用 optionsGroupedSlot：后者拿装配序号
    // 当设置项序号，滚动一开就整屏错行。
    const std::string geometry = read(MC_REBEDROCK_MENU_GEOMETRY_SRC);
    const auto layoutFn = geometry.find("UiRect frontendButtonRect(");
    CHECK(layoutFn != std::string::npos);
    if (layoutFn == std::string::npos) {
        return;
    }
    const std::string body = geometry.substr(layoutFn);
    CHECK(body.find("optionsScrolledSlot(") != std::string::npos);
    CHECK(body.find("optionsGroupedSlot(") == std::string::npos);

    // 两个渲染侧都必须从 optionsWindowFor 取窗口，而不是各自现算一个。
    //
    // ★ 光有 optionsWindowFor 还不够：窗口要**同时**喂给装配（ctx.optionsWindow）和
    //   布局（layoutPageInto 的 optionsFirstRow）。只喂装配、布局那个参数传 0，
    //   是最容易犯的一种——两处都编译得过，滚动之后整屏错行，而这两个翻译单元
    //   没有任何测试链接得到。所以逐个数：optionsWindowFor 出现了，
    //   `optionsWindow.firstRow` 也必须出现在同一个文件里。
    for (const char* path : {MC_REBEDROCK_HUD_RENDERER_SRC, MC_REBEDROCK_RENDERER_SRC}) {
        const std::string source = read(path);
        check(source.find("ui::optionsWindowFor(") != std::string::npos,
              "both build sites must read the window from ui::optionsWindowFor", __LINE__);
        check(source.find("optionsWindow.firstRow") != std::string::npos,
              "the window's firstRow must reach layoutPageInto too", __LINE__);
        // 传字面量 0 当滚动位置就是把窗口丢了。
        check(source.find("keyFirst, 0U)") == std::string::npos,
              "layoutPageInto must not be given a literal scroll row", __LINE__);
    }
}

// --- 15. 滑块：光标 → 比例 → 值 --------------------------------------------
//
// ★ 这一条对着一个现场缺陷：「一调节模糊强度就跳到 0，且无法再次调整」。
//   根因是 `SliderBind::onDrag(float)` 有**两种约定**——三个既有滑块忽略参数、
//   自己去读光标，而 UI-6d 表驱动的整数滑块把参数当权威。按下时那句
//   `onDrag(0.0F)`（注释还写着 "the appliers read the cursor themselves"）
//   于是一把把值打成 0；拖拽循环里又没有它的分支，所以再也拖不回来。
//   收口成一条约定：fraction 是权威的，由 ui::sliderFractionFromCursor 从控件
//   自己的矩形算出来。
void testSliderCursorMapping() {
    // 一个 150 宽的小格，scale 3 → 帧缓冲里 450 宽，把手 8*3 = 24。
    const mc::ui::UiRect rect{300.0F, 100.0F, 450.0F, 60.0F};
    constexpr float kScale = 3.0F;
    const auto fraction = [&](float cursorX) {
        return mc::ui::sliderFractionFromCursor(rect, cursorX, kScale);
    };
    // 抓的是把手**中心**（26.1 `AbstractSliderButton`：`(mouseX - (getX()+4)) / (width-8)`）
    const float handle = 8.0F * kScale;
    CHECK(fraction(rect.x + handle * 0.5F) == 0.0F);
    CHECK(fraction(rect.x + rect.width - handle * 0.5F) == 1.0F);
    // 两端之外要夹住，不能给出负数或大于 1（那会让取值绕回另一端）
    CHECK(fraction(rect.x - 1000.0F) == 0.0F);
    CHECK(fraction(rect.x + rect.width + 1000.0F) == 1.0F);
    // 中点是 0.5（容一点浮点误差）
    const float middle = fraction(rect.x + rect.width * 0.5F);
    check(middle > 0.49F && middle < 0.51F, "the midpoint must be half", __LINE__);

    // 与绘制侧互为逆：把手左缘 → 比例 → 同一个把手左缘。
    for (float f : {0.0F, 0.25F, 0.5F, 1.0F}) {
        const float x = mc::ui::sliderHandleX(rect, f, kScale);
        const float back = mc::ui::sliderFractionFromCursor(rect, x + handle * 0.5F, kScale);
        check(back > f - 0.01F && back < f + 0.01F,
              "handle position and cursor fraction must be inverses", __LINE__);
    }

    // ★ 缺陷本身：光标停在滑块中间时，模糊强度必须是 5，不是 0。
    const auto* blur = mc::ui::findIntSlider(mc::ui::WidgetId::MenuBackgroundBlurriness);
    CHECK(blur != nullptr);
    if (blur == nullptr) {
        return;
    }
    CHECK(mc::ui::intSliderValue(*blur, middle) == 5);
    CHECK(mc::ui::intSliderValue(*blur, 1.0F) == 10);
    CHECK(mc::ui::intSliderValue(*blur, 0.0F) == 0);
    // 按下时若传 0.0F（那个缺陷的形状），值就是 0——这正是"一按就跳到 0"。
    // 断言它确实会归 0，是为了让下面那条源码守的理由留在测试里，而不是只在注释里。
    CHECK(mc::ui::intSliderValue(*blur, 0.0F) != 5);
    // 往返：值 → 比例 → 值 必须稳定，否则拖一下就漂一格
    for (int value = blur->minimum; value <= blur->maximum; ++value) {
        const float f = mc::ui::intSliderFraction(*blur, value);
        check(mc::ui::intSliderValue(*blur, f) == value,
              "value -> fraction -> value must round-trip", __LINE__);
    }

    // ★ 四舍五入不是截断。`OptionSlider.hpp` 的注释写明了这条理由，但第一轮
    //   sabotage（把 `+ 0.5F` 去掉）**没被抓住**——上面那个往返循环测不出来：
    //   0..10 这个跨度小，float 精度会把 v/10*10 吸回整数，截断与四舍五入同解。
    //   真正区分两者的是**档位边界**：截断会让每一档的吸附点整体偏左半格。
    CHECK(mc::ui::intSliderValue(*blur, 0.55F) == 6);   // 截断给 5
    CHECK(mc::ui::intSliderValue(*blur, 0.96F) == 10);  // 截断给 9：滑块贴到头却不是最大档
    CHECK(mc::ui::intSliderValue(*blur, 0.44F) == 4);   // 这一档两种算法同解，防"无脑进位"
    CHECK(mc::ui::intSliderValue(*blur, 0.04F) == 0);
}

// --- 16. 滑块拖拽只有一条路径 ------------------------------------------------
//
// ★ 源码守：拖拽逻辑住在渲染器的翻译单元里，没有测试链接得到它。
void testSliderDragSingleSourceGuard() {
    std::ifstream file{MC_REBEDROCK_RENDERER_SRC};
    std::stringstream buffer;
    buffer << file.rdbuf();
    // ★ 必须先剥注释：这一轮的落地注释里就写着 `onDrag(0.0F)`（在说"从前是这样"），
    //   不剥的话守到的是注释、不是代码——第一次跑就是这么红的。
    std::string source;
    {
        std::istringstream lines{buffer.str()};
        std::string line;
        while (std::getline(lines, line)) {
            const auto comment = line.find("//");
            source += comment == std::string::npos ? line : line.substr(0, comment);
            source += '\n';
        }
    }

    // 按下时不得再传字面量 0——那是"一按就跳到最小档"的直接原因。
    CHECK(source.find("onDrag(0.0F)") == std::string::npos);
    // 三个 per-slider 的拖拽 bool 已经收成一个 draggingSlider。留着它们
    // 就意味着"现在在拖谁"仍有多份表述，加一个滑块又要逐处补。
    CHECK(source.find("viewDistanceSliderDragging") == std::string::npos);
    CHECK(source.find("masterVolumeSliderDragging") == std::string::npos);
    CHECK(source.find("draggingSlider") != std::string::npos);
    // 应用器不得再自己去读光标（那条路上带着硬编码的控件序号）。
    CHECK(source.find("updateViewDistanceFromCursor") == std::string::npos);
    CHECK(source.find("updateMasterVolumeFromCursor") == std::string::npos);
    // 换算只有一处
    CHECK(source.find("ui::sliderFractionFromCursor(") != std::string::npos);

    // ★ 松开必须清掉"在拖谁"。不清的症状是**松手之后滑块还跟着鼠标走**——
    //   拖拽循环只看 draggingSlider != None，不看按键状态。从前那三个 bool 各清一次，
    //   收成一个 id 之后只剩一处，但"只剩一处"不等于"那一处还在"：
    //   第一轮 sabotage（删掉这一行）没被任何断言抓住。
    const auto releaseFn = source.find("void handleMenuButtonRelease(");
    CHECK(releaseFn != std::string::npos);
    if (releaseFn == std::string::npos) {
        return;
    }
    const std::string releaseBody = source.substr(releaseFn, 1600U);
    CHECK(releaseBody.find("draggingSlider = ui::WidgetId::None") != std::string::npos);

    // 绘制侧的把手位置必须走同一份几何，否则上面那条"互为逆"只是两个纯函数
    // 自己跟自己对得上，而画面上把手停的位置和松手的位置差半格。
    std::ifstream hud{MC_REBEDROCK_HUD_RENDERER_SRC};
    std::stringstream hudBuffer;
    hudBuffer << hud.rdbuf();
    std::string hudSource;
    {
        std::istringstream lines{hudBuffer.str()};
        std::string line;
        while (std::getline(lines, line)) {
            const auto comment = line.find("//");
            hudSource += comment == std::string::npos ? line : line.substr(0, comment);
            hudSource += '\n';
        }
    }
    // ★ 只截 drawMinecraftSlider 的函数体。整文件搜 `8.0F * scale` 会命中十几处
    //   与滑块无关的用途（文本框内缩、面板边距、toast 高度），那条守会永远红——
    //   第一次写宽了就是这么红的。
    const auto knobFn = hudSource.find("void drawMinecraftSlider(");
    CHECK(knobFn != std::string::npos);
    if (knobFn == std::string::npos) {
        return;
    }
    const std::string knobBody = hudSource.substr(knobFn, 1800U);
    CHECK(knobBody.find("ui::sliderHandleX(") != std::string::npos);
    // 把手宽度取自共享常量，不是这里自己写一个 8
    CHECK(knobBody.find("kSliderHandleWidth") != std::string::npos);
    CHECK(knobBody.find("8.0F * scale") == std::string::npos);
}

// --- 17. float 滑块表（UI-6e ①-a）--------------------------------------------
//
// ★ 两条与整数滑块**方向相反**的规则，都是照抄 vanilla，写成一样的必错一个：
//   1. 百分比是**截断**（`Options.java:1911` 的 `(int)(value * 100.0)`），
//      而整数滑块的档位是**四舍五入**（`intSliderValue` 的 `+ 0.5F`）。
//   2. OFF 的判据是**原始值 == 0**，不是"百分比取整后为 0"。
void testFloatSliders() {
    const auto* master = mc::ui::findFloatSlider(mc::ui::WidgetId::MasterVolume);
    CHECK(master != nullptr);
    if (master == nullptr) {
        return;
    }

    // ★ 截断：0.999 显示 99%，不是 100%。写成四舍五入这一条会红。
    CHECK(mc::ui::floatSliderPercent(0.999F) == 99);
    CHECK(mc::ui::floatSliderPercent(0.5F) == 50);
    CHECK(mc::ui::floatSliderPercent(1.0F) == 100);
    CHECK(mc::ui::floatSliderPercent(0.0F) == 0);
    // 越界要夹住，不能给出负数或 >100
    CHECK(mc::ui::floatSliderPercent(-1.0F) == 0);
    CHECK(mc::ui::floatSliderPercent(2.0F) == 100);

    // ★ OFF 只看原始值是不是 0。0.004 的百分比是 0，但 vanilla 显示 "0%" 不是 OFF。
    CHECK(mc::ui::floatSliderShowsOff(*master, 0.0F));
    CHECK(!mc::ui::floatSliderShowsOff(*master, 0.004F));
    CHECK(mc::ui::floatSliderPercent(0.004F) == 0);   // 百分比确实是 0
    CHECK(!mc::ui::floatSliderShowsOff(*master, 1.0F));

    // 独立字段那条取值路径
    mc::config::GameOptions options;
    mc::ui::setFloatSliderValue(*master, options, 0.25F);
    CHECK(options.masterVolume == 0.25F);
    CHECK(mc::ui::floatSliderValue(*master, options) == 0.25F);
    // 写入要夹到 [0,1]
    mc::ui::setFloatSliderValue(*master, options, 5.0F);
    CHECK(options.masterVolume == 1.0F);
    mc::ui::setFloatSliderValue(*master, options, -5.0F);
    CHECK(options.masterVolume == 0.0F);

    // ★ 数组那条取值路径：十个音量住在 std::array 里，成员指针指不到某一格。
    //   这条断言在"两种取值位置只实现了字段那一种"时会红——而那正是最容易漏的一半，
    //   因为表里今天只有主音量一项走字段路径，光看表看不出另一条路存在。
    const mc::ui::FloatSliderDesc musicVolume{
        mc::ui::WidgetId::None, "soundCategory.music", "Music",
        nullptr, mc::audio::SoundCategory::Music, true};
    mc::ui::setFloatSliderValue(musicVolume, options, 0.4F);
    CHECK(options.soundCategoryVolumes[static_cast<std::size_t>(
              mc::audio::SoundCategory::Music)] == 0.4F);
    CHECK(mc::ui::floatSliderValue(musicVolume, options) == 0.4F);
    // 写数组那一格**不能**顺手动到主音量（两条路径必须互不干扰）
    CHECK(options.masterVolume == 0.0F);

    // 本作的音量类别必须与 26.1 的 SoundSource 一一对应，否则"音乐与声音"那一屏
    // 会比 vanilla 少或多一个滑块。26.1：MASTER/MUSIC/RECORDS/WEATHER/BLOCKS/
    // HOSTILE/NEUTRAL/PLAYERS/AMBIENT/VOICE 共 10 个。
    CHECK(mc::audio::kSoundCategoryCount == 10U);
}

// --- 18. 分节标题行与变高条目（UI-6e ①-c / D15）-------------------------------
//
// 26.1 的设置屏用 `addHeader` 分节，而分节行**占一行、不产生控件、且高度不是 25**：
//   `OptionsList.addHeader`（OptionsList.java:52-56）
//       paddingTop = children().isEmpty() ? 0 : 9*2;
//       addEntry(entry, paddingTop + 9 + 4);
//   → 首个 13，其后 31。
//
// ★ 变高**只做在 OptionsList 这一层，没有动 ScrollList**。语言、按键绑定、世界列表
//   三张列表共用 ScrollList，它们全是等高的；把变高塞进共享的那一层，是拿三屏的
//   回归风险去换一屏的功能。
void testOptionsHeaderRows() {
    using K = mc::ui::OptionsGroupKind;
    // 26.1 视频设置的真实结构（VideoSettingsScreen.addOptions()）
    constexpr std::array<mc::ui::OptionsGroup, 6> kVanillaShape{{
        {0U, K::Header, "options.video.display.header", "Display"},
        {1U, K::Big},
        {7U, K::Small},
        {0U, K::Header, "options.video.quality.header", "Quality"},
        {1U, K::Big},
        {17U, K::Small},
    }};

    // 行数：1 + 1 + 4 + 1 + 1 + 9 = 17
    CHECK(mc::ui::optionsGroupedRowCount(kVanillaShape) == 17U);

    // 首个标题 13，其后 31
    CHECK(mc::ui::optionsRowAt(kVanillaShape, 0).isHeader);
    CHECK(mc::ui::optionsRowAt(kVanillaShape, 0).height == 13);
    CHECK(mc::ui::optionsRowAt(kVanillaShape, 0).headerFallback == "Display");
    const std::size_t secondHeaderRow = 1U + 1U + 4U;   // big + ceil(7/2)
    CHECK(mc::ui::optionsRowAt(kVanillaShape, secondHeaderRow).isHeader);
    CHECK(mc::ui::optionsRowAt(kVanillaShape, secondHeaderRow).height == 31);
    CHECK(mc::ui::optionsRowAt(kVanillaShape, secondHeaderRow).headerFallback == "Quality");
    // 中间那些是普通设置行
    CHECK(!mc::ui::optionsRowAt(kVanillaShape, 1U).isHeader);
    CHECK(mc::ui::optionsRowAt(kVanillaShape, 1U).height == mc::ui::kOptionsRowHeight);

    // ★ 标题行占一行却**不吞控件序号**：第 0 个设置项是 big，落在行 1 不是行 0。
    CHECK(mc::ui::optionsGroupedSlot(kVanillaShape, 0) == (mc::ui::OptionsSlot{1U, 0, true}));
    // 第 1 个设置项是第一组 addSmall 的头一个，落在行 2 左列
    CHECK(mc::ui::optionsGroupedSlot(kVanillaShape, 1) == (mc::ui::OptionsSlot{2U, 0, false}));
    // 第二个标题之后的那个 big：设置项序号 8（1 + 7），行号 secondHeaderRow + 1
    CHECK(mc::ui::optionsGroupedSlot(kVanillaShape, 8) ==
          (mc::ui::OptionsSlot{secondHeaderRow + 1U, 0, true}));

    // 行顶偏移不再是乘法：行 1 在 13 而不是 25
    CHECK(mc::ui::optionsRowTop(kVanillaShape, 0) == 0);
    CHECK(mc::ui::optionsRowTop(kVanillaShape, 1) == 13);
    CHECK(mc::ui::optionsRowTop(kVanillaShape, 2) == 13 + 25);
    // 第二个标题之前累计：13 + 5 行 * 25
    CHECK(mc::ui::optionsRowTop(kVanillaShape, secondHeaderRow) == 13 + 5 * 25);

    // ★★ 回归护栏：**没有标题行时，一切必须与等高时逐字节相同**。
    //    这一条挡的是"为了做变高，把等高情形也算歪了"——那会让 Controls、
    //    高级图形、以及今天的视频设置三屏同时错位，而它们本来是好的。
    for (const auto page : {mc::ui::PageId::Controls, mc::ui::PageId::VideoSettings,
                            mc::ui::PageId::AdvancedGraphics}) {
        const auto groups = mc::ui::optionsGroupsOf(page);
        const std::size_t rows = mc::ui::optionsRowCountOf(page);
        for (std::size_t row = 0; row <= rows; ++row) {
            check(mc::ui::optionsRowTop(groups, row) ==
                      static_cast<int>(row) * mc::ui::kOptionsRowHeight,
                  "an all-equal-height page must still lay out as row * 25", __LINE__);
        }
        // 可见行数也必须与 ScrollList 的等高算法一致
        for (int viewport : {50, 174, 294, 300}) {
            const std::size_t byPixels =
                mc::ui::optionsVisibleRows(groups, 0U, viewport, rows);
            const std::size_t byDivision =
                std::min(static_cast<std::size_t>(viewport / mc::ui::kOptionsRowHeight), rows);
            check(byPixels == byDivision,
                  "equal-height visible-row count must match plain division", __LINE__);
        }
    }

    // 变高时"一屏装几行"取决于从哪一行开始看——这正是不能用除法的原因。
    const std::size_t fromTop = mc::ui::optionsVisibleRows(kVanillaShape, 0U, 100, 17U);
    const std::size_t fromSecond = mc::ui::optionsVisibleRows(kVanillaShape, 1U, 100, 17U);
    // 从第 0 行起：13 + 25*3 = 88，再加一行 25 就超了 → 4 行
    CHECK(fromTop == 4U);
    // 从第 1 行起：25*4 = 100 正好 → 4 行（同样是 4，但走的是不同的累加）
    CHECK(fromSecond == 4U);
    // 视口再小一点就能分开：13+25+25 = 63 ≤ 70 → 3 行；25*2=50 ≤ 70 但 75 > 70 → 2 行
    CHECK(mc::ui::optionsVisibleRows(kVanillaShape, 0U, 70, 17U) == 3U);
    CHECK(mc::ui::optionsVisibleRows(kVanillaShape, 1U, 70, 17U) == 2U);
}

// --- 19. 双栏可转移列表（UI-6e ①-b / spec §5 的 L4）---------------------------
//
// ★ 它不是"一行两个控件"（那是 OptionsList）。是**两张互相独立的列表**，各有自己的
//   滚动位置与选中项。做成一张两列的列表会在第一次滚动时露馅：两栏条目数不同。
void testDualColumnLists() {
    // 1280x720 @ scale 3 → 逻辑 427x240，三段式内容区 y=33 高 174
    const mc::ui::HudLayout layout{1280.0F, 720.0F, 3};
    const auto frame =
        mc::ui::headerAndFooterLayout(layout.logicalWidth(), layout.logicalHeight());
    const auto lists = mc::ui::dualColumnLists(frame.contentBox(), layout.logicalWidth());

    // 两栏等宽，且都是 26.1 的 200
    CHECK(lists.available.width == mc::ui::kTransferListWidth);
    CHECK(lists.selected.width == mc::ui::kTransferListWidth);
    // ★ 相对画布中线对称：左栏右缘与右栏左缘到中线的距离相等，都是 15
    const int centre = layout.logicalWidth() / 2;
    CHECK(centre - lists.available.right() == mc::ui::kTransferCentreGap);
    CHECK(lists.selected.x - centre == mc::ui::kTransferCentreGap);
    // 中缝正好 30
    CHECK(lists.selected.x - lists.available.right() == mc::ui::kTransferCentreGap * 2);
    // 两栏不重叠——中缝为负是"两栏叠在一起"，画面上像一栏
    CHECK(lists.available.right() < lists.selected.x);

    // 行高 36（26.1 的 super(..., 33, 36)），不是设置行的 25
    CHECK(lists.available.rowHeight == 36);
    CHECK(lists.available.rowHeight != mc::ui::kOptionsRowHeight);
    // 行宽 = 列宽 - 4
    CHECK(lists.available.rowWidth == mc::ui::kTransferListWidth - mc::ui::kTransferRowInset);

    // 两栏共用内容区的 y 与高度
    CHECK(lists.available.y == lists.selected.y);
    CHECK(lists.available.height == lists.selected.height);
    CHECK(lists.available.y == frame.contentBox().y);

    // 行内：图标格贴左上角内缩 2，是 32x32；**不是**垂直居中
    const auto row = mc::ui::scrollListRow(lists.available, 0U);
    const auto icon = mc::ui::transferIconCell(row);
    CHECK(icon.width == 32.0F);
    CHECK(icon.height == 32.0F);
    CHECK(icon.x == row.x + 2.0F);
    CHECK(icon.y == row.y + 2.0F);
    // 图标没有垂直居中：行高 36、图标 32，居中会是 y + 2 —— 这里恰好同值，
    // 所以换个行高来证它确实是"贴顶内缩"而不是"居中"
    const mc::ui::UiRect tallRow{0.0F, 0.0F, 196.0F, 60.0F};
    CHECK(mc::ui::transferIconCell(tallRow).y == 2.0F);   // 居中会是 14

    // 文字块在图标右侧，不与图标重叠
    const auto text = mc::ui::transferTextCell(row);
    CHECK(text.x >= icon.x + icon.width);
    CHECK(text.width > 0.0F);
    // 描述宽度装得下（闭合关系已有 static_assert，这里量实际行）
    CHECK(text.width >= static_cast<float>(mc::ui::kTransferDescriptionWidth) - 2.0F);
}

// --- 20. 没有任何控件可以越出画布（UI-6e，对着一次真实的版面事故）------------
//
// ★ 起因：创建世界页的表单是**挂在按钮块上沿往上堆**的——
//       buttonTop = 逻辑高/2 - 按钮数*12;  名字标签 y = buttonTop - 80
//   1280x720 @ scale 3 的逻辑画布高 240，于是 buttonTop = 60、标签落在 **-20**：
//   世界名输入框被切出画布顶部，文件夹提示与标题糊在一起。加一个种子框就撞上了
//   （往上堆的高度从 30 涨到 80）。
//
//   **往上堆的版面没有上界**，而"顶出画布"不会让任何断言变红——只有截图看得见。
//   这条测试就是那个缺失的下界，而且它是**通用**的：所有页面、多档缩放一起量。
void testNoWidgetEscapesTheCanvas() {
    // 覆盖窄、方、宽三类画布与三档缩放。320x240 是 spec §1.1 的最小逻辑画布。
    struct Canvas final { float width; float height; int scale; };
    constexpr std::array<Canvas, 6> kCanvases{{
        {1280.0F, 720.0F, 3}, {1280.0F, 720.0F, 2}, {1280.0F, 720.0F, 1},
        {854.0F, 480.0F, 2},  {640.0F, 480.0F, 1},  {1920.0F, 1080.0F, 4},
    }};

    for (const Canvas& canvas : kCanvases) {
        const mc::ui::HudLayout layout{canvas.width, canvas.height, canvas.scale};
        for (std::size_t raw = 0; raw <= static_cast<std::size_t>(mc::ui::PageId::Accessibility);
             ++raw) {
            const auto page = static_cast<mc::ui::PageId>(raw);
            // 游戏内那几屏没有菜单按钮，装配是空的
            if (mc::ui::pageDrawKind(page) == mc::ui::PageDrawKind::InGame) {
                continue;
            }
            mc::ui::MenuBuildContext ctx;
            ctx.optionsWindow = mc::ui::optionsWindowFor(layout, page, 0U);
            const mc::ui::MenuCallbacks cb;
            mc::ui::Page built;
            mc::ui::buildPageInto(built, page, ctx, cb);
            mc::ui::layoutPageInto(built, page, layout, canvas.width, 0U,
                                   ctx.optionsWindow.firstRow);
            // 夹具自证：这些页面必须真的装配出了控件，否则上面那个循环是空的，
            // 整条测试就成了摆设。
            if (page == mc::ui::PageId::CreateWorld) {
                check(built.size() >= 5U, "the create-world page must assemble widgets", __LINE__);
            }
            for (const auto& widget : built) {
                const auto& rect = widget.rect;
                // ★ 上边界是这次事故的那一条：控件顶在画布外，画面上被切掉半截。
                check(rect.y >= -0.5F,
                      "a widget escaped the top of the canvas", __LINE__);
                check(rect.x >= -0.5F,
                      "a widget escaped the left of the canvas", __LINE__);
                check(rect.y + rect.height <= canvas.height + 0.5F,
                      "a widget escaped the bottom of the canvas", __LINE__);
                check(rect.x + rect.width <= canvas.width + 0.5F,
                      "a widget escaped the right of the canvas", __LINE__);
            }
        }
    }
}

// --- 21. 创建世界页的三段式表单 ----------------------------------------------
void testCreateWorldForm() {
    const mc::ui::HudLayout layout{1280.0F, 720.0F, 3};
    const auto form =
        mc::ui::createWorldLayout(layout.logicalWidth(), layout.logicalHeight());
    const auto frame =
        mc::ui::headerAndFooterLayout(layout.logicalWidth(), layout.logicalHeight());

    // ★ 表单从内容区**顶部**往下排，第一行就贴着页眉下沿——不是从按钮往上堆。
    CHECK(form.nameLabel.y == frame.contentBox().y);
    // 自上而下严格递增，且互不重叠
    CHECK(form.nameField.y > form.nameLabel.y);
    CHECK(form.folderHint.y >= form.nameField.y + form.nameField.height);
    CHECK(form.seedLabel.y >= form.folderHint.y + form.folderHint.height);
    CHECK(form.seedField.y >= form.seedLabel.y + form.seedLabel.height);
    // 按钮带在表单下方
    CHECK(static_cast<float>(form.optionButtonsTop) >=
          form.seedField.y + form.seedField.height);

    // 表单整体不越过页脚
    const auto footer = frame.footerBox();
    CHECK(form.seedField.y + form.seedField.height <= footer.y);
    // 三个循环按钮也不越过页脚
    const auto lastButton = mc::ui::createWorldOptionButton(form, layout.logicalWidth(), 2U);
    CHECK(lastButton.y + lastButton.height <= footer.y);

    // 页脚两个按钮横排、等宽、不重叠、整体居中
    CHECK(form.footerLeft.y == form.footerRight.y);
    CHECK(form.footerLeft.width == form.footerRight.width);
    CHECK(form.footerRight.x >= form.footerLeft.x + form.footerLeft.width);
    const float pairCentre =
        (form.footerLeft.x + form.footerRight.x + form.footerRight.width) * 0.5F;
    CHECK(std::abs(pairCentre - static_cast<float>(layout.logicalWidth()) * 0.5F) <= 1.0F);

    // 两个输入框等宽且水平居中
    CHECK(form.nameField.width == form.seedField.width);
    CHECK(form.nameField.x == form.seedField.x);
    CHECK(form.nameField.width == static_cast<float>(mc::ui::kCreateWorldFieldWidth));

    // 版式判定走表，不是手写清单
    CHECK(mc::ui::pageLayoutKind(mc::ui::PageId::CreateWorld) ==
          mc::ui::PageLayoutKind::HeaderFooterForm);

    // ★ 走**生产路径**看具体控件落在哪。上面那些只量了几何函数——把布局侧
    //   页脚两个按钮的序号判定写反（Create 跑到右边、Back 跑到左边），或者让内容区的
    //   三个按钮**不换行**（全叠在同一位置），几何函数一条都不会红：
    //   它返回的矩形本身还是对的，错的是"哪个控件拿到哪一个"。
    {
        mc::ui::MenuBuildContext ctx;
        const mc::ui::MenuCallbacks cb;
        mc::ui::Page page;
        mc::ui::buildPageInto(page, mc::ui::PageId::CreateWorld, ctx, cb);
        mc::ui::layoutPageInto(page, mc::ui::PageId::CreateWorld, layout, 1280.0F);
        const std::size_t count = mc::ui::countPageButtons(page);
        CHECK(count >= 5U);

        const auto rectOf = [&](mc::ui::WidgetId id) {
            for (const auto& widget : page) {
                if (static_cast<mc::ui::WidgetId>(widget.debugId) == id) {
                    return widget.rect;
                }
            }
            check(false, "expected widget missing from the create-world page", __LINE__);
            return mc::ui::UiRect{};
        };

        // 页脚：Create New World 在**左**，Back 在**右**，同一行
        const auto create = rectOf(mc::ui::WidgetId::CreateConfirm);
        const auto back = rectOf(mc::ui::WidgetId::Back);
        check(create.y == back.y, "the two footer buttons share a row", __LINE__);
        check(create.x < back.x, "Create sits left of Back", __LINE__);
        check(create.x + create.width <= back.x, "the footer buttons must not overlap", __LINE__);

        // 内容区的三个循环按钮自上而下**逐个换行**，互不重叠
        const auto mode = rectOf(mc::ui::WidgetId::CreateGameMode);
        const auto difficulty = rectOf(mc::ui::WidgetId::Difficulty);
        const auto commands = rectOf(mc::ui::WidgetId::CreateAllowCommands);
        check(difficulty.y >= mode.y + mode.height,
              "the difficulty button must sit below the game-mode button", __LINE__);
        check(commands.y >= difficulty.y + difficulty.height,
              "the allow-commands button must sit below the difficulty button", __LINE__);
        // 三个都在页脚之上
        check(commands.y + commands.height <= create.y,
              "the option buttons must stay above the footer", __LINE__);
    }

    // ★★ 表单矩形也不许越出画布——而 `testNoWidgetEscapesTheCanvas` **覆盖不到它们**：
    //    输入框、标签、文件夹提示都不是 `ui::Widget`，它们不进 `ui::Page`，只是绘制侧的
    //    矩形。这次事故越界的恰恰是它们（世界名框被切出画布顶部），所以那条通用护栏
    //    抓不住这次的 bug，必须在这里单独量一遍。
    //
    //    这是"两套界面栈"那个问题的小版本：凡是绕过 Widget 模型自己画的东西，
    //    Widget 上的护栏一概管不到。
    struct Canvas final { int width; int height; };
    constexpr std::array<Canvas, 5> kCanvases{{
        {427, 240}, {640, 360}, {1280, 720}, {320, 240}, {854, 480},
    }};
    for (const Canvas& canvas : kCanvases) {
        const auto solved = mc::ui::createWorldLayout(canvas.width, canvas.height);
        const auto solvedFrame = mc::ui::headerAndFooterLayout(canvas.width, canvas.height);
        const std::array<mc::ui::UiRect, 5> parts{{solved.nameLabel, solved.nameField,
                                                   solved.folderHint, solved.seedLabel,
                                                   solved.seedField}};
        for (const auto& rect : parts) {
            check(rect.y >= 0.0F, "a form row escaped the top of the canvas", __LINE__);
            check(rect.x >= 0.0F, "a form row escaped the left of the canvas", __LINE__);
            check(rect.y + rect.height <= static_cast<float>(canvas.height),
                  "a form row escaped the bottom of the canvas", __LINE__);
            check(rect.x + rect.width <= static_cast<float>(canvas.width),
                  "a form row escaped the right of the canvas", __LINE__);
            // 表单永远在页眉之下：顶出页眉就会和标题糊在一起（这次事故的第二个症状）
            check(rect.y >= solvedFrame.contentBox().y,
                  "a form row climbed into the header", __LINE__);
        }
        // 页脚两个按钮同样要在画布内
        for (const auto& rect : {solved.footerLeft, solved.footerRight}) {
            check(rect.x >= 0.0F && rect.x + rect.width <= static_cast<float>(canvas.width),
                  "a footer button escaped the canvas", __LINE__);
        }
    }
}

} // namespace

int main() {
    testHeaderAndFooter();
    testOptionsList();
    testKeyBindRowCells();
    testKeyBindDecoration();
    testGroupedSlots();
    testControlsHubLayout();
    testPageTitles();
    testKeyBindListRows();
    testEscapeUnbindsSourceGuard();
    testScrollbarUsesRowCountSourceGuard();
    testBigGroups();
    testScrolledSlots();
    testVideoSettingsWindowedLayout();
    testWhichPagesActuallyScroll();
    testWindowSingleSourceGuard();
    testSliderCursorMapping();
    testSliderDragSingleSourceGuard();
    testFloatSliders();
    testOptionsHeaderRows();
    testDualColumnLists();
    testNoWidgetEscapesTheCanvas();
    testCreateWorldForm();
    if (failures != 0) {
        std::printf("options_layout_test: %d checks failed\n", failures);
        return 1;
    }
    return 0;
}

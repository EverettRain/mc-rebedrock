// UI-11 / A6：世界选择列表里**一行**的几何与内容
// （26.1 `WorldSelectionList.WorldListEntry` + `AbstractSelectionList.Entry`）。
//
// ★ 断言钉的是 26.1 那几条式子里的数，不是本作实现跑出来的值：
//     WorldSelectionList:116   super(minecraft, width, height, 0, 36)   ← itemHeight
//     WorldSelectionList:251   getRowWidth() = 270
//     WorldSelectionList:403   ICON_SIZE = 32
//     WorldSelectionList:571   getTextX() = getContentX() + 32 + 3
//     WorldSelectionList:499-504  三行 y = contentY + 1 / +9+3 / +9+9+3
//     WorldSelectionList:421   maxTextWidth = 270 - getTextX() - 2（构造期 x==0 → 231）
//     AbstractSelectionList:471-481  content 四周各让 2
//     AbstractSelectionList:429-434  第二行是 `<目录名> (<日期>)`，lastPlayed 无记录时只有目录名

#include "ui/MenuGeometry.hpp"
#include "ui/PageBuilder.hpp"
#include "ui/WorldListRow.hpp"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        std::printf("world_list_row_test line %d: %s\n", line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

// --- 1. 行本身：26.1 的 270 x 36 ---------------------------------------------
void testRowBox() {
    const mc::ui::HudLayout layout{1280.0F, 720.0F, 3, false};   // 逻辑 427x240
    const auto row = mc::ui::logicalWorldListRow(0U, layout);
    CHECK(row.width == 270.0F);
    // ★ 36，不是本作从前自造的 22，也不是"行距减 2"。26.1 的行与行之间**不留缝**。
    CHECK(row.height == 36.0F);
    const auto next = mc::ui::logicalWorldListRow(1U, layout);
    CHECK(next.y - row.y == 36.0F);
    // 行在画布里居中：427/2 - 270/2 = 213 - 135 = 78（两次整数除法，不化简）。
    CHECK(row.x == 78.0F);
}

// --- 2. 行内几块：图标、三行字 -----------------------------------------------
void testRowParts() {
    const mc::ui::UiRect row{78.0F, 34.0F, 270.0F, 36.0F};
    const auto parts = mc::ui::worldRowParts(row);
    // content 四周各让 2（AbstractSelectionList:471-481）。
    CHECK(parts.icon.x == 80.0F);
    CHECK(parts.icon.y == 36.0F);
    // ICON_SIZE = 32，而且 contentHeight = 36 - 4 = 32 —— 图标正好占满内容区的高。
    CHECK(parts.icon.width == 32.0F);
    CHECK(parts.icon.height == 32.0F);
    // getTextX() = contentX + 32 + 3。
    CHECK(parts.textX == 115.0F);
    // ★ 三行**不是等距的**：+1 / +12 / +21，第一段差 11、第二段差 9。
    //   写成 `+1 + i*10` 会让第二、三行各偏 1 与 2。
    CHECK(parts.nameY == 37.0F);
    CHECK(parts.metaY == 48.0F);
    CHECK(parts.infoY == 57.0F);
    CHECK(parts.metaY - parts.nameY == 11.0F);
    CHECK(parts.infoY - parts.metaY == 9.0F);
    // spec §7 的 `<WorldRow>` 里第三行写的是 `top + 24`，那是旧值；源码是 contentY + 21。
    CHECK(parts.infoY != row.y + 24.0F);
}

// --- 3. 文字宽度上限 ---------------------------------------------------------
void testMaxTextWidth() {
    // 270 - (2 + 32 + 3) - 2 = 231。
    CHECK(mc::ui::kWorldRowMaxTextWidth == 231);
}

// --- 4. 第二行的拼接 ---------------------------------------------------------
void testMetaLine() {
    CHECK(mc::ui::worldRowMetaLine("new-world", "2024-01-02 03:04") ==
          "new-world (2024-01-02 03:04)");
    // 没有"最后游玩"记录时**只有目录名**，连括号都不画（26.1 的 lastPlayed != -1 分支）。
    CHECK(mc::ui::worldRowMetaLine("flat-testbed", "") == "flat-testbed");
}

// --- 5. 颜色 -----------------------------------------------------------------
void testColours() {
    // 第 2、3 行与未聚焦时的选中边框：`withColor(-8355712)` = 0xFF808080。
    CHECK(mc::ui::kWorldRowSecondaryChannel * 255.0F == 128.0F);
    // 悬停时盖在图标上的那一层：`-1601138544` = 0xA0909090。
    CHECK(mc::ui::kWorldIconHoverChannel * 255.0F == 144.0F);
    CHECK(mc::ui::kWorldIconHoverAlpha * 255.0F == 160.0F);
}

// --- 6. 装配 + 布局：缩略图跟着自己那一行，行拿到的是**列表**的矩形 ----------
mc::ui::MenuBuildContext worldContext(std::size_t rows) {
    mc::ui::MenuBuildContext ctx;
    ctx.labelFor = [](std::uint16_t) { return std::string{"x"}; };
    ctx.worldRowCount = rows;
    ctx.worldSelectable = rows > 0U;
    return ctx;
}

void testPageLayout() {
    using mc::ui::WidgetId;
    using mc::ui::WidgetKind;
    const auto ctx = worldContext(3U);
    mc::ui::MenuCallbacks cb;
    mc::ui::Page page;
    mc::ui::buildPageInto(page, mc::ui::PageId::WorldList, ctx, cb);
    // 三行，每行「一行 + 一张图」，后面四个按钮。
    CHECK(page.size() == 3U * 2U + 4U);
    CHECK(page[0].kind == WidgetKind::ListRow);
    CHECK(page[1].kind == WidgetKind::Image);
    CHECK(static_cast<WidgetId>(page[1].debugId) == WidgetId::WorldIcon);
    // 缩略图带着自己的行号，而不是靠"上一个控件是第几行"。
    CHECK(page[1].imageIndex == 0U);
    CHECK(page[5].imageIndex == 2U);
    // 图不可交互：命中会穿过去落到它下面那一行上。
    CHECK(!page[1].interactive());

    const mc::ui::HudLayout layout{1280.0F, 720.0F, 3, false};
    mc::ui::layoutPageInto(page, mc::ui::PageId::WorldList, layout);
    const float scale = layout.scale();
    // ★ 行拿到的必须是**列表行**的矩形。此前它落在 BottomBandTwoColumn 那条路上，
    //   拿的是底部按钮的矩形（宽 200、贴着画布底），而绘制与命中两侧各自绕过
    //   Widget::rect 去调 worldListRow()，所以画面上看不出来。
    CHECK(page[0].rect.width == 270.0F * scale);
    CHECK(page[0].rect.height == 36.0F * scale);
    CHECK(page[2].rect.y - page[0].rect.y == 36.0F * scale);
    // 缩略图坐在它那一行里，位置就是 worldRowParts 给的那一块。
    const auto expected =
        mc::ui::worldRowParts(mc::ui::logicalWorldListRow(0U, layout)).icon;
    CHECK(page[1].rect.x == expected.x * scale);
    CHECK(page[1].rect.y == expected.y * scale);
    CHECK(page[1].rect.width == 32.0F * scale);
    // 四个按钮仍然在底部带里，而且**行不占按钮序号**：第一个按钮是第 0 个按钮。
    CHECK(mc::ui::countPageButtons(page) == 4U);
    CHECK(page[6].rect.y ==
          mc::ui::frontendButtonRect(layout, mc::ui::PageId::WorldList, 0U, 4U).y);
}

// --- 7. 回归：可见行多到把按钮序号顶过 20 时不再抛 ---------------------------
void testManyRowsDoNotThrow() {
    // scale 1 的 1280x720 逻辑画布能放下 (720-…)/36 行，远多于 20 - 4 = 16。
    // 从前每一行都要一个 `bottomMenuButton(index, buttonCount, 2)`，而那个函数对
    // `buttonCount > kMaximumMenuButtons(20)` 抛 out_of_range —— 也就是**闪退**。
    const auto ctx = worldContext(30U);
    mc::ui::MenuCallbacks cb;
    mc::ui::Page page;
    mc::ui::buildPageInto(page, mc::ui::PageId::WorldList, ctx, cb);
    const mc::ui::HudLayout layout{1280.0F, 720.0F, 1, false};
    bool threw = false;
    try {
        mc::ui::layoutPageInto(page, mc::ui::PageId::WorldList, layout);
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);
    CHECK(mc::ui::countPageButtons(page) == 4U);

    // 语言行同族（它此前也在那条路上）。
    mc::ui::MenuBuildContext language;
    language.labelFor = [](std::uint16_t) { return std::string{"x"}; };
    language.languageRowCount = 30U;
    mc::ui::Page languagePage;
    mc::ui::buildPageInto(languagePage, mc::ui::PageId::Language, language, cb);
    threw = false;
    try {
        mc::ui::layoutPageInto(languagePage, mc::ui::PageId::Language, layout);
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);
}

} // namespace

int main() {
    testRowBox();
    testRowParts();
    testMaxTextWidth();
    testMetaLine();
    testColours();
    testPageLayout();
    testManyRowsDoNotThrow();
    if (failures > 0) {
        std::printf("world_list_row_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("world_list_row_test: all checks passed\n");
    return 0;
}

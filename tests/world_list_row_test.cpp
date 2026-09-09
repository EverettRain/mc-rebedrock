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

#include "core/BrokenDownTime.hpp"
#include "render/vulkan/HudTypes.hpp"
#include "ui/MenuGeometry.hpp"
#include "ui/PageBuilder.hpp"
#include "ui/WorldListRow.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef MC_REBEDROCK_RENDERER_SRC
#error "MC_REBEDROCK_RENDERER_SRC must point at src/render/vulkan/VulkanRenderer.cpp"
#endif

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

// --- 1b. 列表带：底衬与两条分隔线的矩形，必须**正好装下**那几行 --------------
//
// ★ 这一条是现场缺陷逼出来的（export/savelist-problem.png）：绘制侧自己算了一份
//   `visibleRows * 22 + 8`，而 A6 把行距改成了 36。带比内容矮了近四成，下缘那条
//   分隔线**穿过第五行的中间**，后面的行画在带外面。
//   「一份几何两处表述」的老形状——现在带与行都从 `worldScrollList` 派生。
void testListBand() {
    for (const int scale : {1, 2, 3}) {
        const mc::ui::HudLayout layout{1280.0F, 720.0F, scale, false};
        const auto band = mc::ui::worldListBox(layout);
        const std::size_t rows = mc::ui::worldListVisibleRows(layout);
        CHECK(rows >= 1U);
        // 带的高**正好**是"放得下几行 x 行距"，不多不少。
        CHECK(band.height == static_cast<float>(rows) * 36.0F * layout.scale());
        // 带的上缘就是第一行的上缘（此前绘制侧还往上多留了 4 像素）。
        CHECK(band.y == mc::ui::worldListRow(0U, layout).y);
        // ★ 最后一行**整行**都在带里。带按 22 一行算高时这一条会红。
        const auto last = mc::ui::worldListRow(rows - 1U, layout);
        CHECK(last.y + last.height <= band.y + band.height);
        // 带整宽（26.1 的列表视口是整宽的，行自己在里面居中）。
        CHECK(band.x == 0.0F);
        CHECK(band.width >= 1280.0F);
    }
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

// --- 4a. 日期的拆分：时区是**参数**，不是进程状态 ---------------------------
void testBrokenDownTime() {
    // 1704164645 = 2024-01-02T03:04:05Z。`tm_year` 从 1900 起、`tm_mon` 从 0 起。
    const std::tm utc = mc::core::brokenDownTime(1704164645, /*utc=*/true);
    CHECK(utc.tm_year == 124);
    CHECK(utc.tm_mon == 0);
    CHECK(utc.tm_mday == 2);
    CHECK(utc.tm_hour == 3);
    CHECK(utc.tm_min == 4);
    CHECK(utc.tm_sec == 5);
    // ★ 这一条无论跑它的机器在哪个时区都成立——这正是"时区做成参数"的意义。
    //   第一版是在截图通道里 `setenv("TZ","UTC")`：Windows 的 CRT 没有 setenv
    //   （交叉构建当场报错），而且它与 getenv 并发是未定义行为。
    //   把 `utc` 那个实参忽略掉（两支都调 localtime）时，非 UTC 机器上这六条会红。
}

// --- 4b. 第三行的拼接（26.1 LevelSummary.createInfo:166-186）-----------------
void testInfoLine() {
    // `gameMode.survival` + ", " + `selectWorld.version` + " " + 版本名。
    // 三个键在 26.1 的 en_us 里分别是 "Survival Mode" / "Version:" / 版本号，
    // 所以整行是 "Survival Mode, Version: 26.1"——那个冒号来自译文，不是这里加的。
    CHECK(mc::ui::worldRowInfoLine("Survival Mode", "Version:", "26.1") ==
          "Survival Mode, Version: 26.1");
    // 旧存档的版本块是重建出来的（SaveVersionHeader::derived），版本名是空串：
    // 那时不画", Version: " 这一段，而不是画一个空版本号。
    CHECK(mc::ui::worldRowInfoLine("Creative Mode", "Version:", "") == "Creative Mode");
}

// --- 4c. 缩略图在图集那一层的槽位 -------------------------------------------
void testIconSlots() {
    // 一层 256x256 切成 4x4 的 64x64。
    CHECK(mc::render::kWorldIconSlotCount == 16);
    const auto first = mc::render::worldIconSlotRect(0);
    CHECK(first.x == 0.0F && first.y == 0.0F);
    CHECK(first.width == 64.0F && first.height == 64.0F);
    // 第 3 个在第一行最右，第 4 个换行——`slot % 4` 是列、`slot / 4` 是行，
    // 反过来写（列取商、行取余）在 slot 0..3 上**结果相同**，从第 4 个才分岔。
    CHECK(mc::render::worldIconSlotRect(3).x == 192.0F);
    CHECK(mc::render::worldIconSlotRect(3).y == 0.0F);
    CHECK(mc::render::worldIconSlotRect(4).x == 0.0F);
    CHECK(mc::render::worldIconSlotRect(4).y == 64.0F);
    CHECK(mc::render::worldIconSlotRect(15).x == 192.0F);
    CHECK(mc::render::worldIconSlotRect(15).y == 192.0F);
    // 十六个槽位互不重叠、且都落在这一层里。
    for (int slot = 0; slot < mc::render::kWorldIconSlotCount; ++slot) {
        const auto rect = mc::render::worldIconSlotRect(slot);
        CHECK(rect.x + rect.width <= 256.0F);
        CHECK(rect.y + rect.height <= 256.0F);
        for (int other = 0; other < slot; ++other) {
            const auto previous = mc::render::worldIconSlotRect(other);
            CHECK(previous.x != rect.x || previous.y != rect.y);
        }
    }
    // 回落图标不在这一层（它是启动时烘死的静态美术，与运行期刷新的这一层是两回事）。
    CHECK(mc::render::kWorldIconLayer != mc::render::kTabWidgetLayer);
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
    // 缩略图坐在**它自己那一行**里，位置就是 worldRowParts 给的那一块。
    //
    // ★ 三行都要断言，不能只断言第 0 行：把 imageIndex 换成常量 0（"所有图标都画在
    //   第一行"）时，第 0 行的矩形**一个字节都不变**——第一轮 sabotage 就是这么
    //   漏过去的（REGULAR §5 的第一问，这一次是断言不够，不是夹具分辨不出）。
    for (std::size_t row = 0; row < 3U; ++row) {
        const auto expected =
            mc::ui::worldRowParts(mc::ui::logicalWorldListRow(row, layout)).icon;
        const auto& image = page[row * 2U + 1U];
        CHECK(image.rect.x == expected.x * scale);
        CHECK(image.rect.y == expected.y * scale);
        CHECK(image.rect.width == 32.0F * scale);
    }
    // 相邻两行的图标正好差一个行高——"都画在第一行"会让这个差变成 0。
    CHECK(page[3].rect.y - page[1].rect.y == 36.0F * scale);
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

// --- 8. 源码守：存档缩略图的**时机**照 26.1，不是退出时抓 --------------------
//
// ★ 这一条是现场反馈逼出来的：第一版在 `returnToTitle` 里抓最后一帧，于是每个存档的
//   图标都是**暂停菜单那块灰蒙蒙的底**。26.1 是在游戏内渲染循环里抓的
//   （`GameRenderer.tryTakeScreenshotIfNeeded`:614-631，紧跟 `renderLevel`:445 之后），
//   而那一行只在 `advanceGameTime` 为真时才跑——**菜单开着时根本不会抓**。
//   规则住在渲染器的翻译单元里，没有测试链接得到它，只能读源码守。
void testWorldIconTimingSourceGuard() {
    std::ifstream input{MC_REBEDROCK_RENDERER_SRC, std::ios::binary};
    if (!input) {
        std::printf("world_list_row_test: cannot open %s\n", MC_REBEDROCK_RENDERER_SRC);
        ++failures;
        return;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    const std::size_t begin = source.find("void updateWorldIconRequest()");
    CHECK(begin != std::string::npos);
    if (begin == std::string::npos) {
        return;
    }
    const std::size_t end = source.find("\n    // 上一帧是被选中的那一帧", begin);
    const std::string body = source.substr(begin, end - begin);
    // ② 暂停时不抓 —— 这正是"每张图标都是暂停菜单"的那条修正。
    CHECK(body.find("paused") != std::string::npos);
    // ③ 已经有 icon.png 就永远不再抓（26.1 的 hasWorldScreenshot）。
    CHECK(body.find("is_regular_file") != std::string::npos);
    CHECK(body.find("worldIconDone_ = true") != std::string::npos);
    // ④ 每秒最多试一次。
    CHECK(body.find("std::chrono::seconds{1}") != std::string::npos);
    // ⑤ 世界真的画出来了才抓。
    CHECK(body.find("pendingSectionUpdates.empty()") != std::string::npos);
    // 出图与方块预览两条路径都不许写盘。
    CHECK(body.find("uiCapture.has_value()") != std::string::npos);
    CHECK(body.find("testScene.has_value()") != std::string::npos);

    // ★ 而且 `returnToTitle` 里**不能**再有抓图那一步。
    const std::size_t exitAt = source.find("void returnToTitle(bool saveFirst)");
    CHECK(exitAt != std::string::npos);
    if (exitAt != std::string::npos) {
        const std::size_t exitEnd = source.find("\n    // ---- 输入归属", exitAt);
        const std::string exitBody = source.substr(exitAt, exitEnd - exitAt);
        CHECK(exitBody.find("writeCurrentWorldIcon()") == std::string::npos);
    }
}

} // namespace

int main() {
    testRowBox();
    testListBand();
    testRowParts();
    testMaxTextWidth();
    testBrokenDownTime();
    testMetaLine();
    testInfoLine();
    testIconSlots();
    testColours();
    testPageLayout();
    testWorldIconTimingSourceGuard();
    testManyRowsDoNotThrow();
    if (failures > 0) {
        std::printf("world_list_row_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("world_list_row_test: all checks passed\n");
    return 0;
}

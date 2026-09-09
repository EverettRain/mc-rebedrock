// UI-11 / A5：全屏提示屏（26.1 `WarningScreen` / `SafetyScreen`）与复选框
// （26.1 `Checkbox`）的几何。
//
// ★ 每一个数都是照 26.1 那几条式子**手算**的，不是抄本作实现跑出来的值：
//     Checkbox.getBoxSize            = 9 + 8
//     Checkbox.getDefaultWidth       = boxSize + 4 + font.width(message)
//     Checkbox.extractContents       textY = y + boxSize/2 - textHeight/2
//     WarningScreen.init             竖列 spacing 8，正文格 padding 12，
//                                    正文控件 (width-100) x (height-100) 再 minimizeHeight
//     AbstractChildWrapper.setX      offset = (int)Mth.lerp(a, least, most)      ← 截断
//     AbstractChildWrapper.setY      offset = Math.round(Mth.lerp(a, least, most)) ← 四舍五入
//     FrameLayout.centerInRectangle  整块内容在整屏里居中
//     Button.DEFAULT_WIDTH/HEIGHT    150 / 20，页脚 LinearLayout.horizontal().spacing(8)
//
// ★ spec §2.5 的两个数已在这里被推翻并钉住：复选框 blit 的边长是 **17** 不是 20，
//   整体宽是 **21 + 文字宽** 不是 24 + 文字宽。

#include "render/vulkan/GuiSpriteAtlas.hpp"
#include "ui/MenuGeometry.hpp"
#include "ui/MenuInteraction.hpp"
#include "ui/PageLayoutKind.hpp"
#include "ui/PageTitles.hpp"
#include "ui/NoticeScreen.hpp"
#include "ui/PageBuilder.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        std::printf("notice_screen_test line %d: %s\n", line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

bool sameRect(const mc::ui::UiRect& rect, float x, float y, float width, float height) {
    return rect.x == x && rect.y == y && rect.width == width && rect.height == height;
}

// --- 1. Checkbox：spec §2.5 的两个数都是旧值 --------------------------------
void testCheckboxMetrics() {
    // `getBoxSize(font)` = 9 + BOX_PADDING(8)。精灵美术是 20x20，blit 出去的是 17。
    CHECK(mc::ui::kCheckboxBoxSize == 17);
    CHECK(mc::ui::kCheckboxSpacing == 4);
    // `getDefaultWidth` = 17 + 4 + 文字宽。spec 写的 24 + 文字宽是 1.20 的旧值。
    CHECK(mc::ui::checkboxWidth(140) == 161);
    CHECK(mc::ui::checkboxWidth(0) == 21);
    // `getAdjustedHeight` = max(boxSize, 文字块高)。一行 9 → 17；两行 18 → 18。
    CHECK(mc::ui::checkboxHeight(9) == 17);
    CHECK(mc::ui::checkboxHeight(18) == 18);

    // `extractContents`：盒子画在控件左上角，文字在 x + 17 + 4。
    const auto parts = mc::ui::checkboxParts({40.0F, 60.0F, 161.0F, 17.0F}, 9);
    CHECK(sameRect(parts.box, 40.0F, 60.0F, 17.0F, 17.0F));
    CHECK(parts.textX == 61.0F);
    // textY = y + 17/2 - 9/2 = y + 8 - 4。
    CHECK(parts.textY == 64.0F);
    // ★ 两次整数除法**先各自除再相减**，不能化简成 (17 - 18) / 2 = 0：
    //   两行文字时 vanilla 给的是 8 - 9 = -1。
    const auto twoLines = mc::ui::checkboxParts({40.0F, 60.0F, 161.0F, 18.0F}, 18);
    CHECK(twoLines.textY == 59.0F);
}

// --- 2. 布局引擎的两种取整 ---------------------------------------------------
void testLayoutRounding() {
    // 偶数余量上两者相同。
    CHECK(mc::ui::layoutOffsetX(0, 76) == 38);
    CHECK(mc::ui::layoutOffsetY(0, 76) == 38);
    // ★ 奇数余量上分道扬镳：setX 截断（60），setY 四舍五入（61）。
    //   26.1 的横竖两个方向确实用了两个不同的取整，这不是笔误。
    CHECK(mc::ui::layoutOffsetX(0, 121) == 60);
    CHECK(mc::ui::layoutOffsetY(0, 121) == 61);
    // 带 padding 的那一支：least/most 不是 0/L，lerp 走的是这两端之间。
    CHECK(mc::ui::layoutOffsetX(12, 76) == 44);
}

// --- 3. 正文框：宽高与换行宽 -------------------------------------------------
void testMessageBox() {
    // `new FittingMultiLineTextWidget(0, 0, width - 100, height - 100, …)`
    CHECK(mc::ui::noticeMessageBoxWidth(427) == 327);
    // 换行宽再减去 `totalInnerPadding()` = 4 * 2。
    CHECK(mc::ui::noticeMessageWrapWidth(427) == 319);
    // `minimizeHeight()`：内容装得下就收缩到 行数*9 + 8。
    CHECK(mc::ui::noticeMessageBoxHeight(240, 6) == 62);
    // 装不下（可滚）就保持 height - 100，不再收缩。
    CHECK(mc::ui::noticeMessageBoxHeight(240, 20) == 140);

    // 逐行左对齐，从 (x+4, y+4) 起每行 9。
    const mc::ui::UiRect box{50.0F, 71.0F, 327.0F, 62.0F};
    CHECK(sameRect(mc::ui::noticeMessageLineRect(box, 0), 54.0F, 75.0F, 319.0F, 9.0F));
    CHECK(sameRect(mc::ui::noticeMessageLineRect(box, 3), 54.0F, 102.0F, 319.0F, 9.0F));
}

// --- 4. 整屏版面：正文格最宽的那一档（1280x720 @ scale 3） -------------------
void testWideLayout() {
    // 手算：
    //   正文 327x62，格 351x86；复选框 161x17；按钮行 308x20；页脚 308x45
    //   内容列 max(100, 351, 308) = 351 宽，9+8+86+8+45 = 156 高
    //   originX = (int)(0.5 * (427 - 351)) = 38
    //   originY = round(0.5 * (240 - 156)) = round(42.0) = 42
    const auto notice = mc::ui::noticeLayout(427, 240, 6, {100, 140});
    CHECK(sameRect(notice.title, 163.0F, 42.0F, 100.0F, 9.0F));   // 38 + (int)125.5
    CHECK(sameRect(notice.message, 50.0F, 71.0F, 327.0F, 62.0F)); // 38+12, 42+17+12
    CHECK(sameRect(notice.check, 132.0F, 153.0F, 161.0F, 17.0F)); // 59 + (int)73.5
    CHECK(sameRect(notice.proceed, 59.0F, 178.0F, 150.0F, 20.0F));
    CHECK(sameRect(notice.back, 217.0F, 178.0F, 150.0F, 20.0F));  // 59 + 150 + 8
}

// --- 5. 窄画布：页脚才是最宽的那一个 ----------------------------------------
void testNarrowLayout() {
    // 画布 320 宽时 正文格 244 < 按钮行 308，内容列宽由**页脚**决定。
    //   originX = (int)(0.5 * (320 - 308)) = 6
    //   originY = round(0.5 * (240 - 120)) = 60
    // ★ 这一档是 `titleWidth` / `checkTextWidth` 唯一说得上话的地方，也是正文那个
    //   12 的 padding lerp 唯一不等于 12 的地方：
    //     x = 6 + (int)Mth.lerp(0.5, 12, 308 - 220 - 12) = 6 + 44 = 50
    //   把它写成"永远贴左 12"会得到 18。
    const auto notice = mc::ui::noticeLayout(320, 240, 2, {60, 140});
    CHECK(sameRect(notice.title, 130.0F, 60.0F, 60.0F, 9.0F));
    CHECK(sameRect(notice.message, 50.0F, 89.0F, 220.0F, 26.0F));
    CHECK(sameRect(notice.check, 79.0F, 135.0F, 161.0F, 17.0F));
    CHECK(sameRect(notice.proceed, 6.0F, 160.0F, 150.0F, 20.0F));
    CHECK(sameRect(notice.back, 164.0F, 160.0F, 150.0F, 20.0F));
}

// --- 6. 奇数余量：横截断 / 竖四舍五入，在整屏版面上也要成立 ------------------
void testOddRemainder() {
    // 321 - 308 = 13 → x 截断成 6；241 - 120 = 121 → y 四舍五入成 61。
    // 两个方向都用截断的话 y 会是 60，两个方向都用 round 的话 x 会是 7。
    const auto notice = mc::ui::noticeLayout(321, 241, 2, {60, 140});
    CHECK(notice.proceed.x == 6.0F);
    CHECK(notice.proceed.y == 161.0F);
    CHECK(notice.check.y == 136.0F);
}

// --- 7. 复选框四态精灵 -------------------------------------------------------
void testCheckboxSprites() {
    using mc::render::GuiWidgetSprite;
    // 26.1 `Checkbox.extractContents`：selected 选上下两张，isFocused 选左右两张。
    CHECK(mc::render::checkboxSprite(false, false) == GuiWidgetSprite::Checkbox);
    CHECK(mc::render::checkboxSprite(true, false) == GuiWidgetSprite::CheckboxSelected);
    CHECK(mc::render::checkboxSprite(false, true) == GuiWidgetSprite::CheckboxHighlighted);
    CHECK(mc::render::checkboxSprite(true, true) ==
          GuiWidgetSprite::CheckboxSelectedHighlighted);
    // 四态互不相同——把"勾上"和"有焦点"接反了仍然是四张不同的图，所以上面四条
    // 必须逐条钉住取值，这一条只是防止有人把某两态合并成同一张。
    CHECK(mc::render::checkboxSprite(false, false) != mc::render::checkboxSprite(true, false));
    CHECK(mc::render::checkboxSprite(false, false) != mc::render::checkboxSprite(false, true));
    CHECK(mc::render::checkboxSprite(true, false) !=
          mc::render::checkboxSprite(true, true));
}

// --- 8. 装配与布局：一页的形状，以及"点 Proceed 不会触发 Back" ---------------
mc::ui::MenuBuildContext noticeContext() {
    mc::ui::MenuBuildContext ctx;
    ctx.labelFor = [](std::uint16_t) { return std::string{"x"}; };
    ctx.noticeMessageLines = {"one", "two", "three", "four", "five", "six"};
    ctx.noticeMetrics = {100, 140};
    return ctx;
}

void testPageShape() {
    using mc::ui::WidgetId;
    using mc::ui::WidgetKind;
    const auto ctx = noticeContext();
    mc::ui::MenuCallbacks cb;
    int proceeded = 0;
    int toggled = 0;
    int backed = 0;
    cb.proceedAdvancedGraphicsNotice = [&] { ++proceeded; };
    cb.toggleNoticeStopShowing = [&] { ++toggled; };
    cb.back = [&] { ++backed; };

    mc::ui::Page page;
    mc::ui::buildPageInto(page, mc::ui::PageId::AdvancedGraphicsNotice, ctx, cb);
    // 标题 + 六行正文 + 复选框 + Proceed + Back
    CHECK(page.size() == 10U);
    CHECK(page[0].kind == WidgetKind::Label);
    CHECK(static_cast<WidgetId>(page[0].debugId) == WidgetId::NoticeTitle);
    CHECK(page[1].kind == WidgetKind::Label);
    CHECK(page[6].label == "six");
    CHECK(page[7].kind == WidgetKind::Checkbox);
    CHECK(static_cast<WidgetId>(page[8].debugId) == WidgetId::NoticeProceed);
    CHECK(static_cast<WidgetId>(page[9].debugId) == WidgetId::Back);
    // 标题与正文是 Label，永远不可交互——焦点遍历跳过，点它也不该有反应。
    CHECK(!page[0].interactive());
    CHECK(!page[3].interactive());

    // 1280x720 @ scale 3 → 逻辑 427x240，也就是第 4 条手算的那一档。
    const mc::ui::HudLayout layout{1280.0F, 720.0F, 3, false};
    mc::ui::layoutPageInto(page, mc::ui::PageId::AdvancedGraphicsNotice, layout, 0U, 0U,
                           mc::ui::CreateWorldTab::Game, ctx.noticeMetrics);
    const float scale = layout.scale();
    // ★ Proceed 拿的是**左**边那个矩形。把这两行的次序调过来，页面装配与布局
    //   各自都仍然自洽——只有这条断言会红。
    CHECK(page[8].rect.x == 59.0F * scale);
    CHECK(page[9].rect.x == 217.0F * scale);
    CHECK(page[7].rect.y == 153.0F * scale);
    // 正文六行各自一行的高度，逐行往下 9。
    CHECK(page[2].rect.y - page[1].rect.y == 9.0F * scale);

    // 通用护栏：没有控件跑出画布。
    for (const auto& widget : page) {
        CHECK(widget.rect.x >= 0.0F);
        CHECK(widget.rect.y >= 0.0F);
        CHECK(widget.rect.x + widget.rect.width <= 1280.0F);
        CHECK(widget.rect.y + widget.rect.height <= 720.0F);
    }

    // 命中：点复选框翻状态，点 Proceed 才提交。
    const auto hit = [&](const mc::ui::UiRect& rect) {
        return mc::ui::hitTest(page, rect.x + 1.0F, rect.y + 1.0F);
    };
    const std::size_t onCheck = hit(page[7].rect);
    CHECK(onCheck == 7U);
    // 正文那几行盖不住复选框：Label 不可交互，命中会穿过去。
    CHECK(hit(page[3].rect) == mc::ui::kNoWidget);
    if (onCheck != mc::ui::kNoWidget && page[onCheck].onActivate) {
        page[onCheck].onActivate();
    }
    CHECK(toggled == 1 && proceeded == 0 && backed == 0);
    const std::size_t onProceed = hit(page[8].rect);
    CHECK(onProceed == 8U);
    if (onProceed != mc::ui::kNoWidget && page[onProceed].onActivate) {
        page[onProceed].onActivate();
    }
    CHECK(proceeded == 1 && backed == 0);
}

// --- 9. 勾选状态是**装配时的快照**，不是绘制侧另算的 ------------------------
void testCheckedSnapshot() {
    auto ctx = noticeContext();
    mc::ui::MenuCallbacks cb;
    mc::ui::Page off;
    mc::ui::buildPageInto(off, mc::ui::PageId::AdvancedGraphicsNotice, ctx, cb);
    CHECK(!off[7].checked);
    ctx.noticeStopShowing = true;
    mc::ui::Page on;
    mc::ui::buildPageInto(on, mc::ui::PageId::AdvancedGraphicsNotice, ctx, cb);
    CHECK(on[7].checked);
}

// --- 10. 版面种类与标题归属 --------------------------------------------------
void testPageClassification() {
    using mc::ui::PageId;
    using mc::ui::PageLayoutKind;
    CHECK(mc::ui::pageLayoutKind(PageId::AdvancedGraphicsNotice) ==
          PageLayoutKind::CentredNotice);
    // ★ 提示屏的标题由页面自己的 Label 画。绘制侧再画一行就是两个标题——
    //   这一条钉的正是 drawPauseMenu 里那个 if 的判据。
    CHECK(mc::ui::drawsTitleAsWidget(PageLayoutKind::CentredNotice));
    CHECK(!mc::ui::drawsTitleAsWidget(PageLayoutKind::HeaderFooterList));
    CHECK(!mc::ui::drawsTitleAsWidget(PageLayoutKind::CentredColumn));
    // 它不是三段式：没有页眉分隔线、没有页脚带、没有列表底衬。
    CHECK(!mc::ui::usesHeaderAndFooter(PageLayoutKind::CentredNotice));
    // 它有自己的标题条目，而不是掉进别的页的标题。
    CHECK(std::string{mc::ui::pageTitle(PageId::AdvancedGraphicsNotice).key} !=
          std::string{mc::ui::pageTitle(PageId::AdvancedGraphics).key});
}

} // namespace

int main() {
    testCheckboxMetrics();
    testLayoutRounding();
    testMessageBox();
    testWideLayout();
    testNarrowLayout();
    testOddRemainder();
    testCheckboxSprites();
    testPageShape();
    testCheckedSnapshot();
    testPageClassification();
    if (failures > 0) {
        std::printf("notice_screen_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("notice_screen_test: all checks passed\n");
    return 0;
}

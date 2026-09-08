// UI-2：主菜单版面的黄金值。
//
// 在这个测试之前，主菜单的按钮坐标全仓没有任何断言：menu_layout_test 只检查"每个控件的
// 矩形能解出来、不抛、非负"，menu_geometry_test 只覆盖语言列表。也就是说，把一个按钮
// 挪一像素在此之前是**不会有任何东西变红**的——而这条线的全部内容就是像素对齐。
//
// 所以下面断言的是**手算出来的字面量**，不是把 titleScreenLayout 的公式再抄一遍：
// 再抄一遍的测试只会证明代码等于它自己。数字的来源是 26.1 的源码：
//   TitleScreen.init      —— j = height/4 + 48；三个主按钮 spacing 24；随后 topPos += 36
//   LogoRenderer          —— logo (W/2-128, 30, 256x44)；edition (W/2-64, 30+44-7, 128x14)
// 其中图标行的 y 与 edition 的 y 与 GUI spec §6.3 的正文**不一致**（spec 写的是 1.20 的
// j+72 与 74），这里以源码为准，差异记在任务书的落地记录里。

#include "ui/HudLayout.hpp"
#include "ui/MenuGeometry.hpp"
#include "ui/PageBuilder.hpp"
#include "ui/TitleScreenLayout.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        std::printf("title_screen_layout_test line %d: %s\n", line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

void expectRect(const mc::ui::TitleRect& actual, int x, int y, int width, int height,
                const char* what, int line) {
    if (actual.x != x || actual.y != y || actual.width != width || actual.height != height) {
        std::printf("title_screen_layout_test line %d: %s is {%d,%d,%d,%d}, expected {%d,%d,%d,%d}\n",
                    line, what, actual.x, actual.y, actual.width, actual.height, x, y, width,
                    height);
        ++failures;
    }
}

#define EXPECT_RECT(actual, x, y, w, h) expectRect((actual), (x), (y), (w), (h), #actual, __LINE__)

// --- 1. 一个整除的画布：1280x720 @ GUI 缩放 2 -> 逻辑 640x360 -------------------
//
// j = 360/4 + 48 = 138，图标行 y = 138 + 48 + 36 = 222。
void testEvenCanvas() {
    const auto layout = mc::ui::titleScreenLayout(640, 360, /*versionTextWidth=*/70,
                                                  /*copyrightTextWidth=*/200);
    EXPECT_RECT(layout.logo, 192, 30, 256, 44);
    // edition 压在 logo 下沿上 7 像素：30 + 44 - 7 = 67。spec §6.3 写的 74 是 1.20 的值。
    EXPECT_RECT(layout.edition, 256, 67, 128, 14);
    EXPECT_RECT(layout.singleplayer, 220, 138, 200, 20);
    EXPECT_RECT(layout.multiplayer, 220, 162, 200, 20);
    EXPECT_RECT(layout.realms, 220, 186, 200, 20);
    // 图标行：j + 48 + 36 = 222。spec §6.3 写的 j + 72（也就是 210）是 1.20 的值。
    EXPECT_RECT(layout.language, 196, 222, 20, 20);
    EXPECT_RECT(layout.options, 220, 222, 98, 20);
    EXPECT_RECT(layout.quit, 322, 222, 98, 20);
    EXPECT_RECT(layout.accessibility, 424, 222, 20, 20);
    // 页脚：版本行贴左边 2 像素，版权行右对齐到 W - 宽 - 2；两行都在 H - 10。
    EXPECT_RECT(layout.version, 2, 350, 70, 10);
    EXPECT_RECT(layout.copyright, 640 - 200 - 2, 350, 200, 10);
}

// --- 2. 一个不整除的画布，专门咬整数除法 -------------------------------------
//
// 1280x720 @ GUI 缩放 3 -> 逻辑 ceil(1280/3) x ceil(720/3) = 427 x 240。
// j = 240/4 + 48 = 108；W/2 = 213（不是 213.5）。
// 谁把这两处整数除法写成浮点再取整，这一组就会红——而 spec §1.2 明确写了
// "所有居中都是整数运算，否则会与原版差 1px"。
void testOddCanvas() {
    const auto layout = mc::ui::titleScreenLayout(427, 240, 70, 200);
    EXPECT_RECT(layout.logo, 213 - 128, 30, 256, 44);
    EXPECT_RECT(layout.edition, 213 - 64, 67, 128, 14);
    EXPECT_RECT(layout.singleplayer, 113, 108, 200, 20);
    EXPECT_RECT(layout.multiplayer, 113, 132, 200, 20);
    EXPECT_RECT(layout.realms, 113, 156, 200, 20);
    EXPECT_RECT(layout.language, 89, 192, 20, 20);
    EXPECT_RECT(layout.options, 113, 192, 98, 20);
    EXPECT_RECT(layout.quit, 215, 192, 98, 20);
    EXPECT_RECT(layout.accessibility, 317, 192, 20, 20);
    EXPECT_RECT(layout.version, 2, 230, 70, 10);
    EXPECT_RECT(layout.copyright, 427 - 200 - 2, 230, 200, 10);

    // 高度取 241 时 241/4 仍是 60：一次真正的整数除法才有这个性质。
    const auto taller = mc::ui::titleScreenLayout(427, 241, 70, 200);
    CHECK(taller.singleplayer.y == layout.singleplayer.y);
    // 而 244/4 = 61，比 240/4 大 1：基线确实随高度走，只是走得不连续。
    const auto tallest = mc::ui::titleScreenLayout(427, 244, 70, 200);
    CHECK(tallest.singleplayer.y == layout.singleplayer.y + 1);
}

// --- 3. 版面内部的关系 --------------------------------------------------------
void testRelations() {
    const auto layout = mc::ui::titleScreenLayout(640, 360, 70, 200);
    // options 与 quit 是一对 98 宽的半钮，中间留 4 像素，合起来正好是一个 200 宽的按钮。
    CHECK(layout.quit.x - (layout.options.x + layout.options.width) == 4);
    CHECK(layout.options.x == layout.singleplayer.x);
    CHECK(layout.quit.x + layout.quit.width == layout.singleplayer.x + layout.singleplayer.width);
    // 两个图标钮各自贴在半钮排的外侧，不与它们重叠。
    CHECK(layout.language.x + layout.language.width <= layout.options.x);
    CHECK(layout.accessibility.x >= layout.quit.x + layout.quit.width);
    // 三个主按钮等距，间距 24 = 20 高 + 4 间隙（spec §5 范式 L1）。
    CHECK(layout.multiplayer.y - layout.singleplayer.y == 24);
    CHECK(layout.realms.y - layout.multiplayer.y == 24);
    // logo 与 edition 都水平居中于同一条中线。
    CHECK(layout.logo.x + layout.logo.width / 2 == layout.edition.x + layout.edition.width / 2);
    CHECK(layout.logo.x + layout.logo.width / 2 == 320);
}

// --- 4. 控件顺序 = 装配顺序 ---------------------------------------------------
//
// titleWidgetRect 的下标约定同时被 PageBuilder（装配）与 MenuInteraction（命中）使用。
// 顺序一旦和版面对不上，点"选项"就会触发"退出"，而两边都不会抛。
void testWidgetOrder() {
    const auto layout = mc::ui::titleScreenLayout(640, 360, 70, 200);
    CHECK(mc::ui::kTitleWidgetCount == 7U);
    CHECK(mc::ui::titleWidgetRect(layout, 0) == layout.singleplayer);
    CHECK(mc::ui::titleWidgetRect(layout, 1) == layout.multiplayer);
    CHECK(mc::ui::titleWidgetRect(layout, 2) == layout.realms);
    CHECK(mc::ui::titleWidgetRect(layout, 3) == layout.language);
    CHECK(mc::ui::titleWidgetRect(layout, 4) == layout.options);
    CHECK(mc::ui::titleWidgetRect(layout, 5) == layout.quit);
    CHECK(mc::ui::titleWidgetRect(layout, 6) == layout.accessibility);
    bool threw = false;
    try {
        static_cast<void>(mc::ui::titleWidgetRect(layout, mc::ui::kTitleWidgetCount));
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    // 页面上的控件数与版面里的控件数是同一个数。
    //
    // ★ 现在它是从**装配结果**数出来的：从前有一张 `menuButtonCount` 表另说一遍，
    //   而那两份没有任何东西保证一致。表已经删了，这条断言因此变成了真正的对账。
    mc::ui::MenuBuildContext ctx;
    const mc::ui::MenuCallbacks cb;
    mc::ui::Page title;
    mc::ui::buildPageInto(title, mc::ui::PageId::Title, ctx, cb);
    CHECK(mc::ui::countPageButtons(title) == mc::ui::kTitleWidgetCount);
    ctx.worldOpen = true;
    mc::ui::Page titleInWorld;
    mc::ui::buildPageInto(titleInWorld, mc::ui::PageId::Title, ctx, cb);
    CHECK(mc::ui::countPageButtons(titleInWorld) == mc::ui::kTitleWidgetCount);
}

// --- 5. 逻辑画布，以及它到帧缓冲像素的换算 ------------------------------------
//
// frontendButtonRect 是绘制与命中共用的那一份，主菜单在它里面走逻辑整数版面再乘 scale。
// 这一组同时钉住 HudLayout::logicalWidth/Height 的 ceil 语义。
void testFramebufferMapping() {
    const mc::ui::HudLayout twice{1280.0F, 720.0F, 2};
    CHECK(twice.scale() == 2.0F);
    CHECK(twice.logicalWidth() == 640);
    CHECK(twice.logicalHeight() == 360);
    const auto single = mc::ui::frontendButtonRect(twice, mc::ui::PageId::Title, 0U,
                                                   mc::ui::kTitleWidgetCount);
    CHECK(single.x == 440.0F);   // 220 * 2
    CHECK(single.y == 276.0F);   // 138 * 2
    CHECK(single.width == 400.0F);
    CHECK(single.height == 40.0F);

    // 非整除：1280/3 = 426.67 -> 逻辑宽 427，与 vanilla 的 ceil 一致。
    const mc::ui::HudLayout thrice{1280.0F, 720.0F, 3};
    CHECK(thrice.scale() == 3.0F);
    CHECK(thrice.logicalWidth() == 427);
    CHECK(thrice.logicalHeight() == 240);
    const auto quit = mc::ui::frontendButtonRect(thrice, mc::ui::PageId::Title, 5U,
                                                 mc::ui::kTitleWidgetCount);
    CHECK(quit.x == 645.0F);     // 215 * 3
    CHECK(quit.y == 576.0F);     // 192 * 3
    CHECK(quit.width == 294.0F); // 98 * 3

    // 同一个屏幕在两档缩放下是**不同的版面**，不是同一张图放大——截图通道至少拍两档
    // 就是为了这件事。两档下的单人按钮，其归一化位置并不相同。
    const auto singleAtThree = mc::ui::frontendButtonRect(thrice, mc::ui::PageId::Title, 0U,
                                                          mc::ui::kTitleWidgetCount);
    CHECK(std::fabs(single.y / 720.0F - singleAtThree.y / 720.0F) > 0.01F);
}

// --- 6. 主菜单不再走那个按钮数居中的通用求解器 --------------------------------
//
// menuButton 把整块按钮按数量垂直居中，那不是 vanilla 的 j = H/4 + 48。
// 这条断言存在的意义：谁把 Title 的分支删掉、让它掉回通用路径，这里会红。
void testTitleDoesNotUseCenteredStack() {
    const mc::ui::HudLayout layout{1280.0F, 720.0F, 2};
    const auto title = mc::ui::frontendButtonRect(layout, mc::ui::PageId::Title, 0U,
                                                  mc::ui::kTitleWidgetCount);
    const auto centred = layout.menuButton(0U, mc::ui::kTitleWidgetCount);
    CHECK(title.y != centred.y);
    // 暂停页仍走通用求解器，一像素不动（README 护栏第 4 条：动共用的那个会移动每一屏）。
    const auto pause = mc::ui::frontendButtonRect(layout, mc::ui::PageId::Pause, 0U, 3U);
    CHECK(pause.y == layout.menuButton(0U, 3U).y);
    CHECK(pause.x == layout.menuButton(0U, 3U).x);
}

} // namespace

int main() {
    testEvenCanvas();
    testOddCanvas();
    testRelations();
    testWidgetOrder();
    testFramebufferMapping();
    testTitleDoesNotUseCenteredStack();
    if (failures != 0) {
        std::printf("title_screen_layout_test: %d checks failed\n", failures);
        return 1;
    }
    return 0;
}

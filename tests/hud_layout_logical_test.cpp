// UI-3：版面跑在**逻辑画布的整数网格**上（GUI spec §1.1 + §1.2）。
//
// spec §1.2 把这条写成硬要求：所有"居中"都是整数运算，否则与原版差 1px。26.1 的版面
// 因此解在 `scaledWidth/Height = ceil(帧缓冲 / 缩放)` 这块画布上，用整数除法；绘制才乘
// 缩放回到帧缓冲像素（`GuiRenderer.java:203` 的正交投影用的是 `width / guiScale` 的
// **未取整**值——spec §1.1 那句 "ortho(0, scaledWidth, …)" 是错的，两者故意不一致）。
//
// 本仓此前一律在帧缓冲像素上用浮点算：`(帧缓冲宽 - 内容宽) * 0.5F`。整除的缩放档下两者
// 恰好相同，非整除档下就差一像素——而非整除是常态（1280/3 = 426.67）。
//
// 下面断言的是**手算的字面量**，不是把公式再抄一遍。最关键的一条：
//   1280x720 @ 缩放 3 → 逻辑 427x240 → 快捷栏 x = (427-182)/2 = 122 → 帧缓冲 366。
//   旧算法给的是 (1280 - 182*3)/2 = 367。

#include "ui/HudLayout.hpp"
#include "ui/MenuGeometry.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        std::printf("hud_layout_logical_test line %d: %s\n", line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

void expectRect(const mc::ui::UiRect& actual, float x, float y, float width, float height,
                const char* what, int line) {
    if (actual.x != x || actual.y != y || actual.width != width || actual.height != height) {
        std::printf("hud_layout_logical_test line %d: %s is {%g,%g,%g,%g}, expected {%g,%g,%g,%g}\n",
                    line, what, static_cast<double>(actual.x), static_cast<double>(actual.y),
                    static_cast<double>(actual.width), static_cast<double>(actual.height),
                    static_cast<double>(x), static_cast<double>(y), static_cast<double>(width),
                    static_cast<double>(height));
        ++failures;
    }
}

#define EXPECT_RECT(actual, x, y, w, h) expectRect((actual), (x), (y), (w), (h), #actual, __LINE__)

// --- 1. 逻辑画布本身 ---------------------------------------------------------
void testLogicalCanvas() {
    const mc::ui::HudLayout even{1280.0F, 720.0F, 2};
    CHECK(even.scale() == 2.0F);
    CHECK(even.logicalWidth() == 640);
    CHECK(even.logicalHeight() == 360);

    // 1280/3 = 426.67 -> ceil 427，与 26.1 `Window.setGuiScale` 的 "除不尽就 +1" 同义
    const mc::ui::HudLayout odd{1280.0F, 720.0F, 3};
    CHECK(odd.scale() == 3.0F);
    CHECK(odd.logicalWidth() == 427);
    CHECK(odd.logicalHeight() == 240);

    // 854x480 自动档解出缩放 2（854/3 = 284 < 320），逻辑画布与上面那档**相同**
    const mc::ui::HudLayout automatic{854.0F, 480.0F, 0};
    CHECK(automatic.scale() == 2.0F);
    CHECK(automatic.logicalWidth() == 427);
    CHECK(automatic.logicalHeight() == 240);
}

// --- 2. GUI 缩放求解，含 forceUnicode ----------------------------------------
void testGuiScale() {
    using mc::ui::HudLayout;
    CHECK(HudLayout::calculateGuiScale(1280, 720, 0, false) == 3);
    CHECK(HudLayout::calculateGuiScale(1280, 720, 2, false) == 2);
    // 请求超过上限就被上限夹住
    CHECK(HudLayout::calculateGuiScale(1280, 720, 9, false) == 3);
    CHECK(HudLayout::calculateGuiScale(640, 480, 0, false) == 2);
    CHECK(HudLayout::calculateGuiScale(320, 240, 0, false) == 1);

    // ★ forceUnicode 把奇数档抬成偶数（26.1 `Window.calculateScale:424-426`）。
    //   spec §1.1 说"26.1 已无此逻辑 [?]"——那是错的。
    //   注意它在夹紧**之后**发生，所以结果可以超过自动档的上限，这也是原版行为。
    CHECK(HudLayout::calculateGuiScale(1280, 720, 0, true) == 4);
    CHECK(HudLayout::calculateGuiScale(1280, 720, 3, true) == 4);
    CHECK(HudLayout::calculateGuiScale(1280, 720, 1, true) == 2);
    // 偶数档不动
    CHECK(HudLayout::calculateGuiScale(1280, 720, 2, true) == 2);
    CHECK(HudLayout::calculateGuiScale(640, 480, 0, true) == 2);
    // 极小画布的兜底：缩放不得超过帧缓冲本身
    CHECK(HudLayout::calculateGuiScale(1, 1, 0, false) == 1);
}

// --- 3. HUD 锚点：整除档 ------------------------------------------------------
//
// 1280x720 @ 缩放 2 -> 逻辑 640x360。这一档整除，整数与浮点结果**相同**——
// 也就是说本轮的改动在整除档下一像素都不该动。
void testHudAnchorsEvenScale() {
    const mc::ui::HudLayout layout{1280.0F, 720.0F, 2};
    // x = (640-182)/2 = 229 -> 458；y = 360-22 = 338 -> 676
    // ★ UI-6f（D5）：**贴底**，没有边距（26.1 `Gui.java:554` 的 `guiHeight() - 22`）。
    //   从前留 4 逻辑像素，那是自造值。
    EXPECT_RECT(layout.hotbarBackground(), 458.0F, 676.0F, 364.0F, 44.0F);
    // x = (640-176)/2 = 232 -> 464；y = (360-166)/2 = 97 -> 194
    EXPECT_RECT(layout.inventoryPanel(), 464.0F, 194.0F, 352.0F, 332.0F);
    // x = (640-195)/2 = 222 -> 444；y = (360-136)/2 = 112 -> 224
    EXPECT_RECT(layout.creativePanel(), 444.0F, 224.0F, 390.0F, 272.0F);
}

// --- 4. HUD 锚点：非整除档（本轮真正改变的那一组） ---------------------------
//
// 1280x720 @ 缩放 3 -> 逻辑 427x240。
void testHudAnchorsOddScale() {
    const mc::ui::HudLayout layout{1280.0F, 720.0F, 3};
    // ★ x = (427-182)/2 = 122 -> 366。旧算法：(1280-546)/2 = 367。
    //    y = 240-22 = 218 -> 654（D5：贴底，无边距）。
    EXPECT_RECT(layout.hotbarBackground(), 366.0F, 654.0F, 546.0F, 66.0F);
    // 槽位是从锚点派生的，因此自动落回整数网格
    EXPECT_RECT(layout.hotbarSlot(0), 366.0F + 9.0F, 654.0F + 9.0F, 48.0F, 48.0F);
    // x = (427-176)/2 = 125 -> 375；y = (240-166)/2 = 37 -> 111
    EXPECT_RECT(layout.inventoryPanel(), 375.0F, 111.0F, 528.0F, 498.0F);
    // 经验条贴在快捷栏上方 7 逻辑像素。★ 它**相对快捷栏**定位，所以 D5 那一个常量
    //   一改，它自动跟着下移——这正是当初就该这么写的理由。
    EXPECT_RECT(layout.experienceBar(), 366.0F, 654.0F - 21.0F, 546.0F, 15.0F);
    // 聊天输入：x = 2 -> 6；y = 240-14 = 226 -> 678；w = 427-4 = 423 -> 1269
    EXPECT_RECT(layout.chatInput(), 6.0F, 678.0F, 1269.0F, 36.0F);
}

// --- 5. 菜单按钮网格 ---------------------------------------------------------
void testMenuButtons() {
    const mc::ui::HudLayout layout{1280.0F, 720.0F, 3};   // 逻辑 427x240
    // menuButton(0,3): x = (427-200)/2 = 113 -> 339；y = 240/2 - 3*12 = 84 -> 252
    EXPECT_RECT(layout.menuButton(0U, 3U), 339.0F, 252.0F, 600.0F, 60.0F);
    EXPECT_RECT(layout.menuButton(2U, 3U), 339.0F, 252.0F + 144.0F, 600.0F, 60.0F);

    // bottomMenuButton(0,4,2): rows=2, maxWidth=(427-32-4)/2=195, width=195,
    // blockWidth=394, blockX=(427-394)/2=16, blockBottom=240-16=224,
    // blockTop=224-20-24=180
    EXPECT_RECT(layout.bottomMenuButton(0U, 4U, 2U), 48.0F, 540.0F, 585.0F, 60.0F);
    // 第二列：x = 16 + (195+4) = 215 -> 645
    EXPECT_RECT(layout.bottomMenuButton(2U, 4U, 2U), 645.0F, 540.0F, 585.0F, 60.0F);

    // videoSettingsButton(…,12): settingCount=11, rows=6, totalRows=7,
    // blockTop = 240/2 - 7*12 = 36
    EXPECT_RECT(layout.videoSettingsButton(0U, 12U), 48.0F, 108.0F, 585.0F, 60.0F);
    // 末位的"完成"独占一行并居中：x = (427-195)/2 = 116 -> 348；y = 36 + 6*24 = 180 -> 540
    EXPECT_RECT(layout.videoSettingsButton(11U, 12U), 348.0F, 540.0F, 585.0F, 60.0F);
}

// --- 6. 列表几何 -------------------------------------------------------------
void testListGeometry() {
    constexpr float kWidth = 1280.0F;
    const mc::ui::HudLayout layout{kWidth, 720.0F, 3};   // 逻辑 427x240
    // UI-4：行宽改成 26.1 的 270（`WorldSelectionList:251`），此前是自造的 300。
    // x = (427-270)/2 = 78 -> 234；行距 22 -> 66；行高 20 -> 60。
    EXPECT_RECT(mc::ui::worldListRow(0U, layout), 234.0F, 102.0F, 810.0F, 60.0F);
    EXPECT_RECT(mc::ui::worldListRow(1U, layout), 234.0F, 102.0F + 66.0F, 810.0F, 60.0F);

    // 整宽的列表框铺满**逻辑**画布，而不是帧缓冲宽度：427*3 = 1281，比 1280 多一像素。
    // 那一列被切掉正是原版行为（26.1 的版面就摆在 ceil 后的画布上）。
    const auto box = mc::ui::languageListBox(layout);
    CHECK(box.x == 0.0F);
    CHECK(box.width == 1281.0F);
    // 行在框内整数居中，且落在整数网格上
    const auto row = mc::ui::languageRow(0U, layout);
    CHECK(row.width == 810.0F);            // min(270, 427-32) = 270 -> 810
    CHECK(row.x == 0.0F + 234.0F);         // (427-270)/2 = 78 -> 234
    // UI-4：按键绑定行宽改成 26.1 的 340（`KeyBindsList:59`），此前是自造的 300。
    const auto controls = mc::ui::keyBindsRow(0U, layout);
    CHECK(controls.width == 1020.0F);      // 340 -> 1020
    CHECK(controls.x == 129.0F);           // (427-340)/2 = 43 -> 129
}

// --- 7. 一条通用性质：每个矩形都落在缩放的整数倍上 ---------------------------
//
// 这条比逐个字面量更宽：任何一处又退回浮点居中，它都会红，哪怕那个方法没被上面点名。
void testEverythingLandsOnTheGrid() {
    for (const auto canvas : {std::pair{1280.0F, 720.0F}, std::pair{854.0F, 480.0F},
                              std::pair{1281.0F, 721.0F}, std::pair{1024.0F, 600.0F}}) {
        for (int guiScale = 1; guiScale <= 4; ++guiScale) {
            const mc::ui::HudLayout layout{canvas.first, canvas.second, guiScale};
            const float scale = layout.scale();
            const mc::ui::UiRect rects[] = {
                layout.hotbarBackground(), layout.hotbarSlot(4U),  layout.hotbarSelection(0U),
                layout.experienceBar(),    layout.inventoryPanel(), layout.inventorySlot(0U),
                layout.creativePanel(),    layout.creativeSlot(0U), layout.chatInput(),
                layout.worldNameField(),   layout.menuButton(0U, 3U),
                layout.bottomMenuButton(0U, 4U, 2U), layout.videoSettingsButton(0U, 12U),
                layout.tableCraftingSlot(0U), layout.anvilLeftSlot(),
            };
            for (const auto& rect : rects) {
                // x / scale 必须是整数——浮点居中会在非整除档留下 .5
                CHECK(std::fmod(rect.x, scale) == 0.0F);
                CHECK(std::fmod(rect.y, scale) == 0.0F);
                CHECK(std::fmod(rect.width, scale) == 0.0F);
                CHECK(std::fmod(rect.height, scale) == 0.0F);
            }
        }
    }
}

} // namespace

int main() {
    testLogicalCanvas();
    testGuiScale();
    testHudAnchorsEvenScale();
    testHudAnchorsOddScale();
    testMenuButtons();
    testListGeometry();
    testEverythingLandsOnTheGrid();
    if (failures != 0) {
        std::printf("hud_layout_logical_test: %d checks failed\n", failures);
        return 1;
    }
    return 0;
}

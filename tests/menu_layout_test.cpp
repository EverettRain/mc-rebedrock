// PX-6 Bug1 regression: every front-end page must build and lay out without
// throwing. The crash was PageId::Controls building 24 fixed key-bind buttons
// through frontendButtonRect, which throws past the 20-button menu cap. This
// test rebuilds each page with the SAME rect-provider contract the renderer uses
// (list rows for the Controls key-binds, frontend buttons otherwise) and asserts
// every widget's rect resolves — the guard PX-5's stubbed rect provider lacked.

#include "input/InputNaming.hpp"
#include "ui/HudLayout.hpp"
#include "ui/KeyBindList.hpp"
#include "ui/ListRow.hpp"
#include "ui/MenuGeometry.hpp"
#include "ui/PageBuilder.hpp"
#include "ui/PageStack.hpp"

#include <cassert>
#include <cstddef>

using namespace mc;

namespace {

// ★ 不再镜像渲染器的 rect provider —— 直接调**生产**函数 `ui::menuWidgetRect`。
//
// 从前这里抄了一份渲染器 lambda 的等价物。UI-6b 把按键绑定行拆成两个控件时，
// 渲染器那边**有两份**同样的 lambda（绘制侧 buildDrawPage、输入侧 menuRectProvider），
// 只改了一份；这份抄本跟着改的是绘制侧，于是测试全绿而点 Controls 底部任何一个按钮
// 都会抛 `menu button index or count is invalid` 并闪退。
//
// 教训：**镜像不是覆盖**。测试要调被测的那个函数，不是它的另一份写法。
ui::RectProvider providerFor(ui::PageId page, const ui::HudLayout& layout, float fbWidth,
                             std::size_t count, std::size_t keyRows, std::size_t keyFirst = 0U) {
    return [layout, page, fbWidth, count, keyFirst, keyRows](std::size_t index) {
        return ui::menuWidgetRect(page, index, layout, fbWidth, count, keyFirst, keyRows);
    };
}

void buildAndLayoutPage(ui::PageId page, bool worldOpen, float fbW, float fbH, int guiScale) {
    const ui::HudLayout layout{fbW, fbH, guiScale};
    const std::size_t count = ui::menuButtonCount(page, worldOpen);

    ui::MenuBuildContext ctx;
    ctx.worldOpen = worldOpen;
    ctx.worldSelectable = true;
    ctx.keyBindLabelsFor = [](input::InputAction a) {
        return ui::MenuBuildContext::KeyBindRowLabels{
            std::string{input::actionDisplayName(a)}, {}};
    };
    std::size_t keyRows = 0U;
    if (page == ui::PageId::KeyBinds) {
        // UI-6c：窗口数的是行（含分类标题行），不是动作。
        const std::size_t total = ui::kKeyBindListRowCount;
        const std::size_t window =
            ui::keyBindsVisibleRowCount(fbW, fbH, guiScale, /*forceUnicode=*/false);
        keyRows = std::min(window, total);
        ctx.keyBindFirstIndex = 0U;
        ctx.keyBindRowCount = keyRows;
    }

    ui::MenuCallbacks cb;
    const ui::RectProvider rectFor = providerFor(page, layout, fbW, count, keyRows);
    const ui::Page built = ui::buildPage(page, ctx, cb, rectFor);

    // Every widget's rect must already be resolved (buildPage stamped it), and
    // re-resolving each index through the provider must not throw. Rects must be
    // finite and non-degenerate for interactive widgets.
    for (std::size_t i = 0; i < built.size(); ++i) {
        const ui::UiRect rect = rectFor(i);  // must not throw for any built index
        if (built[i].interactive()) {
            assert(rect.width > 0.0F && rect.height > 0.0F);
        }
        // buildPage stored the same rect on the widget.
        assert(built[i].rect.width == rect.width);
    }
}

// 每一页装配出的**按钮**数必须与 menuButtonCount 说的一致。
//
// 不一致就会在 frontendButtonRect 里抛 out_of_range 并闪退——那正是 Controls 底部
// 四个按钮遇到的事（列表后半段的序号被当成按钮序号）。这一条把它推广到每一页：
// 任何一页只要多装配一个按钮、或者 menuButtonCount 少算一个，这里就红。
void testEveryPageButtonBudget() {
    const ui::PageId pages[] = {
        ui::PageId::Title,     ui::PageId::WorldList,     ui::PageId::CreateWorld,
        ui::PageId::EditWorld, ui::PageId::ConfirmDelete, ui::PageId::Options,
        ui::PageId::VideoSettings, ui::PageId::Controls,  ui::PageId::Language,
        ui::PageId::Experimental,  ui::PageId::Pause,     ui::PageId::Death,
        // UI-6c：Controls 拆成了枢纽（§7.6）与绑定列表（§7.8）两屏，另加辅助功能（§7.11）
        ui::PageId::KeyBinds,      ui::PageId::Accessibility,
    };
    for (const bool worldOpen : {false, true}) {
        for (const ui::PageId page : pages) {
            const float fbW = 1280.0F;
            const float fbH = 720.0F;
            const int scale = 3;
            const ui::HudLayout layout{fbW, fbH, scale};
            const std::size_t count = ui::menuButtonCount(page, worldOpen);

            ui::MenuBuildContext ctx;
            ctx.worldOpen = worldOpen;
            ctx.worldSelectable = true;
            ctx.keyBindLabelsFor = [](input::InputAction a) {
                return ui::MenuBuildContext::KeyBindRowLabels{
                    std::string{input::actionDisplayName(a)}, {}};
            };
            std::size_t keyRows = 0U;
            if (page == ui::PageId::KeyBinds) {
                keyRows = std::min(
                    ui::keyBindsVisibleRowCount(fbW, fbH, scale, /*forceUnicode=*/false),
                    ui::kKeyBindListRowCount);
                ctx.keyBindFirstIndex = 0U;
                ctx.keyBindRowCount = keyRows;
            }
            ui::MenuCallbacks cb;
            const ui::Page built = ui::buildPage(
                page, ctx, cb, providerFor(page, layout, fbW, count, keyRows));

            // 列表控件之外的每一个控件都要落在按钮预算里。
            // ★ UI-6c：可见窗口里夹着分类标题行，它占一行却不产生控件——
            //   所以控件数要按**绑定行**数算，不是按行数。
            const std::size_t keyWidgets =
                ui::keyBindBindingRowsIn(0U, keyRows) * ui::kKeyBindWidgetsPerRow;
            const std::size_t buttons = built.size() - keyWidgets;
            assert(built.size() >= keyWidgets);
            assert(buttons <= count);
            // 而且**每一个**序号都要解得出矩形，不抛。这是闪退的直接复现条件：
            // 输入侧解第 keyWidgets 个序号时越界。
            for (std::size_t i = 0; i < built.size(); ++i) {
                const ui::UiRect rect =
                    ui::menuWidgetRect(page, i, layout, fbW, count, 0U, keyRows);
                if (built[i].interactive()) {
                    assert(rect.width > 0.0F && rect.height > 0.0F);
                }
            }
        }
    }
}

void testEveryPageLaysOut() {
    const ui::PageId pages[] = {
        ui::PageId::Title,     ui::PageId::WorldList,     ui::PageId::CreateWorld,
        ui::PageId::EditWorld, ui::PageId::ConfirmDelete, ui::PageId::Options,
        ui::PageId::VideoSettings, ui::PageId::Controls,  ui::PageId::Language,
        ui::PageId::Experimental,  ui::PageId::Pause,     ui::PageId::Death,
        // UI-6c：Controls 拆成了枢纽（§7.6）与绑定列表（§7.8）两屏，另加辅助功能（§7.11）
        ui::PageId::KeyBinds,      ui::PageId::Accessibility,
    };
    // A spread of canvas sizes and GUI scales, so the Controls visible-row window
    // varies (a small canvas fits fewer rows — the scroll window must still bound
    // the built widget count).
    const struct {
        float w;
        float h;
        int scale;
    } canvases[] = {{1920.0F, 1080.0F, 3}, {1280.0F, 720.0F, 2}, {854.0F, 480.0F, 1}};

    for (const auto& canvas : canvases) {
        for (const ui::PageId page : pages) {
            buildAndLayoutPage(page, /*worldOpen=*/false, canvas.w, canvas.h, canvas.scale);
            buildAndLayoutPage(page, /*worldOpen=*/true, canvas.w, canvas.h, canvas.scale);
        }
    }
}

// Neither of the two screens Controls was split into may exceed the button cap.
//
// UI-6c: Controls is now 26.1's §7.6 hub -- one jump button, seven options, Done --
// and the key binds live on their own screen (§7.8) whose footer is two buttons.
// The list rows go to the list, not the button grid: the full action set is larger
// than the cap, which is what made them impossible as fixed buttons (PX-6 Bug1).
void testControlsBottomBandBounded() {
    // 一个跳转（Key Binds…）+ 七个设置项 + Done。少的那个跳转是 Mouse Settings…：
    // 本作没有那一屏，而"页面为空就完全不建"。
    assert(ui::menuButtonCount(ui::PageId::Controls, false) == 9U);
    assert(ui::menuButtonCount(ui::PageId::Controls, false) <=
           ui::HudLayout::kMaximumMenuButtons);
    // 绑定列表页的页脚是横排两个：`controls.resetAll` 与 Done。
    assert(ui::menuButtonCount(ui::PageId::KeyBinds, false) == 2U);
    assert(input::keyBindRows().size() > ui::HudLayout::kMaximumMenuButtons);
}

// The Controls key-bind list is windowed: the built row count never exceeds the
// visible window even though there are 24 actions.
void testControlsListWindowed() {
    const float fbW = 854.0F;
    const float fbH = 480.0F;
    const int scale = 1;
    const std::size_t window = ui::keyBindsVisibleRowCount(fbW, fbH, scale, /*forceUnicode=*/false);
    ui::MenuBuildContext ctx;
    ctx.keyBindFirstIndex = 0U;
    ctx.keyBindRowCount = std::min(window, ui::kKeyBindListRowCount);
    ctx.keyBindLabelsFor = [](input::InputAction a) {
        return ui::MenuBuildContext::KeyBindRowLabels{
            std::string{input::actionDisplayName(a)}, {}};
    };
    ui::MenuCallbacks cb;
    const ui::HudLayout layout{fbW, fbH, scale};
    const ui::Page page = ui::buildPage(
        ui::PageId::KeyBinds, ctx, cb,
        providerFor(ui::PageId::KeyBinds, layout, fbW,
                    ui::menuButtonCount(ui::PageId::KeyBinds, false), ctx.keyBindRowCount));
    // UI-6c：一行是**三个**控件——名称 Label、改键 Button、重置 Button。
    // 前两个带 KeyBindRow 这个 debugId，重置按钮带它自己的 ResetKeyBind
    // （它有自己的标签 `controls.reset`，与页脚那个"重置所有"不是一回事）。
    std::size_t names = 0;
    std::size_t changes = 0;
    std::size_t resets = 0;
    for (const auto& w : page) {
        if (w.debugId == static_cast<std::uint16_t>(ui::WidgetId::KeyBindRow)) {
            if (w.kind == ui::WidgetKind::Label) ++names;
            if (w.kind == ui::WidgetKind::Button) ++changes;
        }
        if (w.debugId == static_cast<std::uint16_t>(ui::WidgetId::ResetKeyBind)) {
            ++resets;
        }
    }
    // ★ UI-6c：窗口里的行不全是绑定行——分类标题行占一行却不产生控件。
    //   所以控件数对的是**绑定行**数，不是行数。拿行数比会多出标题行那么多，
    //   而那个差正好等于这一屏里跨了几个分类。
    const std::size_t bindingRows = ui::keyBindBindingRowsIn(0U, ctx.keyBindRowCount);
    assert(bindingRows < ctx.keyBindRowCount);   // 这一屏确实跨了至少一个分类边界
    // 每一个绑定行恰好一个名称、一个改键、一个重置——少一个就是某一行缺了一块，
    // 而画面上只表现为"有一行没有键名"或"有一行没有重置按钮"。
    assert(names == bindingRows);
    assert(changes == bindingRows);
    assert(resets == bindingRows);
    assert(names + changes + resets == bindingRows * ui::kKeyBindWidgetsPerRow);
    assert(ctx.keyBindRowCount <= window);
}

// PX-6 Bug1 (round 2): the visible key-bind rows must land in the MIDDLE band —
// between the top (title) and the bottom button band — and shift by exactly one
// row step when the scroll offset advances. This guards the "middle band
// invisible / rows off-screen" regression at the geometry level (the draw skip
// that hid them is a renderer concern, verified on mac).
void testControlsRowsInMiddleBandAndScroll() {
    // A canvas where the list scrolls (more actions than fit).
    const float fbW = 1920.0F;
    const float fbH = 1080.0F;
    const int scale = 3;
    const ui::HudLayout layout{fbW, fbH, scale};
    const std::size_t window = ui::keyBindsVisibleRowCount(fbW, fbH, scale, /*forceUnicode=*/false);
    const std::size_t total = input::keyBindRows().size();
    assert(window < total);  // this canvas genuinely scrolls

    const ui::UiRect box = ui::keyBindsListBox(layout, fbW);
    const ui::UiRect band =
        layout.bottomMenuButton(0U, ui::menuButtonCount(ui::PageId::KeyBinds, false), 2U);
    // Every visible row sits inside the middle band: below the box top, and its
    // bottom stays above the bottom button band.
    for (std::size_t i = 0; i < window; ++i) {
        const ui::UiRect row = ui::keyBindsRow(i, layout, fbW);
        assert(row.y >= box.y - 0.01F);
        assert(row.y + row.height <= band.y + 0.01F);
        assert(row.width > 0.0F && row.height > 0.0F);
    }

    // Scrolling by one advances the first visible action, and the row at visible
    // index 0 keeps the SAME screen rect (the window slides over the data, the
    // slots stay put) — while the ACTION shown there is the next one.
    const ui::UiRect firstSlot = ui::keyBindsRow(0U, layout, fbW);
    // The rect for visible slot 0 is offset-independent (it is the top slot).
    // What changes is which action maps to it, which the page builder handles via
    // keyBindFirstIndex: build at offset 0 and offset 1 and compare row 0's action.
    const auto rows = input::keyBindRows();
    ui::MenuBuildContext ctx0;
    ctx0.keyBindFirstIndex = 0U;
    ctx0.keyBindRowCount = window;
    ctx0.keyBindLabelsFor = [](input::InputAction a) {
        return ui::MenuBuildContext::KeyBindRowLabels{
            std::string{input::actionDisplayName(a)}, {}};
    };
    ui::MenuBuildContext ctx1 = ctx0;
    ctx1.keyBindFirstIndex = 1U;
    ui::MenuCallbacks cb;
    // ★ provider 的起始行必须与 ctx 的 keyBindFirstIndex **一致**：控件序号折回哪一行
    //   取决于起点之后有几条标题行。两者不一致，控件数就对不上，多出来的序号会被当成
    //   页脚按钮而越界——生产代码里两侧都读 menuSystem.controlsListFirstIndex，所以
    //   一致是天然的；测试里是手传的，这条注释就是提醒。
    const auto keyButtons = ui::menuButtonCount(ui::PageId::KeyBinds, false);
    const ui::Page p0 = ui::buildPage(
        ui::PageId::KeyBinds, ctx0, cb,
        providerFor(ui::PageId::KeyBinds, layout, fbW, keyButtons, window, ctx0.keyBindFirstIndex));
    const ui::Page p1 = ui::buildPage(
        ui::PageId::KeyBinds, ctx1, cb,
        providerFor(ui::PageId::KeyBinds, layout, fbW, keyButtons, window, ctx1.keyBindFirstIndex));
    // Slot 0 keeps its rect; the action shown there advances by one.
    //
    // UI-6b: slot 0 is now the row's NAME label, whose rect is the row's content
    // box centred vertically -- so it is compared against keyBindsNameCell, not
    // against the row itself. Both cells must still sit inside that row: a cell
    // that drifted out of its row would still look like a list, just a misaligned
    // one, and nothing else here would notice.
    // ★ UI-6c：展开后的行表以一条**分类标题行**开头，而标题行不产生控件——
    //   所以从行 0 起的第一个控件落在**可见行 1**，不是行 0。这正是
    //   keyBindWidgetVisibleRow 存在的理由：照 index/3 算，标题行之后的每一行都会
    //   偏上一格，而画面上只表现为"名字和按钮错位了一行"。
    assert(ui::keyBindListRow(0U).isCategory);
    const std::size_t firstBindingRow =
        ui::keyBindWidgetVisibleRow(0U, 0U, ui::kKeyBindWidgetsPerRow);
    assert(firstBindingRow == 1U);
    const ui::UiRect nameSlot = ui::keyBindsNameCell(firstBindingRow, layout, fbW);
    const ui::UiRect changeSlot = ui::keyBindsChangeCell(firstBindingRow, layout, fbW);
    const ui::UiRect bindingRowRect = ui::keyBindsRow(firstBindingRow, layout, fbW);
    assert(p0[0].rect.y == nameSlot.y);
    assert(p0[1].rect.y == changeSlot.y);
    // 两个格子都落在它们那一行之内：漂出去仍然像个列表，只是对不齐，别处没人会发现。
    assert(nameSlot.y >= bindingRowRect.y);
    assert(nameSlot.y + nameSlot.height <= bindingRowRect.y + bindingRowRect.height);
    assert(changeSlot.y >= bindingRowRect.y);
    assert(changeSlot.y + changeSlot.height <= bindingRowRect.y + bindingRowRect.height);
    // The name is left of the change button and they do not overlap.
    assert(nameSlot.x < changeSlot.x);
    // 行 0 是标题、行 1 是第一个动作；从行 1 起，第一个控件就是那个动作本身，
    // 而且它现在落在**可见行 0**（标题已经滚出去了）。
    assert(ui::keyBindListRow(1U).action == rows[0]);
    assert(p0[0].label == std::string{input::actionDisplayName(rows[0])});
    assert(p1[0].label == std::string{input::actionDisplayName(rows[0])});
    assert(ui::keyBindWidgetVisibleRow(1U, 0U, ui::kKeyBindWidgetsPerRow) == 0U);
    assert(p1[0].rect.y == ui::keyBindsNameCell(0U, layout, fbW).y);
    static_cast<void>(firstSlot);
}

}  // namespace

int main() {
    testEveryPageLaysOut();
    testEveryPageButtonBudget();
    testControlsBottomBandBounded();
    testControlsListWindowed();
    testControlsRowsInMiddleBandAndScroll();
    return 0;
}

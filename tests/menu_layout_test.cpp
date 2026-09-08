// PX-6 Bug1 regression: every front-end page must build and lay out without
// throwing. The crash was PageId::Controls building 24 fixed key-bind buttons
// through frontendButtonRect, which throws past the 20-button menu cap. This
// test rebuilds each page with the SAME rect-provider contract the renderer uses
// (list rows for the Controls key-binds, frontend buttons otherwise) and asserts
// every widget's rect resolves — the guard PX-5's stubbed rect provider lacked.

#include "input/InputNaming.hpp"
#include "ui/HudLayout.hpp"
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
                             std::size_t count, std::size_t keyRows) {
    return [layout, page, fbWidth, count, keyRows](std::size_t index) {
        return ui::menuWidgetRect(page, index, layout, fbWidth, count, keyRows);
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
    if (page == ui::PageId::Controls) {
        const std::size_t total = input::keyBindRows().size();
        const std::size_t window =
            ui::controlsVisibleRowCount(fbW, fbH, guiScale, /*forceUnicode=*/false);
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
            if (page == ui::PageId::Controls) {
                keyRows = std::min(
                    ui::controlsVisibleRowCount(fbW, fbH, scale, /*forceUnicode=*/false),
                    input::keyBindRows().size());
                ctx.keyBindFirstIndex = 0U;
                ctx.keyBindRowCount = keyRows;
            }
            ui::MenuCallbacks cb;
            const ui::Page built = ui::buildPage(
                page, ctx, cb, providerFor(page, layout, fbW, count, keyRows));

            // 列表控件之外的每一个控件都要落在按钮预算里。
            const std::size_t keyWidgets = keyRows * ui::kKeyBindWidgetsPerRow;
            const std::size_t buttons = built.size() - keyWidgets;
            assert(built.size() >= keyWidgets);
            assert(buttons <= count);
            // 而且**每一个**序号都要解得出矩形，不抛。这是闪退的直接复现条件：
            // 输入侧解第 keyWidgets 个序号时越界。
            for (std::size_t i = 0; i < built.size(); ++i) {
                const ui::UiRect rect =
                    ui::menuWidgetRect(page, i, layout, fbW, count, keyRows);
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

// The Controls page must never exceed the button cap: its bottom band is a fixed
// four buttons, and the key-bind rows go to the list, not the button grid.
void testControlsBottomBandBounded() {
    assert(ui::menuButtonCount(ui::PageId::Controls, false) == 4U);
    assert(ui::menuButtonCount(ui::PageId::Controls, false) <=
           ui::HudLayout::kMaximumMenuButtons);
    // The full action set is larger than the button cap — proving they cannot be
    // fixed buttons (the original crash).
    assert(input::keyBindRows().size() > ui::HudLayout::kMaximumMenuButtons);
}

// The Controls key-bind list is windowed: the built row count never exceeds the
// visible window even though there are 24 actions.
void testControlsListWindowed() {
    const float fbW = 854.0F;
    const float fbH = 480.0F;
    const int scale = 1;
    const std::size_t window = ui::controlsVisibleRowCount(fbW, fbH, scale, /*forceUnicode=*/false);
    ui::MenuBuildContext ctx;
    ctx.keyBindFirstIndex = 0U;
    ctx.keyBindRowCount = std::min(window, input::keyBindRows().size());
    ctx.keyBindLabelsFor = [](input::InputAction a) {
        return ui::MenuBuildContext::KeyBindRowLabels{
            std::string{input::actionDisplayName(a)}, {}};
    };
    ui::MenuCallbacks cb;
    const ui::HudLayout layout{fbW, fbH, scale};
    const ui::Page page = ui::buildPage(
        ui::PageId::Controls, ctx, cb,
        providerFor(ui::PageId::Controls, layout, fbW, 4U, ctx.keyBindRowCount));
    // UI-6b：一行是两个控件（名称 Label + 改键 Button），所以带 KeyBindRow 这个
    // debugId 的控件数是行数的两倍。窗口本身仍然是行数。
    std::size_t widgets = 0;
    std::size_t labels = 0;
    std::size_t buttons = 0;
    for (const auto& w : page) {
        if (w.debugId != static_cast<std::uint16_t>(ui::WidgetId::KeyBindRow)) {
            continue;
        }
        ++widgets;
        if (w.kind == ui::WidgetKind::Label) ++labels;
        if (w.kind == ui::WidgetKind::Button) ++buttons;
    }
    assert(widgets == ctx.keyBindRowCount * ui::kKeyBindWidgetsPerRow);
    // 每一行恰好一个名称、一个按钮——少一个就是某一行缺了半边，而画面上只表现为
    // "有一行没有键名"或"有一行没有名字"。
    assert(labels == ctx.keyBindRowCount);
    assert(buttons == ctx.keyBindRowCount);
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
    const std::size_t window = ui::controlsVisibleRowCount(fbW, fbH, scale, /*forceUnicode=*/false);
    const std::size_t total = input::keyBindRows().size();
    assert(window < total);  // this canvas genuinely scrolls

    const ui::UiRect box = ui::controlsListBox(layout, fbW);
    const ui::UiRect band = layout.bottomMenuButton(0U, 4U, 2U);
    // Every visible row sits inside the middle band: below the box top, and its
    // bottom stays above the bottom button band.
    for (std::size_t i = 0; i < window; ++i) {
        const ui::UiRect row = ui::controlsRow(i, layout, fbW);
        assert(row.y >= box.y - 0.01F);
        assert(row.y + row.height <= band.y + 0.01F);
        assert(row.width > 0.0F && row.height > 0.0F);
    }

    // Scrolling by one advances the first visible action, and the row at visible
    // index 0 keeps the SAME screen rect (the window slides over the data, the
    // slots stay put) — while the ACTION shown there is the next one.
    const ui::UiRect firstSlot = ui::controlsRow(0U, layout, fbW);
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
    const ui::Page p0 = ui::buildPage(ui::PageId::Controls, ctx0, cb,
                                      providerFor(ui::PageId::Controls, layout, fbW, 4U, window));
    const ui::Page p1 = ui::buildPage(ui::PageId::Controls, ctx1, cb,
                                      providerFor(ui::PageId::Controls, layout, fbW, 4U, window));
    // Slot 0 keeps its rect; the action shown there advances by one.
    //
    // UI-6b: slot 0 is now the row's NAME label, whose rect is the row's content
    // box centred vertically -- so it is compared against controlsNameCell, not
    // against the row itself. Both cells must still sit inside that row: a cell
    // that drifted out of its row would still look like a list, just a misaligned
    // one, and nothing else here would notice.
    const ui::UiRect nameSlot = ui::controlsNameCell(0U, layout, fbW);
    const ui::UiRect changeSlot = ui::controlsChangeCell(0U, layout, fbW);
    assert(p0[0].rect.y == nameSlot.y);
    assert(p1[0].rect.y == nameSlot.y);
    assert(p0[1].rect.y == changeSlot.y);
    assert(nameSlot.y >= firstSlot.y);
    assert(nameSlot.y + nameSlot.height <= firstSlot.y + firstSlot.height);
    assert(changeSlot.y >= firstSlot.y);
    assert(changeSlot.y + changeSlot.height <= firstSlot.y + firstSlot.height);
    // The name is left of the change button and they do not overlap.
    assert(nameSlot.x < changeSlot.x);
    assert(p0[0].label == std::string{input::actionDisplayName(rows[0])});
    assert(p1[0].label == std::string{input::actionDisplayName(rows[1])});
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

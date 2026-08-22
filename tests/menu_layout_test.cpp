// PX-6 Bug1 regression: every front-end page must build and lay out without
// throwing. The crash was PageId::Controls building 24 fixed key-bind buttons
// through frontendButtonRect, which throws past the 20-button menu cap. This
// test rebuilds each page with the SAME rect-provider contract the renderer uses
// (list rows for the Controls key-binds, frontend buttons otherwise) and asserts
// every widget's rect resolves — the guard PX-5's stubbed rect provider lacked.

#include "input/InputNaming.hpp"
#include "ui/HudLayout.hpp"
#include "ui/MenuGeometry.hpp"
#include "ui/PageBuilder.hpp"
#include "ui/PageStack.hpp"

#include <cassert>
#include <cstddef>

using namespace mc;

namespace {

// Mirror the renderer's menuRectProvider: on Controls the first `keyRows` indices
// are scrolling list rows, the rest are bottom buttons; every other page's
// widgets are all frontend buttons. Building this for real (not a stub) is what
// exercises the layout capacity that threw.
ui::RectProvider providerFor(ui::PageId page, const ui::HudLayout& layout, float fbWidth,
                             std::size_t count, std::size_t keyRows) {
    return [layout, page, fbWidth, count, keyRows](std::size_t index) {
        if (page == ui::PageId::Controls && index < keyRows) {
            return ui::controlsRow(index, layout, fbWidth);
        }
        const std::size_t buttonIndex = page == ui::PageId::Controls ? index - keyRows : index;
        return ui::frontendButtonRect(layout, page, buttonIndex, count);
    };
}

void buildAndLayoutPage(ui::PageId page, bool worldOpen, float fbW, float fbH, int guiScale) {
    const ui::HudLayout layout{fbW, fbH, guiScale};
    const std::size_t count = ui::menuButtonCount(page, worldOpen);

    ui::MenuBuildContext ctx;
    ctx.worldOpen = worldOpen;
    ctx.worldSelectable = true;
    ctx.keyBindLabelFor = [](input::InputAction a) {
        return std::string{input::actionDisplayName(a)};
    };
    std::size_t keyRows = 0U;
    if (page == ui::PageId::Controls) {
        const std::size_t total = input::keyBindRows().size();
        const std::size_t window = ui::controlsVisibleRowCount(fbW, fbH, guiScale);
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
    const std::size_t window = ui::controlsVisibleRowCount(fbW, fbH, scale);
    ui::MenuBuildContext ctx;
    ctx.keyBindFirstIndex = 0U;
    ctx.keyBindRowCount = std::min(window, input::keyBindRows().size());
    ctx.keyBindLabelFor = [](input::InputAction a) {
        return std::string{input::actionDisplayName(a)};
    };
    ui::MenuCallbacks cb;
    const ui::HudLayout layout{fbW, fbH, scale};
    const ui::Page page = ui::buildPage(
        ui::PageId::Controls, ctx, cb,
        providerFor(ui::PageId::Controls, layout, fbW, 4U, ctx.keyBindRowCount));
    std::size_t rows = 0;
    for (const auto& w : page) {
        if (w.debugId == static_cast<std::uint16_t>(ui::WidgetId::KeyBindRow)) ++rows;
    }
    assert(rows == ctx.keyBindRowCount);
    assert(rows <= window);
}

}  // namespace

int main() {
    testEveryPageLaysOut();
    testControlsBottomBandBounded();
    testControlsListWindowed();
    return 0;
}

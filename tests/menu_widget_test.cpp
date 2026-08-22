// PX-4: the data-driven menu model's pure core. A page is a vector<Widget> built
// in one place (buildPage); a click resolves to a widget by geometry and fires
// that widget's callback (dispatchActivate/clickAt); a disabled widget fires
// nothing; a slider drags through its callback, never a traversal side effect.
// This is exactly what the old MenuButton enum + per-page array + switch could
// NOT be tested for (it was welded to Vulkan) — here it is headless.

#include "ui/MenuInteraction.hpp"
#include "ui/PageBuilder.hpp"
#include "ui/PageStack.hpp"
#include "ui/Widget.hpp"

#include <cassert>
#include <cstddef>
#include <string>

using namespace mc;
namespace ui = mc::ui;

namespace {

// A trivial row layout: widget i occupies a 100x20 box stacked down the y axis,
// so a click at (x, i*20 + 10) lands squarely on widget i. Independent of the
// real HudLayout (that is renderer-side); the model/dispatch are what we test.
ui::RectProvider rowLayout() {
    return [](std::size_t index) {
        return ui::UiRect{0.0F, static_cast<float>(index) * 20.0F, 100.0F, 20.0F};
    };
}

// The centre of widget i under rowLayout.
float rowCenterY(std::size_t index) { return static_cast<float>(index) * 20.0F + 10.0F; }

// Find a widget's index on a page by its debugId, or npos.
std::size_t indexOfId(const ui::Page& page, ui::WidgetId id) {
    for (std::size_t i = 0; i < page.size(); ++i) {
        if (page[i].debugId == static_cast<std::uint16_t>(id)) {
            return i;
        }
    }
    return ui::kNoWidget;
}

// --- Page assembly: each page has the historic widgets, in order --------------
void testPageAssembly() {
    ui::MenuBuildContext ctx;
    ui::MenuCallbacks cb;

    const ui::Page title = ui::buildPage(ui::PageId::Title, ctx, cb, rowLayout());
    assert(title.size() == 3);
    assert(title[0].debugId == static_cast<std::uint16_t>(ui::WidgetId::Singleplayer));
    assert(title[2].debugId == static_cast<std::uint16_t>(ui::WidgetId::Exit));

    const ui::Page pause = ui::buildPage(ui::PageId::Pause, ctx, cb, rowLayout());
    assert(pause.size() == 3);
    assert(pause[0].debugId == static_cast<std::uint16_t>(ui::WidgetId::Resume));

    // Options gains the Difficulty button only when a world is open.
    ctx.worldOpen = false;
    const ui::Page optsNoWorld = ui::buildPage(ui::PageId::Options, ctx, cb, rowLayout());
    assert(indexOfId(optsNoWorld, ui::WidgetId::Difficulty) == ui::kNoWidget);
    ctx.worldOpen = true;
    const ui::Page optsWorld = ui::buildPage(ui::PageId::Options, ctx, cb, rowLayout());
    assert(indexOfId(optsWorld, ui::WidgetId::Difficulty) != ui::kNoWidget);
    assert(optsWorld.size() == optsNoWorld.size() + 1);

    // Video settings carries its two sliders as Slider widgets, not buttons.
    const ui::Page video = ui::buildPage(ui::PageId::VideoSettings, ctx, cb, rowLayout());
    const std::size_t vd = indexOfId(video, ui::WidgetId::ViewDistance);
    const std::size_t sd = indexOfId(video, ui::WidgetId::SimulationDistance);
    assert(vd != ui::kNoWidget && video[vd].kind == ui::WidgetKind::Slider);
    assert(sd != ui::kNoWidget && video[sd].kind == ui::WidgetKind::Slider);

    // The world list builds one ListRow per row plus the four action buttons.
    ctx.worldRowCount = 3;
    const ui::Page worlds = ui::buildPage(ui::PageId::WorldList, ctx, cb, rowLayout());
    std::size_t rows = 0;
    for (const auto& w : worlds) {
        if (w.kind == ui::WidgetKind::ListRow) ++rows;
    }
    assert(rows == 3);
    assert(indexOfId(worlds, ui::WidgetId::PlaySelected) != ui::kNoWidget);
    assert(indexOfId(worlds, ui::WidgetId::Back) != ui::kNoWidget);
}

// --- Click dispatch: clicking a widget fires its callback (the switch replacement)
void testClickDispatch() {
    ui::MenuBuildContext ctx;
    ui::MenuCallbacks cb;
    bool resumed = false;
    bool optionsOpened = false;
    bool quit = false;
    cb.resume = [&] { resumed = true; };
    cb.openOptions = [&] { optionsOpened = true; };
    cb.saveAndQuit = [&] { quit = true; };

    ui::Page pause = ui::buildPage(ui::PageId::Pause, ctx, cb, rowLayout());
    // Click "Options" (index 1): only openOptions fires.
    const std::size_t fired = ui::clickAt(pause, 50.0F, rowCenterY(1));
    assert(fired == 1);
    assert(optionsOpened);
    assert(!resumed && !quit);

    // Click "Resume" (index 0).
    optionsOpened = false;
    const std::size_t fired0 = ui::clickAt(pause, 50.0F, rowCenterY(0));
    assert(fired0 == 0 && resumed && !optionsOpened);

    // A click in empty space (below the last widget) fires nothing.
    const std::size_t none = ui::clickAt(pause, 50.0F, rowCenterY(99));
    assert(none == ui::kNoWidget);
}

// --- Disabled widget: hit but never activated ---------------------------------
void testDisabledWidgetNoFire() {
    ui::Page page;
    bool fired = false;
    ui::Widget w;
    w.kind = ui::WidgetKind::Button;
    w.rect = ui::UiRect{0.0F, 0.0F, 100.0F, 20.0F};
    w.enabled = false;
    w.onActivate = [&] { fired = true; };
    page.push_back(std::move(w));

    // The disabled widget is still under the pointer (hitTest finds it) ...
    assert(ui::hitTest(page, 50.0F, 10.0F) == 0);
    // ... but clickAt refuses to activate it, and the callback never runs.
    assert(ui::clickAt(page, 50.0F, 10.0F) == ui::kNoWidget);
    assert(!fired);
    // dispatchActivate directly also refuses (matches ButtonControl.enabled).
    assert(ui::dispatchActivate(page, 0, 50.0F, 10.0F) == ui::kNoWidget);
    assert(!fired);
}

// --- Press/release over different widgets does not activate --------------------
void testPressReleaseMismatch() {
    ui::MenuBuildContext ctx;
    ui::MenuCallbacks cb;
    bool resumed = false;
    cb.resume = [&] { resumed = true; };
    ui::Page pause = ui::buildPage(ui::PageId::Pause, ctx, cb, rowLayout());
    // Press on Resume (0) but release over Options (1): nothing fires.
    const std::size_t fired = ui::dispatchActivate(pause, 0, 50.0F, rowCenterY(1));
    assert(fired == ui::kNoWidget);
    assert(!resumed);
}

// --- Slider: acts through its drag callback, never through activation ----------
void testSliderThroughCallback() {
    ui::MenuBuildContext ctx;
    ui::MenuCallbacks cb;
    float applied = -1.0F;
    bool committed = false;
    cb.masterVolume.value = [] { return 0.5F; };
    cb.masterVolume.onDrag = [&](float f) { applied = f; };
    cb.masterVolume.onCommit = [&] { committed = true; };

    ui::Page opts = ui::buildPage(ui::PageId::Options, ctx, cb, rowLayout());
    const std::size_t volIndex = indexOfId(opts, ui::WidgetId::MasterVolume);
    assert(volIndex != ui::kNoWidget && opts[volIndex].kind == ui::WidgetKind::Slider);

    // A slider is NOT activated by a click (drags act through onDrag).
    const std::size_t clicked = ui::dispatchActivate(opts, volIndex, 50.0F, rowCenterY(volIndex));
    assert(clicked == ui::kNoWidget);

    // beginSliderDrag applies the fraction through the callback — the effect lives
    // in the callback, not in the traversal.
    const std::size_t dragged =
        ui::beginSliderDrag(opts, 50.0F, rowCenterY(volIndex), 0.75F);
    assert(dragged == volIndex);
    assert(applied == 0.75F);
    // Commit is the caller's release step.
    opts[volIndex].slider.onCommit();
    assert(committed);
}

// --- "Add a button = one line": Title has exactly its three, no drawing/dispatch
// knowledge needed elsewhere. This pins that the page shape is the single source
// (the property the migration buys). --------------------------------------------
void testSingleAssemblyPoint() {
    ui::MenuBuildContext ctx;
    ui::MenuCallbacks cb;
    int count = 0;
    cb.exitGame = [&] { ++count; };
    ui::Page title = ui::buildPage(ui::PageId::Title, ctx, cb, rowLayout());
    // Exit is the third title widget; clicking it runs exactly the injected action.
    const std::size_t exitIndex = indexOfId(title, ui::WidgetId::Exit);
    assert(exitIndex != ui::kNoWidget);
    const std::size_t fired = ui::clickAt(title, 50.0F, rowCenterY(exitIndex));
    assert(fired == exitIndex);
    assert(count == 1);
}

}  // namespace

// --- PX-6 Bug3: the Options page carries a Subtitles toggle wired to its cb ----
void testOptionsHasSubtitlesToggle() {
    ui::MenuBuildContext ctx;
    ui::MenuCallbacks cb;
    bool toggled = false;
    cb.toggleSubtitles = [&] { toggled = true; };
    ui::Page opts = ui::buildPage(ui::PageId::Options, ctx, cb, rowLayout());

    std::size_t subIndex = ui::kNoWidget;
    for (std::size_t i = 0; i < opts.size(); ++i) {
        if (opts[i].debugId == static_cast<std::uint16_t>(ui::WidgetId::Subtitles)) {
            subIndex = i;
        }
    }
    assert(subIndex != ui::kNoWidget);
    // Clicking the Subtitles row fires exactly its toggle callback.
    const float rowY = opts[subIndex].rect.y + opts[subIndex].rect.height * 0.5F;
    const std::size_t fired = ui::clickAt(opts, 50.0F, rowY);
    assert(fired == subIndex);
    assert(toggled);
}

int main() {
    testPageAssembly();
    testClickDispatch();
    testDisabledWidgetNoFire();
    testPressReleaseMismatch();
    testSliderThroughCallback();
    testSingleAssemblyPoint();
    testOptionsHasSubtitlesToggle();
    return 0;
}

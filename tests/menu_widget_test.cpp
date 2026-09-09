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

// --- UI-4: keyboard focus traversal (GUI spec §1.4) --------------------------
//
// Tab must skip what cannot be activated: Labels and Panels are not interactive,
// and a DISABLED widget is not focusable either — vanilla's Tab walks past greyed
// buttons, and landing on one would put Enter on something that does nothing.
void testFocusTraversal() {
    ui::MenuBuildContext ctx;
    ui::MenuCallbacks cb;
    // The title page is the useful fixture: seven widgets of which three are
    // disabled (multiplayer / realms / accessibility have no target screen yet).
    const ui::Page title = ui::buildPage(ui::PageId::Title, ctx, cb, rowLayout());
    assert(title.size() == 7);

    // Forward from nothing lands on the first focusable widget.
    const std::size_t first = ui::nextFocus(title, ui::kNoWidget, /*forward=*/true);
    assert(first == 0);                              // singleplayer
    assert(ui::nextFocus(title, 0, true) == 3);      // skips the two disabled ones
    assert(ui::nextFocus(title, 3, true) == 4);      // language -> options
    assert(ui::nextFocus(title, 4, true) == 5);      // options -> quit
    assert(ui::nextFocus(title, 5, true) == 0);      // quit wraps past accessibility

    // Backward from nothing lands on the last focusable widget, and reverses.
    assert(ui::nextFocus(title, ui::kNoWidget, /*forward=*/false) == 5);
    assert(ui::nextFocus(title, 0, false) == 5);
    assert(ui::nextFocus(title, 3, false) == 0);

    // A page with nothing focusable returns kNoWidget rather than looping.
    ui::Page inert;
    ui::Widget label;
    label.kind = ui::WidgetKind::Label;
    inert.push_back(label);
    ui::Widget dead;
    dead.kind = ui::WidgetKind::Button;
    dead.enabled = false;
    inert.push_back(dead);
    assert(ui::nextFocus(inert, ui::kNoWidget, true) == ui::kNoWidget);
    assert(ui::nextFocus(inert, 0, true) == ui::kNoWidget);
    assert(ui::nextFocus(ui::Page{}, ui::kNoWidget, true) == ui::kNoWidget);
}

// Enter / Space fires the focused widget's own callback — the same callback the
// mouse fires, so "what the keyboard can do" is always a subset of "what the
// mouse can do" rather than a second path that drifts.
void testFocusActivation() {
    ui::MenuBuildContext ctx;
    ui::MenuCallbacks cb;
    int singleplayer = 0;
    int quit = 0;
    cb.openSingleplayer = [&] { ++singleplayer; };
    cb.exitGame = [&] { ++quit; };
    const ui::Page title = ui::buildPage(ui::PageId::Title, ctx, cb, rowLayout());

    assert(ui::activateFocused(title, 0));
    assert(singleplayer == 1 && quit == 0);
    assert(ui::activateFocused(title, 5));
    assert(quit == 1);
    // A disabled widget fires nothing, and neither does "no focus".
    assert(!ui::activateFocused(title, 1));
    assert(!ui::activateFocused(title, ui::kNoWidget));
    assert(!ui::activateFocused(title, 99));
    assert(singleplayer == 1 && quit == 1);

    // A slider is a drag control: Enter must not "activate" it (dispatchActivate
    // has the same rule for the mouse).
    ui::Page sliders;
    ui::Widget slider;
    slider.kind = ui::WidgetKind::Slider;
    int sliderFired = 0;
    slider.onActivate = [&] { ++sliderFired; };
    sliders.push_back(slider);
    assert(!ui::activateFocused(sliders, 0));
    assert(sliderFired == 0);
}

// UI-4: the two title-screen icon buttons are IconButtons, not Buttons — the
// draw side picks their icon by kind, and they carry no label.
void testIconButtons() {
    ui::MenuBuildContext ctx;
    ui::MenuCallbacks cb;
    const ui::Page title = ui::buildPage(ui::PageId::Title, ctx, cb, rowLayout());
    assert(title[3].kind == ui::WidgetKind::IconButton);
    assert(title[6].kind == ui::WidgetKind::IconButton);
    assert(title[3].label.empty());
    assert(title[6].label.empty());
    // Still interactive and still hit-testable, exactly like a Button.
    assert(title[3].interactive());
    assert(title[0].kind == ui::WidgetKind::Button);

    // 图标在钮内居中。★ **整数除法的顺序照抄 vanilla，不能代数化简**：
    //   26.1 `SpriteIconButton.CenteredIcon`（:132-133）算的是
    //       getWidth()/2 - spriteWidth/2  = 20/2 - 15/2 = 10 - 7 = 3
    //   而不是 (20 - 15)/2 = 5/2 = 2。两个式子在实数上相等，整数除法下差 1。
    //
    // ★ 这段断言从前写的是 2，还配着一句"这正是 vanilla 的整数除法做的，断言完美对称
    //   等于断言 bug"——**方向说反了**，vanilla 偏的是右下不是左上。测试是照着代码写的，
    //   于是把偏差钉死了；现场报告"图标偏左上"才把它翻出来。
    //   与 UI-6b 那次滚动条 x 完全同形：**照代码写的断言不是覆盖，是固化。**
    const ui::UiRect button{100.0F, 200.0F, 40.0F, 40.0F};   // 20x20 at scale 2
    const auto icon = ui::iconButtonIconRect(button, 2.0F);
    assert(icon.x == 106.0F);   // 100 + 3 逻辑 * scale 2
    assert(icon.y == 206.0F);
    assert(icon.width == 30.0F);
    assert(icon.height == 30.0F);
    // 20 - 15 = 5 是奇数，所以两侧不可能相等：vanilla 那条式子给出前缘 3、后缘 2。
    const float leadingGap = icon.x - button.x;
    const float trailingGap = (button.x + button.width) - (icon.x + icon.width);
    assert(leadingGap == 6.0F);       // 3 logical * scale 2
    assert(trailingGap == 4.0F);      // 2 logical * scale 2
    assert(leadingGap > trailingGap); // 偏**右下**，不是左上
    assert(icon.y - button.y == leadingGap);
    // And it stays inside the button at every scale.
    for (float scale = 1.0F; scale <= 4.0F; scale += 1.0F) {
        const ui::UiRect box{0.0F, 0.0F, ui::kIconButtonSize * scale,
                             ui::kIconButtonSize * scale};
        const auto inner = ui::iconButtonIconRect(box, scale);
        assert(inner.x >= box.x && inner.y >= box.y);
        assert(inner.x + inner.width <= box.x + box.width);
        assert(inner.y + inner.height <= box.y + box.height);
    }
}

// --- Page assembly: each page has the historic widgets, in order --------------
void testPageAssembly() {
    ui::MenuBuildContext ctx;
    ui::MenuCallbacks cb;

    // UI-2: the title screen is 26.1's seven widgets now, in TitleScreen.init's
    // order. The order is not cosmetic — titleWidgetRect indexes the layout by it,
    // so a reordering here aims the clicks at the wrong buttons.
    const ui::Page title = ui::buildPage(ui::PageId::Title, ctx, cb, rowLayout());
    assert(title.size() == 7);
    assert(title[0].debugId == static_cast<std::uint16_t>(ui::WidgetId::Singleplayer));
    assert(title[1].debugId == static_cast<std::uint16_t>(ui::WidgetId::Multiplayer));
    assert(title[2].debugId == static_cast<std::uint16_t>(ui::WidgetId::Realms));
    assert(title[3].debugId == static_cast<std::uint16_t>(ui::WidgetId::TitleLanguage));
    assert(title[4].debugId == static_cast<std::uint16_t>(ui::WidgetId::Options));
    assert(title[5].debugId == static_cast<std::uint16_t>(ui::WidgetId::Exit));
    assert(title[6].debugId == static_cast<std::uint16_t>(ui::WidgetId::TitleAccessibility));
    // The three whose target screens this build does not have yet are present and
    // greyed, exactly as vanilla greys multiplayer and realms when multiplayer is
    // not allowed — not invented, and not silently missing from the layout.
    assert(!title[1].enabled);
    assert(!title[2].enabled);
    assert(!title[6].enabled);
    assert(title[0].enabled && title[3].enabled && title[4].enabled && title[5].enabled);

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
    // ★ UI-6e ④：项数**恒定**，不再是"世界内多一项"。26.1 `OptionsScreen.init()`
    //   那一格是 `inWorld ? Difficulty : Online` —— **二选一**，不是可有可无。
    //   照抄这个结构之后行数恒定 6、正好装满内容区，版面不会因为开没开世界而变。
    assert(optsWorld.size() == optsNoWorld.size());
    // 世界外那一格是置灰的 Online（本作没有多人）
    const std::size_t onlineIndex = indexOfId(optsNoWorld, ui::WidgetId::OnlineOptions);
    assert(onlineIndex != ui::kNoWidget);
    assert(!optsNoWorld[onlineIndex].enabled);
    // 世界内它换成 Difficulty，而 Online 不再出现——两者占**同一格**
    assert(indexOfId(optsWorld, ui::WidgetId::OnlineOptions) == ui::kNoWidget);
    assert(indexOfId(optsWorld, ui::WidgetId::Difficulty) == onlineIndex);

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

    // ★ UI-6e：主音量滑块**从 Options 挪到了"音乐与声音"那一屏**，与 26.1 一致
    //   （`OptionsScreen` 上只有一个跳转，没有音量滑块）。
    cb.floatSliderFor = [&](ui::WidgetId id) {
        ui::SliderBind bind;
        if (id != ui::WidgetId::MasterVolume) {
            return bind;
        }
        bind.value = [] { return 0.5F; };
        bind.onDrag = [&](float f) { applied = f; };
        bind.onCommit = [&] { committed = true; };
        return bind;
    };
    ui::Page opts = ui::buildPage(ui::PageId::SoundSettings, ctx, cb, rowLayout());
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

// --- PX-6 Bug3: the Subtitles toggle exists and is wired to its callback -------
//
// UI-6c: it moved from Options to the new Accessibility page. That is where 26.1
// keeps it (`AccessibilityOptionsScreen.java:25`, `options.showSubtitles()`), and
// where View Bobbing went too (deviation D2). The toggle itself is unchanged --
// this test still guards PX-6 Bug3, it just looks on the screen that now owns it.
void testAccessibilityHasSubtitlesToggle() {
    ui::MenuBuildContext ctx;
    ui::MenuCallbacks cb;
    ui::WidgetId toggled = ui::WidgetId::None;
    cb.cycleOption = [&](ui::WidgetId id, int) { toggled = id; };
    ui::Page page = ui::buildPage(ui::PageId::Accessibility, ctx, cb, rowLayout());

    std::size_t subIndex = ui::kNoWidget;
    std::size_t bobIndex = ui::kNoWidget;
    for (std::size_t i = 0; i < page.size(); ++i) {
        if (page[i].debugId == static_cast<std::uint16_t>(ui::WidgetId::Subtitles)) {
            subIndex = i;
        }
        if (page[i].debugId == static_cast<std::uint16_t>(ui::WidgetId::ViewBobbing)) {
            bobIndex = i;
        }
    }
    assert(subIndex != ui::kNoWidget);
    // D2: View Bobbing belongs here, not on Controls.
    assert(bobIndex != ui::kNoWidget);
    // Clicking the Subtitles row fires exactly its toggle callback.
    const float rowY = page[subIndex].rect.y + page[subIndex].rect.height * 0.5F;
    const std::size_t fired = ui::clickAt(page, 50.0F, rowY);
    assert(fired == subIndex);
    assert(toggled == ui::WidgetId::Subtitles);

    // ...and Options no longer carries it: two screens both showing the same
    // toggle would be two places to change it and one of them would drift.
    const ui::Page opts = ui::buildPage(ui::PageId::Options, ctx, cb, rowLayout());
    for (const auto& w : opts) {
        assert(w.debugId != static_cast<std::uint16_t>(ui::WidgetId::Subtitles));
    }
    // Options gained the entry that leads here instead.
    bool hasEntry = false;
    for (const auto& w : opts) {
        hasEntry = hasEntry || w.debugId == static_cast<std::uint16_t>(ui::WidgetId::Accessibility);
    }
    assert(hasEntry);
}

int main() {
    testPageAssembly();
    testFocusTraversal();
    testFocusActivation();
    testIconButtons();
    testClickDispatch();
    testDisabledWidgetNoFire();
    testPressReleaseMismatch();
    testSliderThroughCallback();
    testSingleAssemblyPoint();
    testAccessibilityHasSubtitlesToggle();
    return 0;
}

#pragma once

// PX-4: the ONE place a menu page is assembled. buildPage(PageId, ctx, cb, rect)
// returns the page's ui::Page (a vector<Widget>) with each widget's kind, label,
// enabled flag, debugId and callback wired. This replaces both the per-page
// constexpr MenuButton arrays (menuButtonForIndex) and the switch(MenuButton)
// dispatch: the widget order IS the layout order, and each widget owns the action
// the switch used to run.
//
// Vulkan-free and testable: the renderer supplies
//   - MenuCallbacks: every action as a std::function (capturing Vulkan/save/audio)
//   - RectProvider:  index -> UiRect, from the renderer's HudLayout
//   - MenuBuildContext: the read-only flags the page shape depends on
// so a headless test builds a page with stub callbacks + a trivial row layout and
// asserts that clicking widget N fires callback N. ui:: never touches Vulkan.

#include "ui/PageStack.hpp"
#include "ui/WidgetId.hpp"
#include "input/InputAction.hpp"
#include "input/InputNaming.hpp"
#include "ui/Widget.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace mc::ui {

// index -> the widget's screen rect, from the caller's layout. The builder asks
// for rects by the widget's ordinal on the page, matching frontendButtonRect's
// existing index contract.
using RectProvider = std::function<UiRect(std::size_t index)>;

// The read-only facts a page's shape depends on. Kept tiny and value-only so a
// test can set them directly.
struct MenuBuildContext final {
    bool worldOpen = false;  // a save is loaded: Options gains Difficulty, etc.
    // At least one save exists: the WorldList's Play/Edit buttons are enabled only
    // then (empty save list greys them, and a click must not fire). Draw and
    // dispatch read the same flag, so a disabled button neither paints active nor
    // activates.
    bool worldSelectable = false;
    // Label text the builder stamps onto option buttons (already localized +
    // value-formatted by the renderer). Empty strings are fine for tests.
    std::function<std::string(std::uint16_t debugId)> labelFor{};
    // Row counts for the scrolling lists (worlds / languages). The renderer folds
    // scroll offset + visible window into these before building.
    std::size_t worldRowCount = 0;
    std::size_t languageRowCount = 0;
    // PX-5 Key Binds: the label for an action's row ("Forward: W", or "Forward:
    // > ? <" while that row is capturing). The renderer builds it from the
    // InputSystem single source; a test can stub it. When set, the Controls page
    // renders the key-bind table instead of the legacy toggle scaffold.
    std::function<std::string(input::InputAction action)> keyBindLabelFor{};
    // PX-6 Bug1: the Controls key-bind list is SCROLLING. Only the visible window
    // is built (like the world/language lists), so the widget count stays bounded
    // regardless of how many actions exist. keyBindFirstIndex is the scroll
    // offset into input::keyBindRows(); keyBindRowCount is the visible window.
    std::size_t keyBindFirstIndex = 0;
    std::size_t keyBindRowCount = 0;
};

// Every menu action the pages can fire, as injectable callbacks. The renderer
// fills these from its member functions (setPaused, startWorld, cycleResolution,
// ...). Any left null is simply a no-op when its widget is clicked (a test can
// leave the ones it does not exercise unset).
struct MenuCallbacks final {
    // Title / world flow
    std::function<void()> openSingleplayer{};
    std::function<void()> exitGame{};
    std::function<void()> playSelectedWorld{};
    std::function<void()> createWorld{};
    std::function<void()> editWorld{};
    std::function<void()> confirmCreate{};
    std::function<void()> toggleCreateGameMode{};
    std::function<void()> toggleCreateAllowCommands{};
    std::function<void()> renameWorld{};
    std::function<void()> deleteWorld{};
    std::function<void()> confirmDelete{};
    std::function<void()> cancelDelete{};
    std::function<void(std::size_t rowIndex)> selectWorldRow{};

    // Pause / death
    std::function<void()> resume{};
    std::function<void()> saveAndQuit{};
    std::function<void()> respawn{};
    std::function<void()> returnToTitle{};

    // Options navigation
    std::function<void()> openOptions{};
    std::function<void()> openVideoSettings{};
    std::function<void()> openControls{};
    std::function<void()> openLanguage{};
    std::function<void()> openExperimental{};
    std::function<void()> doneOptions{};   // pop the current options sub-page
    std::function<void()> back{};          // generic page pop

    // Video / gameplay toggles + cycles
    std::function<void()> cycleResolution{};
    std::function<void()> cycleGuiScale{};
    // Every option that steps through a fixed list of values goes through this
    // one callback, keyed on the widget's id: the values, the field and the label
    // all come from ui::OptionCycle's table, so a new option is a table row plus
    // the addOptionButton line that places it — never another callback here.
    // `direction` is +1 for the next value and -1 for the previous, so a two-way
    // selector control needs no new plumbing.
    std::function<void(WidgetId id, int direction)> cycleOption{};
    std::function<void()> cycleDifficulty{};

    // Experimental page

    // Language list row select (draft selection, committed on Done)
    std::function<void(std::size_t rowIndex)> selectLanguageRow{};

    // PX-5 Key Binds: clicking an action's row begins capturing its next key
    // (KeyBindingScreen::beginCapture); Reset restores the vanilla defaults. Both
    // act on the PX-1 InputSystem single source through the renderer's closures.
    std::function<void(input::InputAction action)> beginKeyCapture{};
    std::function<void()> resetKeyBinds{};

    // Sliders: value getters + drag/commit appliers (fraction in [0,1]).
    SliderBind viewDistance{};
    SliderBind simulationDistance{};
    SliderBind masterVolume{};
};

namespace detail {

[[nodiscard]] inline std::string label(const MenuBuildContext& ctx, WidgetId id) {
    return ctx.labelFor ? ctx.labelFor(static_cast<std::uint16_t>(id)) : std::string{};
}

// Append a plain button widget whose rect is the next ordinal from the provider.
inline void addButton(Page& page, const RectProvider& rectFor, const MenuBuildContext& ctx,
                      WidgetId id, std::function<void()> onActivate, bool enabled = true) {
    Widget w;
    w.kind = WidgetKind::Button;
    w.debugId = static_cast<std::uint16_t>(id);
    w.rect = rectFor ? rectFor(page.size()) : UiRect{};
    w.label = label(ctx, id);
    w.enabled = enabled;
    w.onActivate = std::move(onActivate);
    page.push_back(std::move(w));
}

// A cycling option's button. Its action is always the same generic step, keyed
// on the id — the values, the field and the label are the table's business
// (ui/OptionCycle.hpp), never this call site's.
inline void addOptionButton(Page& page, const RectProvider& rectFor, const MenuBuildContext& ctx,
                            WidgetId id, const MenuCallbacks& cb) {
    // The callback is COPIED into the widget, like every other action: a Page
    // outlives the MenuCallbacks reference buildPage was handed.
    addButton(page, rectFor, ctx, id, [cycle = cb.cycleOption, id] {
        if (cycle) {
            cycle(id, /*direction=*/1);
        }
    });
}

inline void addSlider(Page& page, const RectProvider& rectFor, const MenuBuildContext& ctx,
                      WidgetId id, SliderBind bind) {
    Widget w;
    w.kind = WidgetKind::Slider;
    w.debugId = static_cast<std::uint16_t>(id);
    w.rect = rectFor ? rectFor(page.size()) : UiRect{};
    w.label = label(ctx, id);
    w.slider = std::move(bind);
    page.push_back(std::move(w));
}

inline void addListRow(Page& page, const RectProvider& rectFor, WidgetId id, std::size_t rowIndex,
                       std::function<void()> onActivate) {
    Widget w;
    w.kind = WidgetKind::ListRow;
    w.debugId = static_cast<std::uint16_t>(id);
    w.rect = rectFor ? rectFor(page.size()) : UiRect{};
    w.onActivate = std::move(onActivate);
    static_cast<void>(rowIndex);
    page.push_back(std::move(w));
}

// A key-bind row: an "Action: Key" ListRow whose click begins the rebind capture
// for that action. The label comes from the InputSystem single source via the
// context's keyBindLabelFor. `enabled` is always true (any row is rebindable).
inline void addKeyBindRow(Page& page, const RectProvider& rectFor, const MenuBuildContext& ctx,
                          input::InputAction action, std::function<void()> onActivate) {
    Widget w;
    w.kind = WidgetKind::ListRow;
    w.debugId = static_cast<std::uint16_t>(WidgetId::KeyBindRow);
    w.rect = rectFor ? rectFor(page.size()) : UiRect{};
    w.label = ctx.keyBindLabelFor ? ctx.keyBindLabelFor(action) : std::string{};
    w.onActivate = std::move(onActivate);
    page.push_back(std::move(w));
}

}  // namespace detail

// Build the page for `id`. The widget order matches the historic per-page array;
// each callback runs exactly what the old switch case did.
[[nodiscard]] inline Page buildPage(PageId id, const MenuBuildContext& ctx,
                                    const MenuCallbacks& cb, const RectProvider& rectFor) {
    using detail::addButton;
    using detail::addOptionButton;
    using detail::addListRow;
    using detail::addSlider;
    Page page;

    switch (id) {
        case PageId::Title:
            addButton(page, rectFor, ctx, WidgetId::Singleplayer, cb.openSingleplayer);
            addButton(page, rectFor, ctx, WidgetId::Options, cb.openOptions);
            addButton(page, rectFor, ctx, WidgetId::Exit, cb.exitGame);
            break;

        case PageId::Pause:
            addButton(page, rectFor, ctx, WidgetId::Resume, cb.resume);
            addButton(page, rectFor, ctx, WidgetId::Options, cb.openOptions);
            addButton(page, rectFor, ctx, WidgetId::SaveQuit, cb.saveAndQuit);
            break;

        case PageId::Death:
            addButton(page, rectFor, ctx, WidgetId::Respawn, cb.respawn);
            addButton(page, rectFor, ctx, WidgetId::TitleScreen, cb.returnToTitle);
            break;

        case PageId::WorldList:
            // The scrolling save rows come first (list body), then the four action
            // buttons in the historic order Play/Create/Edit/Back.
            for (std::size_t row = 0; row < ctx.worldRowCount; ++row) {
                addListRow(page, rectFor, WidgetId::WorldRow, row,
                           [cb, row]() { if (cb.selectWorldRow) cb.selectWorldRow(row); });
            }
            addButton(page, rectFor, ctx, WidgetId::PlaySelected, cb.playSelectedWorld,
                      ctx.worldSelectable);
            addButton(page, rectFor, ctx, WidgetId::CreateWorld, cb.createWorld);
            addButton(page, rectFor, ctx, WidgetId::Edit, cb.editWorld, ctx.worldSelectable);
            addButton(page, rectFor, ctx, WidgetId::Back, cb.back);
            break;

        case PageId::CreateWorld:
            addButton(page, rectFor, ctx, WidgetId::CreateGameMode, cb.toggleCreateGameMode);
            addButton(page, rectFor, ctx, WidgetId::CreateAllowCommands,
                      cb.toggleCreateAllowCommands);
            addButton(page, rectFor, ctx, WidgetId::CreateConfirm, cb.confirmCreate);
            addButton(page, rectFor, ctx, WidgetId::Back, cb.back);
            break;

        case PageId::EditWorld:
            addButton(page, rectFor, ctx, WidgetId::SaveRename, cb.renameWorld);
            addButton(page, rectFor, ctx, WidgetId::DeleteWorld, cb.deleteWorld);
            addButton(page, rectFor, ctx, WidgetId::Back, cb.back);
            break;

        case PageId::ConfirmDelete:
            addButton(page, rectFor, ctx, WidgetId::DeleteConfirm, cb.confirmDelete);
            addButton(page, rectFor, ctx, WidgetId::DeleteCancel, cb.cancelDelete);
            break;

        case PageId::Options:
            addSlider(page, rectFor, ctx, WidgetId::MasterVolume, cb.masterVolume);
            if (ctx.worldOpen) {
                addButton(page, rectFor, ctx, WidgetId::Difficulty, cb.cycleDifficulty);
            }
            addButton(page, rectFor, ctx, WidgetId::Controls, cb.openControls);
            addButton(page, rectFor, ctx, WidgetId::VideoSettings, cb.openVideoSettings);
            addOptionButton(page, rectFor, ctx, WidgetId::Subtitles, cb);
            addButton(page, rectFor, ctx, WidgetId::Language, cb.openLanguage);
            addButton(page, rectFor, ctx, WidgetId::Experimental, cb.openExperimental);
            addButton(page, rectFor, ctx, WidgetId::Done, cb.doneOptions);
            break;

        case PageId::VideoSettings:
            addButton(page, rectFor, ctx, WidgetId::Resolution, cb.cycleResolution);
            addButton(page, rectFor, ctx, WidgetId::GuiScale, cb.cycleGuiScale);
            addSlider(page, rectFor, ctx, WidgetId::ViewDistance, cb.viewDistance);
            addSlider(page, rectFor, ctx, WidgetId::SimulationDistance, cb.simulationDistance);
            addOptionButton(page, rectFor, ctx, WidgetId::FrameRateLimit, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::AntiAliasing, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::Anisotropy, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::SmoothLighting, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::DynamicLight, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::Vsync, cb);
            addButton(page, rectFor, ctx, WidgetId::Done, cb.doneOptions);
            break;

        case PageId::Controls: {
            // PX-6 Bug1: the key-bind rows are a SCROLLING list — only the visible
            // window [keyBindFirstIndex, +keyBindRowCount) is built, so the page
            // never exceeds the layout capacity (24 fixed buttons would throw).
            // Each row's rect comes from rectFor (the renderer maps these list
            // indices to controlsRow rects); the trailing four are bottom buttons.
            // PX-5: each row begins its rebind capture on click, label from the
            // InputSystem single source.
            constexpr auto rows = input::keyBindRows();
            const std::size_t first = std::min(ctx.keyBindFirstIndex, rows.size());
            const std::size_t last = std::min(first + ctx.keyBindRowCount, rows.size());
            for (std::size_t i = first; i < last; ++i) {
                const input::InputAction action = rows[i];
                detail::addKeyBindRow(page, rectFor, ctx, action, [cb, action]() {
                    if (cb.beginKeyCapture) cb.beginKeyCapture(action);
                });
            }
            addOptionButton(page, rectFor, ctx, WidgetId::ViewBobbing, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::AutoJump, cb);
            addButton(page, rectFor, ctx, WidgetId::ResetKeyBinds, cb.resetKeyBinds);
            addButton(page, rectFor, ctx, WidgetId::Done, cb.doneOptions);
            break;
        }

        case PageId::Language:
            for (std::size_t row = 0; row < ctx.languageRowCount; ++row) {
                addListRow(page, rectFor, WidgetId::LanguageRow, row,
                           [cb, row]() { if (cb.selectLanguageRow) cb.selectLanguageRow(row); });
            }
            addOptionButton(page, rectFor, ctx, WidgetId::ForceUnicodeFont, cb);
            addButton(page, rectFor, ctx, WidgetId::Done, cb.doneOptions);
            break;

        case PageId::Experimental:
            addOptionButton(page, rectFor, ctx, WidgetId::RainMode, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::ParticleLevel, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::SunShadows, cb);
            addOptionButton(page, rectFor, ctx, WidgetId::RainCollisionCache, cb);
            addButton(page, rectFor, ctx, WidgetId::Back, cb.back);
            break;

        case PageId::Loading:
        case PageId::Game:
            break;  // no menu widgets (in-world HUD / loading are not menu pages)
    }
    return page;
}

}  // namespace mc::ui

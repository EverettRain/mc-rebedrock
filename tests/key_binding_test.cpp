// PX-5: the Key Binds screen. Rebinding writes through the PX-1 InputSystem — the
// SINGLE SOURCE of controls — so a rebind actually changes the game's input, with
// no private page-local copy. Covers: begin capture -> press key -> binding table
// updated; conflict detection; reset to defaults; capture toggle/cancel; and the
// Controls page listing every rebindable action as a clickable row that begins
// its capture. All headless: no GLFW, no Vulkan.

#include "input/InputNaming.hpp"
#include "input/InputSystem.hpp"
#include "input/KeyBindingScreen.hpp"
#include "ui/MenuInteraction.hpp"
#include "ui/PageBuilder.hpp"

#include <cassert>
#include <cstddef>
#include <string>

using namespace mc;
using mc::input::InputAction;
using mc::input::Key;

namespace {

// --- Single source: a rebind lands in the InputSystem's own table -------------
void testRebindWritesThroughSingleSource() {
    input::InputSystem system;
    input::KeyBindingScreen screen{system};

    // Forward starts on W (the default).
    assert(system.bindings().binding(InputAction::MoveForward) == input::keyboard(Key::W));

    // Click the Forward row -> capturing; press T -> Forward rebinds to T IN THE
    // INPUTSYSTEM, so the live input reads T from now on.
    screen.beginCapture(InputAction::MoveForward);
    assert(screen.capturing() && screen.capturingAction() == InputAction::MoveForward);
    const auto result = screen.applyKey(Key::T);
    assert(result.applied);
    assert(!screen.capturing());  // capture ended
    assert(system.bindings().binding(InputAction::MoveForward) == input::keyboard(Key::T));

    // Prove it is the SAME source the poll reads: a frame with T down now yields
    // forward movement, and W no longer does.
    input::InputSystem::EventQueue queue;
    input::RawInputFrame frame;
    frame.setKey(Key::T, true);
    const auto intentT = system.poll(frame, queue);
    assert(intentT.forward > 0.5F);

    input::RawInputFrame frameW;
    frameW.setKey(Key::W, true);
    const auto intentW = system.poll(frameW, queue);
    assert(intentW.forward == 0.0F);  // W no longer bound to forward
}

// --- Conflict: rebinding onto a control another action owns is flagged ---------
void testConflictDetection() {
    input::InputSystem system;
    input::KeyBindingScreen screen{system};

    // Rebind Jump (default Space) onto E, which Inventory already owns.
    screen.beginCapture(InputAction::Jump);
    const auto result = screen.applyKey(Key::E);
    assert(result.applied);
    assert(result.conflict);
    assert(result.conflictingAction == InputAction::Inventory);
    // It still applied to the single source (vanilla rebinds and warns).
    assert(system.bindings().binding(InputAction::Jump) == input::keyboard(Key::E));

    // Rebinding onto a free key reports no conflict.
    screen.beginCapture(InputAction::Jump);
    const auto free = screen.applyKey(Key::Space);  // Space is now unused (Jump left it)
    assert(free.applied && !free.conflict);
}

// --- Reset restores the vanilla defaults through the source --------------------
void testResetToDefaults() {
    input::InputSystem system;
    input::KeyBindingScreen screen{system};
    screen.beginCapture(InputAction::MoveForward);
    static_cast<void>(screen.applyKey(Key::T));
    screen.beginCapture(InputAction::Jump);
    static_cast<void>(screen.applyKey(Key::E));
    // Both are now off-default.
    assert(system.bindings().binding(InputAction::MoveForward) == input::keyboard(Key::T));

    screen.resetToDefaults();
    assert(!screen.capturing());
    assert(system.bindings().binding(InputAction::MoveForward) == input::keyboard(Key::W));
    assert(system.bindings().binding(InputAction::Jump) == input::keyboard(Key::Space));
    assert(system.bindings().binding(InputAction::Inventory) == input::keyboard(Key::E));
}

// --- Capture toggling: clicking the same row twice cancels; a new row moves ----
void testCaptureToggle() {
    input::InputSystem system;
    input::KeyBindingScreen screen{system};
    screen.beginCapture(InputAction::Sneak);
    assert(screen.capturing() && screen.capturingAction() == InputAction::Sneak);
    screen.beginCapture(InputAction::Sneak);  // same row -> cancel
    assert(!screen.capturing());
    screen.beginCapture(InputAction::Sneak);
    screen.beginCapture(InputAction::Sprint);  // different row -> moves
    assert(screen.capturing() && screen.capturingAction() == InputAction::Sprint);
    screen.cancelCapture();
    assert(!screen.capturing());
    // applyKey with nothing capturing is a no-op that changes nothing.
    const auto none = screen.applyKey(Key::W);
    assert(!none.applied);
}

// --- The Controls page lists every rebindable action + reset + done ------------
void testControlsPageLIstsBindRows() {
    ui::MenuBuildContext ctx;
    input::InputSystem system;
    ctx.keyBindLabelFor = [&system](InputAction action) {
        return std::string{input::actionDisplayName(action)} + ": " +
               input::bindingDisplayName(system.bindings().binding(action));
    };
    // PX-6 Bug1: the Controls key-bind list is windowed. Ask for the full window
    // so every action is listed (a real screen sizes the window to the canvas).
    ctx.keyBindFirstIndex = 0;
    ctx.keyBindRowCount = input::keyBindRows().size();
    ui::MenuCallbacks cb;
    InputAction captured = InputAction::Count;
    bool reset = false;
    cb.beginKeyCapture = [&](InputAction a) { captured = a; };
    cb.resetKeyBinds = [&] { reset = true; };

    const auto rectFor = [](std::size_t index) {
        return ui::UiRect{0.0F, static_cast<float>(index) * 20.0F, 200.0F, 20.0F};
    };
    const ui::Page page = ui::buildPage(ui::PageId::Controls, ctx, cb, rectFor);

    // One ListRow per rebindable action (the full window was requested).
    std::size_t rows = 0;
    std::size_t firstRow = ui::kNoWidget;
    for (std::size_t i = 0; i < page.size(); ++i) {
        if (page[i].debugId == static_cast<std::uint16_t>(ui::WidgetId::KeyBindRow)) {
            if (firstRow == ui::kNoWidget) firstRow = i;
            ++rows;
        }
    }
    assert(rows == input::keyBindRows().size());
    // The first row is Forward and its label reflects the live binding (W).
    assert(page[firstRow].label == "Forward: W");

    // Reset and Done exist.
    bool hasReset = false;
    bool hasDone = false;
    for (const auto& w : page) {
        if (w.debugId == static_cast<std::uint16_t>(ui::WidgetId::ResetKeyBinds)) hasReset = true;
        if (w.debugId == static_cast<std::uint16_t>(ui::WidgetId::Done)) hasDone = true;
    }
    assert(hasReset && hasDone);

    // Clicking the first row begins capture for Forward (the first listed action).
    const float rowY = page[firstRow].rect.y + page[firstRow].rect.height * 0.5F;
    const std::size_t fired = ui::clickAt(page, 100.0F, rowY);
    assert(fired == firstRow);
    assert(captured == input::keyBindRows()[0]);
    assert(captured == InputAction::MoveForward);
}

// --- End-to-end: clicking a row then applying a key rebinds the single source --
void testPageRowToRebind() {
    input::InputSystem system;
    input::KeyBindingScreen screen{system};
    ui::MenuBuildContext ctx;
    // PX-6 Bug1: request the full key-bind window so the Inventory row is built.
    ctx.keyBindFirstIndex = 0;
    ctx.keyBindRowCount = input::keyBindRows().size();
    ctx.keyBindLabelFor = [&system, &screen](InputAction action) {
        if (screen.capturing() && screen.capturingAction() == action) {
            return std::string{input::actionDisplayName(action)} + ": > ? <";
        }
        return std::string{input::actionDisplayName(action)} + ": " +
               input::bindingDisplayName(system.bindings().binding(action));
    };
    ui::MenuCallbacks cb;
    cb.beginKeyCapture = [&screen](InputAction a) { screen.beginCapture(a); };
    const auto rectFor = [](std::size_t index) {
        return ui::UiRect{0.0F, static_cast<float>(index) * 20.0F, 200.0F, 20.0F};
    };

    // Build the page, click the Inventory row, then press K -> Inventory = K.
    ui::Page page = ui::buildPage(ui::PageId::Controls, ctx, cb, rectFor);
    // Find the Inventory row by its label prefix.
    std::size_t invRow = ui::kNoWidget;
    for (std::size_t i = 0; i < page.size(); ++i) {
        if (page[i].label.rfind("Inventory:", 0) == 0) {
            invRow = i;
            break;
        }
    }
    assert(invRow != ui::kNoWidget);
    const float rowY = page[invRow].rect.y + page[invRow].rect.height * 0.5F;
    static_cast<void>(ui::clickAt(page, 100.0F, rowY));
    assert(screen.capturing() && screen.capturingAction() == InputAction::Inventory);
    // Now the row shows the capturing prompt when rebuilt.
    page = ui::buildPage(ui::PageId::Controls, ctx, cb, rectFor);
    assert(page[invRow].label == "Inventory: > ? <");
    // Apply a key: the single source updates.
    const auto res = screen.applyKey(Key::T);  // T is free (Chat still on T? default Chat=T)
    assert(res.applied);
    assert(system.bindings().binding(InputAction::Inventory) == input::keyboard(Key::T));
}

}  // namespace

int main() {
    testRebindWritesThroughSingleSource();
    testConflictDetection();
    testResetToDefaults();
    testCaptureToggle();
    testControlsPageLIstsBindRows();
    testPageRowToRebind();
    return 0;
}

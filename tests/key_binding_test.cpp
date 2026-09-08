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
#include "ui/ListRow.hpp"
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
    // UI-6b：按钮上写的是**键名**，不是"动作: 按键"——动作名是它旁边那个 Label。
    ctx.keyBindLabelFor = [&system](InputAction action) {
        return input::bindingDisplayName(system.bindings().binding(action));
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

    // UI-6b: a bind row is TWO widgets now, not one -- the action's name as a Label
    // and the change button as a Button, matching 26.1's KeyBindsList.KeyEntry
    // (whose children() hands out changeButton and resetButton separately). The
    // whole point is that adding another bindable action never touches the screen
    // code again, so the shape of a row is asserted here rather than eyeballed.
    std::size_t rows = 0;
    std::size_t firstRow = ui::kNoWidget;
    for (std::size_t i = 0; i < page.size(); ++i) {
        if (page[i].debugId == static_cast<std::uint16_t>(ui::WidgetId::KeyBindRow)) {
            if (firstRow == ui::kNoWidget) firstRow = i;
            ++rows;
        }
    }
    assert(rows == input::keyBindRows().size() * ui::kKeyBindWidgetsPerRow);
    // The name is a Label and is NOT interactive: focus skips it and clicking it
    // must not start a capture. The button next to it carries the live key name.
    assert(page[firstRow].kind == ui::WidgetKind::Label);
    assert(!page[firstRow].enabled);
    assert(page[firstRow].label == "Forward");
    assert(page[firstRow + 1].kind == ui::WidgetKind::Button);
    assert(page[firstRow + 1].label == "W");
    // ...and NOT the old "Forward: W" single-row label.
    assert(page[firstRow].label != "Forward: W");

    // Reset and Done exist.
    bool hasReset = false;
    bool hasDone = false;
    for (const auto& w : page) {
        if (w.debugId == static_cast<std::uint16_t>(ui::WidgetId::ResetKeyBinds)) hasReset = true;
        if (w.debugId == static_cast<std::uint16_t>(ui::WidgetId::Done)) hasDone = true;
    }
    assert(hasReset && hasDone);

    // Clicking the CHANGE BUTTON begins capture for Forward (the first listed action).
    const std::size_t changeIndex = firstRow + 1U;
    const float buttonY = page[changeIndex].rect.y + page[changeIndex].rect.height * 0.5F;
    const std::size_t fired = ui::clickAt(page, 100.0F, buttonY);
    assert(fired == changeIndex);
    assert(captured == input::keyBindRows()[0]);
    assert(captured == InputAction::MoveForward);

    // Clicking the NAME does nothing: it is a disabled Label. Before UI-6b the whole
    // row was one clickable widget, so this click used to start a capture.
    captured = InputAction::Count;
    const float nameY = page[firstRow].rect.y + page[firstRow].rect.height * 0.5F;
    assert(ui::clickAt(page, 100.0F, nameY) == ui::kNoWidget);
    assert(captured == InputAction::Count);
}

// --- End-to-end: clicking a row then applying a key rebinds the single source --
void testPageRowToRebind() {
    input::InputSystem system;
    input::KeyBindingScreen screen{system};
    ui::MenuBuildContext ctx;
    // PX-6 Bug1: request the full key-bind window so the Inventory row is built.
    ctx.keyBindFirstIndex = 0;
    ctx.keyBindRowCount = input::keyBindRows().size();
    // 与渲染器走**同一个**装饰函数：捕获中是 `> 键名 <`，冲突是 `[ 键名 ]`。
    ctx.keyBindLabelFor = [&system, &screen](InputAction action) {
        const auto decoration = screen.capturing() && screen.capturingAction() == action
                                    ? ui::KeyBindDecoration::Capturing
                                    : ui::KeyBindDecoration::None;
        return ui::decorateKeyBindLabel(
            input::bindingDisplayName(system.bindings().binding(action)), decoration);
    };
    ui::MenuCallbacks cb;
    cb.beginKeyCapture = [&screen](InputAction a) { screen.beginCapture(a); };
    const auto rectFor = [](std::size_t index) {
        return ui::UiRect{0.0F, static_cast<float>(index) * 20.0F, 200.0F, 20.0F};
    };

    // Build the page, click the Inventory row, then press K -> Inventory = K.
    ui::Page page = ui::buildPage(ui::PageId::Controls, ctx, cb, rectFor);
    // Find the Inventory row by its NAME label; the change button is the next widget.
    std::size_t invRow = ui::kNoWidget;
    for (std::size_t i = 0; i < page.size(); ++i) {
        if (page[i].kind == ui::WidgetKind::Label && page[i].label == "Inventory") {
            invRow = i;
            break;
        }
    }
    assert(invRow != ui::kNoWidget);
    const std::size_t invButton = invRow + 1U;
    assert(page[invButton].kind == ui::WidgetKind::Button);
    const float rowY = page[invButton].rect.y + page[invButton].rect.height * 0.5F;
    static_cast<void>(ui::clickAt(page, 100.0F, rowY));
    assert(screen.capturing() && screen.capturingAction() == InputAction::Inventory);
    // Now the BUTTON shows the capturing prompt when rebuilt -- and the name beside
    // it is untouched, which is the whole point of splitting the row in two.
    page = ui::buildPage(ui::PageId::Controls, ctx, cb, rectFor);
    assert(page[invButton].label == "> E <");
    assert(page[invRow].label == "Inventory");
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

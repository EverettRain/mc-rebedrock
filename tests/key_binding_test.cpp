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
using mc::input::MouseButton;

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
    // UI-6b：一次给出两段——动作名与按钮上的键名。
    ctx.keyBindLabelsFor = [&system](InputAction action) {
        return ui::MenuBuildContext::KeyBindRowLabels{
            std::string{input::actionDisplayName(action)},
            input::bindingDisplayName(system.bindings().binding(action))};
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
    ctx.keyBindLabelsFor = [&system, &screen](InputAction action) {
        const auto decoration = screen.capturing() && screen.capturingAction() == action
                                    ? ui::KeyBindDecoration::Capturing
                                    : ui::KeyBindDecoration::None;
        return ui::MenuBuildContext::KeyBindRowLabels{
            std::string{input::actionDisplayName(action)},
            ui::decorateKeyBindLabel(
                input::bindingDisplayName(system.bindings().binding(action)), decoration)};
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

// --- 翻译键：按键设置的两段文字都要能被本地化 -------------------------------
//
// 实机缺陷：界面切成中文以后，按键设置的标题与底部按钮是中文，**列表里满屏还是
// "Forward / Back / Left Shift"**。两段文字都是 `input::*DisplayName` 的英文硬编码。
//
// 翻译键取自 26.1 自己的（`Options.java` 里那批 `new KeyMapping("key.…")` 与
// `InputConstants.Key` 的 `key.keyboard.*`），所以玩家自备的 vanilla 资源包里本来
// 就有它们。这些键写错了不会崩，只会让某一行**回落到英文**——而界面上其它行是中文，
// 看起来就像"这一条漏翻了"。
void testTranslationKeys() {
    // 动作名：逐条对 26.1 的 KeyMapping 名字
    assert(input::actionTranslationKey(InputAction::MoveForward) == "key.forward");
    assert(input::actionTranslationKey(InputAction::MoveBack) == "key.back");
    assert(input::actionTranslationKey(InputAction::Jump) == "key.jump");
    assert(input::actionTranslationKey(InputAction::Sneak) == "key.sneak");
    assert(input::actionTranslationKey(InputAction::Attack) == "key.attack");
    assert(input::actionTranslationKey(InputAction::Use) == "key.use");
    assert(input::actionTranslationKey(InputAction::Inventory) == "key.inventory");
    assert(input::actionTranslationKey(InputAction::DropItem) == "key.drop");
    assert(input::actionTranslationKey(InputAction::Hotbar1) == "key.hotbar.1");
    assert(input::actionTranslationKey(InputAction::Hotbar9) == "key.hotbar.9");
    // ★ 是 togglePerspective，不是 perspective——照本作的枚举名猜会猜错
    assert(input::actionTranslationKey(InputAction::Perspective) == "key.togglePerspective");
    // vanilla 的 F3 不可重绑，没有 KeyMapping 名字，所以走本项目自有的命名空间
    assert(input::actionTranslationKey(InputAction::Debug) == "key.rebedrock.debug");

    // 每一行的键都必须互不相同：撞了就是两行显示同一个名字。
    const auto rows = input::keyBindRows();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto key = input::actionTranslationKey(rows[i]);
        assert(!key.empty());
        assert(key.rfind("key.", 0) == 0);
        for (std::size_t j = i + 1; j < rows.size(); ++j) {
            assert(key != input::actionTranslationKey(rows[j]));
        }
    }

    // 键名：`InputConstants.Key` 的名字
    assert(input::keyTranslationKey(Key::Space) == "key.keyboard.space");
    // ★ 左右修饰键中间还有一段：`left.shift`，不是 `leftshift`
    assert(input::keyTranslationKey(Key::LeftShift) == "key.keyboard.left.shift");
    assert(input::keyTranslationKey(Key::LeftControl) == "key.keyboard.left.control");
    assert(input::keyTranslationKey(Key::F3) == "key.keyboard.f3");
    // 数字键是裸数字，不是 `digit1`
    assert(input::keyTranslationKey(Key::Digit1) == "key.keyboard.1");
    assert(input::keyTranslationKey(Key::W) == "key.keyboard.w");
    assert(input::keyTranslationKey(Key::Unknown) == "key.keyboard.unknown");
    assert(input::mouseTranslationKey(MouseButton::Left) == "key.mouse.left");
    assert(input::mouseTranslationKey(MouseButton::Middle) == "key.mouse.middle");

    // 绑定 → 键名的分派
    assert(input::bindingTranslationKey(input::keyboard(Key::Space)) == "key.keyboard.space");
    assert(input::bindingTranslationKey(input::mouse(MouseButton::Right)) == "key.mouse.right");
    // 未绑定与 vanilla 一样落在 unknown（"未指定"）
    assert(input::bindingTranslationKey(input::InputBinding{}) == "key.keyboard.unknown");

    // ★ 兜底仍然是英文名，且**不是**翻译键本身。翻译缺失时显示 "W"，
    //   与 26.1 一致（它对可打印键走 GLFW 的系统键名，vanilla 语言表里根本没有
    //   `key.keyboard.w`）——回落成 "key.keyboard.w" 才是坏掉的样子。
    assert(input::keyName(Key::W) == "W");
    assert(input::actionDisplayName(InputAction::MoveForward) == "Forward");
}

// --- 一行的两段文字是**一个**返回值 ------------------------------------------
//
// 实机缺陷的第二半：曾经是两个回调，而填这份上下文的地方有两处（渲染器的
// buildCurrentPage 与 HudRenderer 常驻的 drawContext_）。只填了一处的那个字段
// 静默回落到英文兜底——键名汉化了、动作名没有。做成一个返回值的两个字段之后，
// "漏填一个"在类型上就不成立。
void testRowLabelsComeAsOneValue() {
    input::InputSystem system;
    ui::MenuBuildContext ctx;
    ctx.keyBindFirstIndex = 0;
    ctx.keyBindRowCount = input::keyBindRows().size();
    // 一个桩把两段都填成可辨认的样子
    ctx.keyBindLabelsFor = [&system](InputAction action) {
        return ui::MenuBuildContext::KeyBindRowLabels{
            "A:" + std::string{input::actionTranslationKey(action)},
            "K:" + std::string{input::bindingTranslationKey(system.bindings().binding(action))}};
    };
    ui::MenuCallbacks cb;
    const auto rectFor = [](std::size_t index) {
        return ui::UiRect{0.0F, static_cast<float>(index) * 20.0F, 200.0F, 20.0F};
    };
    const ui::Page page = ui::buildPage(ui::PageId::Controls, ctx, cb, rectFor);
    std::size_t checked = 0;
    for (std::size_t i = 0; i + 1 < page.size(); ++i) {
        if (page[i].kind != ui::WidgetKind::Label ||
            page[i].debugId != static_cast<std::uint16_t>(ui::WidgetId::KeyBindRow)) {
            continue;
        }
        // 两段都来自那一个回调：任何一段落空就是回落到了兜底。
        assert(page[i].label.rfind("A:", 0) == 0);
        assert(page[i + 1].label.rfind("K:", 0) == 0);
        ++checked;
    }
    assert(checked == input::keyBindRows().size());
}

}  // namespace

int main() {
    testRebindWritesThroughSingleSource();
    testConflictDetection();
    testResetToDefaults();
    testCaptureToggle();
    testControlsPageLIstsBindRows();
    testTranslationKeys();
    testRowLabelsComeAsOneValue();
    testPageRowToRebind();
    return 0;
}

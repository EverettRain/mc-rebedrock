// PX-5: the Key Binds screen. Rebinding writes through the PX-1 InputSystem — the
// SINGLE SOURCE of controls — so a rebind actually changes the game's input, with
// no private page-local copy. Covers: begin capture -> press key -> binding table
// updated; conflict detection; reset to defaults; capture toggle/cancel; and the
// Controls page listing every rebindable action as a clickable row that begins
// its capture. All headless: no GLFW, no Vulkan.

// ★ 这两个头文件**必须**能进同一个翻译单元。
//
// 它们从前在同一命名空间里各定义了一个同名同签名、函数体不同的 `keyName(Key)`
// （显示名 `"Left Shift"` vs 存档 token `"LeftShift"`）——一次 ODR 违规：两边的 TU 在
// 同一个二进制里，链接器挑哪一份是随意的，挑错了 `bindingToToken` 会把 `"Left Shift"`
// 写进配置文件而 `keyFromName` 再也解析不回来，重开游戏那条绑定静默回默认。
// 一起 include 就是那条护栏：谁把它们改回同名，这里**编译不过**。
#include "input/BindingConfig.hpp"
#include "input/InputNaming.hpp"
#include "input/InputSystem.hpp"
#include "input/KeyBindingScreen.hpp"
#include "ui/MenuInteraction.hpp"
#include "ui/KeyBindList.hpp"
#include "ui/ListRow.hpp"
#include "ui/PageBuilder.hpp"

#include <array>
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

// --- The Key Binds page lists every rebindable action + reset + done ----------
//
// 偏差 D1 收口之后，绑定列表在 §7.8 的 PageId::KeyBinds（Controls 只是 §7.6 的
// 排版枢纽），所以这里建的是 KeyBinds 页。
void testKeyBindsPageListsBindRows() {
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
    // UI-6c：窗口数的是**行**（含分类标题行），要全部动作就得给足行数。
    ctx.keyBindRowCount = ui::kKeyBindListRowCount;
    ui::MenuCallbacks cb;
    InputAction captured = InputAction::Count;
    bool reset = false;
    cb.beginKeyCapture = [&](InputAction a) { captured = a; };
    cb.resetKeyBinds = [&] { reset = true; };

    const auto rectFor = [](std::size_t index) {
        return ui::UiRect{0.0F, static_cast<float>(index) * 20.0F, 200.0F, 20.0F};
    };
    const ui::Page page = ui::buildPage(ui::PageId::KeyBinds, ctx, cb, rectFor);

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
    // UI-6c：一行三个控件，但只有前两个（名称 Label、改键 Button）带 KeyBindRow 这个
    // debugId；重置按钮带它自己的 ResetKeyBind。所以这里数出来的是行数的两倍。
    assert(rows == input::keyBindRows().size() * 2U);
    std::size_t resets = 0;
    for (const auto& w : page) {
        if (w.debugId == static_cast<std::uint16_t>(ui::WidgetId::ResetKeyBind)) ++resets;
    }
    assert(resets == input::keyBindRows().size());
    assert(rows + resets == input::keyBindRows().size() * ui::kKeyBindWidgetsPerRow);
    // The name is a Label and is NOT interactive: focus skips it and clicking it
    // must not start a capture. The button next to it carries the live key name.
    //
    // ★ UI-6c 之后第一行不再是 Forward：行序按 26.1 的分类分组排过（Movement 组
    //   内按显示名排序，"Jump" 在最前），所以这里跟着 keyBindRows()[0] 走，
    //   顺序本身由 testCategoryGrouping 单独断言。
    const InputAction firstAction = input::keyBindRows()[0];
    assert(firstAction == InputAction::Jump);
    assert(page[firstRow].kind == ui::WidgetKind::Label);
    assert(!page[firstRow].enabled);
    assert(page[firstRow].label == std::string{input::actionDisplayName(firstAction)});
    assert(page[firstRow + 1].kind == ui::WidgetKind::Button);
    assert(page[firstRow + 1].label ==
           input::bindingDisplayName(system.bindings().binding(firstAction)));
    assert(page[firstRow + 1].label == "Space");  // Jump 的默认键
    // ...and NOT the old "Jump: Space" single-row label.
    assert(page[firstRow].label.find(':') == std::string::npos);

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
    assert(captured == firstAction);

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
    // UI-6c：窗口数的是**行**（含分类标题行），要全部动作就得给足行数。
    ctx.keyBindRowCount = ui::kKeyBindListRowCount;
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
    ui::Page page = ui::buildPage(ui::PageId::KeyBinds, ctx, cb, rectFor);
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
    page = ui::buildPage(ui::PageId::KeyBinds, ctx, cb, rectFor);
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
    // UI-6c：窗口数的是**行**（含分类标题行），要全部动作就得给足行数。
    ctx.keyBindRowCount = ui::kKeyBindListRowCount;
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
    const ui::Page page = ui::buildPage(ui::PageId::KeyBinds, ctx, cb, rectFor);
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

// --- UI-6c：逐项默认值只有一个来源 -------------------------------------------
//
// 26.1 的每个 KeyMapping 自带 defaultKey（KeyMapping.java:98），逐项 Reset 就是
// setKey(getDefaultKey())（KeyBindsList.java:122）。本作的 defaultBinding 从
// defaults() 里取——手抄第二份逐项默认表会漂：整表 Reset 与逐项 Reset 得出不同的键，
// 而且新表里的值在界面上从没出现过，最难查。
void testDefaultBindingIsTheSameSourceAsDefaults() {
    const input::BindingTable table = input::BindingTable::defaults();
    for (std::size_t i = 0; i < input::kInputActionCount; ++i) {
        const auto action = static_cast<InputAction>(i);
        assert(input::BindingTable::defaultBinding(action) == table.binding(action));
    }
    // constexpr：默认值在编译期就能取到（界面每帧给 Reset 按钮置灰会调它）。
    static_assert(input::BindingTable::defaultBinding(InputAction::MoveForward) ==
                  input::keyboard(Key::W));
    static_assert(input::BindingTable::defaultBinding(InputAction::Attack) ==
                  input::mouse(MouseButton::Left));
}

// --- UI-6c：逐项重置只动这一行；isDefault 给按钮置灰 --------------------------
void testResetOneAndIsDefault() {
    input::InputSystem system;
    input::KeyBindingScreen screen{system};

    // 出厂状态：每一行都是默认，底部「重置所有」该置灰。
    for (const InputAction action : input::keyBindRows()) {
        assert(screen.isDefault(action));
    }
    assert(!screen.anyNonDefault());

    screen.beginCapture(InputAction::MoveForward);
    static_cast<void>(screen.applyKey(Key::T));
    screen.beginCapture(InputAction::Jump);
    static_cast<void>(screen.applyKey(Key::Tab));
    assert(!screen.isDefault(InputAction::MoveForward));
    assert(!screen.isDefault(InputAction::Jump));
    assert(screen.anyNonDefault());

    // ★ 只重置一行：另一行必须原封不动。整表重置冒充逐项重置的话，这一条会红。
    screen.resetOne(InputAction::MoveForward);
    assert(system.bindings().binding(InputAction::MoveForward) == input::keyboard(Key::W));
    assert(screen.isDefault(InputAction::MoveForward));
    assert(system.bindings().binding(InputAction::Jump) == input::keyboard(Key::Tab));
    assert(!screen.isDefault(InputAction::Jump));
    assert(screen.anyNonDefault());

    screen.resetOne(InputAction::Jump);
    assert(system.bindings().binding(InputAction::Jump) == input::keyboard(Key::Space));
    assert(!screen.anyNonDefault());

    // 重置一个本来就是默认的行是个幂等的空操作，且不会波及别人。
    screen.beginCapture(InputAction::Sneak);
    static_cast<void>(screen.applyKey(Key::Tab));
    screen.resetOne(InputAction::MoveForward);
    assert(system.bindings().binding(InputAction::Sneak) == input::keyboard(Key::Tab));
}

// --- UI-6c：解绑（等待按键时按 Esc）------------------------------------------
//
// 26.1：`if (event.isEscape()) selectedKey.setKey(InputConstants.UNKNOWN);`
// （KeyBindsScreen.java:71-86）——Esc 是**解绑**，不是取消本次改键。
// InputDevice::None 这个表示本来就在，但在 UI-6c 之前没有任何路径能产生它。
void testUnbind() {
    input::InputSystem system;
    input::KeyBindingScreen screen{system};

    screen.beginCapture(InputAction::Jump);
    const auto result = screen.applyUnbound();
    assert(result.applied);
    assert(!screen.capturing());  // 与按下一个真键一样，捕获结束
    const auto jump = system.bindings().binding(InputAction::Jump);
    assert(jump.device == input::InputDevice::None);
    // 界面上这一行显示「未指定」，而且 Reset 按钮亮起（不是默认了）。
    assert(input::bindingDisplayName(jump) == "Not Bound");
    assert(input::bindingTranslationKey(jump) == "key.keyboard.unknown");
    assert(!screen.isDefault(InputAction::Jump));

    // ★ 真的落到了单一源：Space 按下去不再跳。只改显示不改绑定的实现会在这里红。
    input::InputSystem::EventQueue queue;
    input::RawInputFrame frame;
    frame.setKey(Key::Space, true);
    const auto intent = system.poll(frame, queue);
    assert(!intent.jumpHeld);

    // 解绑第二行：两个未绑定的行**不算**互相冲突（KeyBindsList.java:161 的外层守卫）。
    screen.beginCapture(InputAction::Sprint);
    static_cast<void>(screen.applyUnbound());
    assert(system.bindings().binding(InputAction::Sprint).device == input::InputDevice::None);
    assert(screen.conflictsFor(InputAction::Jump).empty());
    assert(screen.conflictsFor(InputAction::Sprint).empty());

    // 没在捕获时解绑是 no-op：界面没法解绑一个没被选中的行。
    const auto noop = screen.applyUnbound();
    assert(!noop.applied);

    // 逐项重置把它绑回来。
    screen.resetOne(InputAction::Jump);
    assert(system.bindings().binding(InputAction::Jump) == input::keyboard(Key::Space));
}

// --- UI-6c：冲突要收集**全部**，不是第一个 -----------------------------------
//
// 26.1 把所有同键的动作拼成 controls.keybinds.duplicateKeybinds 的 tooltip
// （"This key is also used for:\n%s"，en_us.json:3187；KeyBindsList.java:160-171）。
void testConflictsCollectAll() {
    input::InputSystem system;
    input::KeyBindingScreen screen{system};

    // E 上堆三个动作：Inventory（默认就在 E）+ Sneak + Sprint。
    screen.beginCapture(InputAction::Sneak);
    static_cast<void>(screen.applyKey(Key::E));
    screen.beginCapture(InputAction::Sprint);
    const auto third = screen.applyKey(Key::E);
    assert(third.conflict);

    const auto conflicts = screen.conflictsFor(InputAction::Inventory);
    assert(conflicts.size() == 2);  // ★ 只报第一个的实现在这里红
    bool sawSneak = false;
    bool sawSprint = false;
    for (const InputAction other : conflicts) {
        sawSneak = sawSneak || other == InputAction::Sneak;
        sawSprint = sawSprint || other == InputAction::Sprint;
        assert(other != InputAction::Inventory);  // 不跟自己比
    }
    assert(sawSneak && sawSprint);

    // 对称：从 Sneak 看过去也是两个。
    const auto fromSneak = screen.conflictsFor(InputAction::Sneak);
    assert(fromSneak.size() == 2);

    // 没撞的行不报冲突。
    assert(screen.conflictsFor(InputAction::MoveForward).empty());

    // 撤掉一个，剩下的仍然互报。
    screen.resetOne(InputAction::Sprint);
    assert(screen.conflictsFor(InputAction::Inventory).size() == 1);
    assert(screen.conflictsFor(InputAction::Inventory)[0] == InputAction::Sneak);
}

// --- UI-6c：冲突谓词的第 4 档（两边都是出厂默认就不报）------------------------
//
// `(!otherKey.isDefault() || !this.key.isDefault())`（KeyBindsList.java:163）不是
// 「随便两个同键就算冲突」：两边**都还停在出厂默认**的撞键不报，因为 26.1 自己就
// 发了一批默认相撞的键（Options.java:616/:643 鼠标中键、:640/:646 的 C、
// :644/:645 的 F3），不加这一句全新安装就满屏黄条。
//
// 本作的默认表今天没有重复默认，所以这一档在 conflictsFor 里跑不到——它只能靠
// 纯谓词直接断言，见 KeyBindingScreen.hpp 里那段注释。
void testReportableConflictPredicate() {
    const auto e = input::keyboard(Key::E);
    const auto t = input::keyboard(Key::T);
    const input::InputBinding unbound{};

    // 同键 + 两边都是默认 -> 不报（第 4 档）
    assert(!input::isReportableConflict(e, true, e, true));
    // 同键 + 只要有一边被改过 -> 报（两种方向都要）
    assert(input::isReportableConflict(e, false, e, true));
    assert(input::isReportableConflict(e, true, e, false));
    assert(input::isReportableConflict(e, false, e, false));
    // 不同键 -> 永远不报
    assert(!input::isReportableConflict(e, false, t, false));
    // 本行未绑定 -> 整行跳过（第 1 档），哪怕对面也未绑定
    assert(!input::isReportableConflict(unbound, false, unbound, false));
    // 对面未绑定、本行有绑定 -> 键不同，不报
    assert(!input::isReportableConflict(e, false, unbound, false));
}

// 本作的默认表里没有两个行共用同一个控件——这是上面那一档「恒不生效」的前提。
// 哪天为了对齐 26.1 引入了重复默认，这条会红，提醒去看 isReportableConflict 的
// 第 4 档是不是终于开始起作用了（以及界面上会不会冒出黄条）。
void testNoDuplicateDefaults() {
    const auto rows = input::keyBindRows();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        for (std::size_t j = i + 1; j < rows.size(); ++j) {
            assert(input::BindingTable::defaultBinding(rows[i]) !=
                   input::BindingTable::defaultBinding(rows[j]));
        }
    }
}

// --- UI-6c：分类与分组排序 ----------------------------------------------------
//
// 组序 = Category.SORT_ORDER 的注册序（KeyMapping.java:199-221），由
// KeyMapping.compareTo 的 SORT_ORDER.indexOf 比较（KeyMapping.java:147）。
// 组内 = 显示名字典序（KeyMapping.java:145）。
void testCategoryGrouping() {
    using input::InputCategory;
    // 逐条对 Options.java 里那批 new KeyMapping(..., Category.XXX)
    assert(input::actionCategory(InputAction::MoveForward) == InputCategory::Movement);
    assert(input::actionCategory(InputAction::Sprint) == InputCategory::Movement);
    // ★ Attack/Use 是 GAMEPLAY，不是 Movement（Options.java:612-613）
    assert(input::actionCategory(InputAction::Attack) == InputCategory::Gameplay);
    assert(input::actionCategory(InputAction::Use) == InputCategory::Gameplay);
    // ★ Drop 是 INVENTORY，不是 Gameplay（Options.java:611）
    assert(input::actionCategory(InputAction::DropItem) == InputCategory::Inventory);
    assert(input::actionCategory(InputAction::Inventory) == InputCategory::Inventory);
    assert(input::actionCategory(InputAction::Hotbar5) == InputCategory::Inventory);
    // ★ 聊天/命令是 MULTIPLAYER，不是 Misc（Options.java:617/:619）
    assert(input::actionCategory(InputAction::Chat) == InputCategory::Multiplayer);
    assert(input::actionCategory(InputAction::Command) == InputCategory::Multiplayer);
    assert(input::actionCategory(InputAction::Perspective) == InputCategory::Misc);
    // ★ 26.1 有一个真的 DEBUG 分类，且 keyDebugOverlay（F3）在 keyMappings 里
    //   （Options.java:644 / :726），所以本作的 Debug 归 Debug 而不是 Misc。
    assert(input::actionCategory(InputAction::Debug) == InputCategory::Debug);

    // ★ 分类标题的翻译键是 key.category.minecraft.*，不是遗留的 key.categories.*
    //   （Category.label() = id.toLanguageKey("key.category")，KeyMapping.java:223-225；
    //    Identifier.toLanguageKey() = namespace + "." + path，Identifier.java:159-161）。
    assert(input::categoryTranslationKey(InputCategory::Movement) ==
           "key.category.minecraft.movement");
    assert(input::categoryTranslationKey(InputCategory::Misc) == "key.category.minecraft.misc");
    assert(input::categoryTranslationKey(InputCategory::Debug) == "key.category.minecraft.debug");
    assert(input::categoryDisplayName(InputCategory::Misc) == "Miscellaneous");  // 不是 "Misc"
    for (std::size_t i = 0; i < input::kInputCategoryCount; ++i) {
        const auto category = static_cast<InputCategory>(i);
        const auto key = input::categoryTranslationKey(category);
        assert(key.rfind("key.category.minecraft.", 0) == 0);
        assert(!input::categoryDisplayName(category).empty());
    }

    const auto rows = input::keyBindRows();

    // 排序是个置换：一项不多、一项不少（Pause 仍然不在内）。
    assert(rows.size() == input::kRebindableActions.size());
    for (const InputAction action : input::kRebindableActions) {
        std::size_t seen = 0;
        for (const InputAction row : rows) {
            if (row == action) ++seen;
        }
        assert(seen == 1);
    }
    for (const InputAction row : rows) {
        assert(row != InputAction::Pause);
    }

    // 分组不变量：同一个分类的行必须**连成一段**。界面靠「相邻两行分类不同」插标题，
    // 一个分类要是断成两段就会插出两条同名标题。
    std::array<bool, input::kInputCategoryCount> closed{};
    InputCategory previous = input::actionCategory(rows[0]);
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto category = input::actionCategory(rows[i]);
        if (category != previous) {
            assert(!closed[input::index(category)]);  // 这个分类不能再出现第二次
            closed[input::index(previous)] = true;
            // 组序：新分类在 SORT_ORDER 里必须排在前一个之后（严格递增）。
            assert(input::index(category) > input::index(previous));
            previous = category;
        } else {
            // 组内：按显示名字典序（KeyMapping.compareTo，KeyMapping.java:145）。
            assert(input::actionSortName(rows[i - 1]) < input::actionSortName(rows[i]));
        }
    }

    // 出现过的分类顺序，逐条对 SORT_ORDER 的子序列：
    // MOVEMENT -> MISC -> MULTIPLAYER -> GAMEPLAY -> INVENTORY -> DEBUG。
    // ★ MISC 排在 MULTIPLAYER/GAMEPLAY **之前**（KeyMapping.java:201-203）——
    //   按「常用在前」或字母序猜都会猜错。
    const std::array<InputCategory, 6> expectedGroups = {
        InputCategory::Movement, InputCategory::Misc,      InputCategory::Multiplayer,
        InputCategory::Gameplay, InputCategory::Inventory, InputCategory::Debug};
    std::size_t group = 0;
    assert(input::actionCategory(rows[0]) == expectedGroups[0]);
    for (std::size_t i = 1; i < rows.size(); ++i) {
        if (input::actionCategory(rows[i]) != input::actionCategory(rows[i - 1])) {
            ++group;
            assert(group < expectedGroups.size());
            assert(input::actionCategory(rows[i]) == expectedGroups[group]);
        }
    }
    assert(group + 1 == expectedGroups.size());

    // 具体的头尾：en_us 里 Movement 组是 Jump / Sneak / Sprint / Strafe Left /
    // Strafe Right / Walk Backward / Walk Forward，所以第一行是 Jump、
    // Movement 组的最后一行是 MoveForward（"Walk Forward"）。
    assert(rows[0] == InputAction::Jump);
    assert(rows[1] == InputAction::Sneak);
    assert(rows[2] == InputAction::Sprint);
    assert(rows[3] == InputAction::MoveLeft);
    assert(rows[4] == InputAction::MoveRight);
    assert(rows[5] == InputAction::MoveBack);
    assert(rows[6] == InputAction::MoveForward);
    assert(rows[7] == InputAction::Perspective);   // Misc
    assert(rows[8] == InputAction::Chat);          // Multiplayer
    assert(rows[9] == InputAction::Command);
    assert(rows[10] == InputAction::Attack);       // Gameplay
    assert(rows[11] == InputAction::Use);
    assert(rows[12] == InputAction::DropItem);     // Inventory: "Drop Selected Item"
    assert(rows[13] == InputAction::Hotbar1);
    assert(rows[21] == InputAction::Hotbar9);
    assert(rows[22] == InputAction::Inventory);    // "Open/Close Inventory"
    assert(rows[23] == InputAction::Debug);        // Debug
}

// --- 显示名与存档 token 是两张表，不是一张 ---------------------------------
//
// 一张给人看（要翻译、有空格），一张给文件读（必须逐字稳定、绝不能随语言变）。
// 合成一张就是让存档格式跟着界面文案走。
void testDisplayNamesAndTokensAreSeparate() {
    // 同一个键，两张表给出**不同**的字符串——这正是它们不能共用一个名字的理由。
    assert(input::keyName(Key::LeftShift) == "Left Shift");
    assert(input::keyToken(Key::LeftShift) == "LeftShift");
    assert(input::keyName(Key::LeftShift) != input::keyToken(Key::LeftShift));
    assert(input::keyName(Key::Slash) == "/");
    assert(input::keyToken(Key::Slash) == "Slash");
    assert(input::mouseName(MouseButton::Left) == "Left Button");
    assert(input::mouseToken(MouseButton::Left) == "MouseLeft");

    // token 必须能被自己的解析器读回来——这是"丢配置"那条失效模式的直接反面。
    for (const Key key : {Key::W, Key::LeftShift, Key::Slash, Key::Space, Key::F3}) {
        const auto token = input::bindingToToken(input::keyboard(key));
        const auto parsed = input::bindingFromToken(token);
        assert(parsed.has_value());
        assert(*parsed == input::keyboard(key));
    }
}

}  // namespace

int main() {
    testRebindWritesThroughSingleSource();
    testConflictDetection();
    testResetToDefaults();
    testCaptureToggle();
    testKeyBindsPageListsBindRows();
    testTranslationKeys();
    testDisplayNamesAndTokensAreSeparate();
    testRowLabelsComeAsOneValue();
    testPageRowToRebind();
    testDefaultBindingIsTheSameSourceAsDefaults();
    testResetOneAndIsDefault();
    testUnbind();
    testConflictsCollectAll();
    testReportableConflictPredicate();
    testNoDuplicateDefaults();
    testCategoryGrouping();
    return 0;
}

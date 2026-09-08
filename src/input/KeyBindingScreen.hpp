#pragma once

// PX-5: the Key Binds screen's rebinding logic, as a Vulkan-free state object
// over the PX-1 InputSystem — the SINGLE SOURCE of bindings. Clicking a row
// begins a capture; the next key/mouse press is written straight into the
// InputSystem's binding table via rebind(), so the game's actual controls change
// (there is no private copy). A rebind that collides with another action's
// binding is reported so the page can warn; resetting restores the defaults.
//
// UI-6c 补齐了 26.1 那一行的另外两个元素所需要的 input 能力：逐项默认值/逐项重置
// （resetOne / isDefault / anyNonDefault）、解绑（applyUnbound）、以及「本行与
// 哪些行冲突」的**全集**（conflictsFor，不是只报第一个）。
//
// Headless-testable: construct with an InputSystem, beginCapture(action),
// applyKey(Key) — then assert system.bindings() changed, that a conflict is
// flagged, and that resetToDefaults() restores the vanilla layout. No GLFW, no
// Vulkan, no window.

#include "input/InputAction.hpp"
#include "input/InputBinding.hpp"
#include "input/InputNaming.hpp"
#include "input/InputSystem.hpp"

#include <array>
#include <cstddef>
#include <optional>

namespace mc::input {

// The result of applying a captured control to the action being rebound.
//
// `conflictingAction` 只是冲突集合的**第一个**，留着是因为「刚改完这一下撞了谁」
// 只需要一个名字。整行的黄色警告与 tooltip 必须走 conflictsFor()——26.1 的
// KeyBindsList.KeyEntry.refreshEntry 是给**每一行**都重算一遍冲突集合的
// （KeyBindsList.java:156-182），不是只看刚改的那一行。
struct RebindResult final {
    bool applied = false;               // the binding was written
    bool conflict = false;              // another action already used this control
    InputAction conflictingAction = InputAction::Count;  // valid only if conflict
};

// 一行的冲突集合。26.1 把**所有**与本行同键的动作名拼成
// `controls.keybinds.duplicateKeybinds` 的 tooltip（"This key is also used for:\n%s"，
// en_us.json:3187；拼接在 KeyBindsList.java:160-171），所以只报第一个是不够的。
//
// 定长内联数组，不分配：上限就是可重绑行数（自己不算，所以实际最多 N-1）。
struct ConflictList final {
    std::array<InputAction, kInputActionCount> actions{};
    std::size_t count = 0;

    [[nodiscard]] bool empty() const noexcept { return count == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return count; }
    [[nodiscard]] InputAction operator[](std::size_t i) const noexcept { return actions[i]; }
    [[nodiscard]] const InputAction* begin() const noexcept { return actions.data(); }
    [[nodiscard]] const InputAction* end() const noexcept { return actions.data() + count; }

    void push(InputAction action) noexcept {
        if (count < actions.size()) {
            actions[count++] = action;
        }
    }
};

// 冲突判定的纯谓词：给定「本行的绑定 + 本行是不是默认」和「另一行的绑定 + 另一行
// 是不是默认」，这一对该不该报冲突。逐条对 KeyBindsList.java:161-163，编号见
// KeyBindingScreen::conflictsFor 上面那段注释。
//
// ★ 为什么单独抽出来而不是直接写在循环里：第 4 档（两边都还是出厂默认就不报）在
//   本作**今天恒不生效**——本作的 defaults() 里没有两个动作共用同一个控件
//   （见 tests/key_binding_test.cpp 的 testNoDuplicateDefaults；vanilla 是有的，
//   比如 Options.java:616/:643 的鼠标中键），所以它藏在 conflictsFor 里就等于
//   没有任何断言盯着它，改坏了也不会红。抽成纯函数以后四种组合都能直接断言。
//   这一档不能删：它是 vanilla 的规则，而本作的默认表随时可能长出重复默认
//   （对齐 26.1 的过程中迟早会），到那天规则得已经在这里。
[[nodiscard]] constexpr bool isReportableConflict(const InputBinding& own, bool ownIsDefault,
                                                  const InputBinding& other,
                                                  bool otherIsDefault) noexcept {
    if (own.device == InputDevice::None) {  // 1. 未绑定的行不参与冲突
        return false;
    }
    if (other != own) {  // 3. same(): 同一个物理控件才算
        return false;
    }
    if (ownIsDefault && otherIsDefault) {  // 4. 两边都是出厂默认 -> 不是玩家撞出来的
        return false;
    }
    return true;
}

// Owns only the transient "which action is capturing" state; the bindings live
// in the InputSystem it points at. The screen never copies the table.
class KeyBindingScreen final {
  public:
    explicit KeyBindingScreen(InputSystem& system) noexcept : system_{&system} {}

    // Whether a row is currently waiting for the next key press.
    [[nodiscard]] bool capturing() const noexcept { return capturing_.has_value(); }
    [[nodiscard]] InputAction capturingAction() const noexcept {
        return capturing_.value_or(InputAction::Count);
    }

    // Begin (or cancel) capturing for an action's row. Clicking the same row again
    // cancels; clicking a different row moves the capture.
    void beginCapture(InputAction action) noexcept {
        if (capturing_.has_value() && *capturing_ == action) {
            capturing_.reset();  // toggle off
        } else {
            capturing_ = action;
        }
    }

    void cancelCapture() noexcept { capturing_.reset(); }

    // The binding an action currently points at, read straight from the source.
    [[nodiscard]] InputBinding bindingOf(InputAction action) const noexcept {
        return system_->bindings().binding(action);
    }

    // Apply an arbitrary captured control to the capturing row. If nothing is
    // capturing this is a no-op. Detects a conflict (the control already belongs
    // to another action) but still applies — vanilla rebinds and shows the clash;
    // the caller may then choose to also unbind the loser. Ends the capture.
    RebindResult applyBinding(InputBinding binding) noexcept {
        RebindResult result;
        if (!capturing_.has_value()) {
            return result;
        }
        const InputAction target = *capturing_;
        // 先写进单一源，再算冲突：冲突规则里有「本行是不是还等于默认值」这一档
        // （见 conflictsFor），所以必须在新绑定生效之后才算得对。
        system_->rebind(target, binding);
        result.applied = true;
        capturing_.reset();
        // 冲突判定只有一份（conflictsFor），这里不再写第二遍循环——两份规则会漂：
        // 界面上那一行是黄的、而刚改完的返回值说没冲突（或者反过来）。
        const ConflictList conflicts = conflictsFor(target);
        if (!conflicts.empty()) {
            result.conflict = true;
            result.conflictingAction = conflicts[0];
        }
        return result;
    }

    // Convenience overloads for the two device families the screen captures.
    RebindResult applyKey(Key key) noexcept { return applyBinding(keyboard(key)); }
    RebindResult applyMouse(MouseButton button) noexcept { return applyBinding(mouse(button)); }

    // 解绑：等待按键时按 Esc，26.1 是**解除这一行的绑定**，不是取消本次改键：
    //     if (event.isEscape()) { this.selectedKey.setKey(InputConstants.UNKNOWN); }
    //     else                  { this.selectedKey.setKey(InputConstants.getKey(event)); }
    // （KeyBindsScreen.java:71-86，两个分支之后都会 selectedKey = null 并刷新列表。）
    //
    // ★ 做成「捕获流程里的一个动词」而不是自由函数 unbind(action)，理由是 vanilla
    //   里解绑**只能**发生在正在改的那一行上：26.1 的界面根本没有「解绑此行」的按钮，
    //   Esc 也只有在 selectedKey != null 时才走这条分支。写成 applyBinding 的一个
    //   包装，就自动继承了「没在捕获就是 no-op」的守卫、以及「结束捕获」的收尾，
    //   界面没法解绑一个没被选中的行。
    //   顺带：applyBinding 里对 None 的冲突守卫（见 conflictsFor 的第一条）不要动——
    //   它保证多个未绑定的动作**不算**互相冲突，否则一解绑就会有一堆行同时变黄。
    RebindResult applyUnbound() noexcept { return applyBinding(InputBinding{}); }

    // Restore the vanilla defaults through the single source, ending any capture.
    void resetToDefaults() noexcept {
        system_->setBindings(BindingTable::defaults());
        capturing_.reset();
    }

    // 逐项重置：只把这一个动作写回它的出厂默认，别的一律不动。
    // 26.1 的那个 50 宽 Reset 按钮就是这一下：`key.setKey(key.getDefaultKey())`
    // （KeyBindsList.java:121-124）。
    //
    // 不动捕获状态：26.1 里只要 selectedKey != null，任何鼠标点击都会被
    // KeyBindsScreen.mouseClicked 截走去绑定那个鼠标键（KeyBindsScreen.java:59-68），
    // 玩家根本点不到 Reset 按钮，所以「重置时该不该取消捕获」在 vanilla 里是个
    // 不存在的状态，这里也就不替它发明一个语义。
    void resetOne(InputAction action) noexcept {
        system_->rebind(action, BindingTable::defaultBinding(action));
    }

    // 这一行还是不是出厂默认。26.1 用它给逐项 Reset 按钮置灰：
    //     resetButton.active = !this.key.isDefault();      KeyBindsList.java:158
    //     isDefault() { return this.key.equals(this.defaultKey); }  KeyMapping.java:178-180
    [[nodiscard]] bool isDefault(InputAction action) const noexcept {
        return bindingOf(action) == BindingTable::defaultBinding(action);
    }

    // 有没有任何一行被改过——底部那个「重置所有」按钮的置灰条件：
    //     for (key : keyMappings) if (!key.isDefault()) { canReset = true; break; }
    //     this.resetButton.active = canReset;              KeyBindsScreen.java:90-100
    // 只看列出来的那些行（vanilla 遍历的正是 options.keyMappings），所以不可重绑的
    // Pause 不参与。
    [[nodiscard]] bool anyNonDefault() const noexcept {
        for (const InputAction action : keyBindRows()) {
            if (!isDefault(action)) {
                return true;
            }
        }
        return false;
    }

    // 某个动作当前与**哪些**动作冲突，全部返回。逐条对
    // KeyBindsList.KeyEntry.refreshEntry（KeyBindsList.java:156-182）：
    //
    //   1. `if (!this.key.isUnbound())`（:161）——未绑定的行整个跳过。这就是
    //      「多个 Not Bound 不算互相冲突」的来源：解绑三行不会让它们一起变黄。
    //   2. `otherKey != this.key`（:163）——不跟自己比。
    //   3. `this.key.same(otherKey)`（:163）——`same` 就是 `this.key.equals(that.key)`
    //      （KeyMapping.java:156-158），即同一个物理控件。
    //   4. `(!otherKey.isDefault() || !this.key.isDefault())`（:163）——★ 最后这一句
    //      **不是**「随便两个同键就算冲突」。它的意思是：两边**都还停在出厂默认**上
    //      的撞键，不报。因为 26.1 自己就发了一批默认就相撞的键——
    //      keyPickItem 与 keySpectatorHotbar 同为鼠标中键（Options.java:616 / :643）、
    //      keySaveHotbarActivator 与 keyDebugCrash 同为 C（:640 / :646）、
    //      keyDebugOverlay 与 keyDebugModifier 同为 F3（:644 / :645）。
    //      若不加这一句，全新安装打开按键设置就会有好几行挂着黄条和 tooltip。
    //      换句话说：只有**玩家自己改出来的**撞键才值得警告；只要有一边被改过，
    //      这一对就重新算冲突（包括「另一边是默认、我改过来撞了它」这种）。
    //
    // 遍历的是 keyBindRows()——vanilla 遍历的是 options.keyMappings，也就是界面上
    // 列出来的那一批，两者是同一个集合。因此不可重绑的 Pause 既不会被撞，也撞不了别人。
    // 返回顺序是行序（vanilla 是 keyMappings 的声明序），tooltip 里的先后与 26.1
    // 不必逐字相同，这一点已登记。
    [[nodiscard]] ConflictList conflictsFor(InputAction action) const noexcept {
        ConflictList out;
        const InputBinding own = bindingOf(action);
        // 第 1 档（未绑定的行整个跳过）**不在这里**再写一遍：它是
        // isReportableConflict 的第一句。vanilla 是把它写成包住循环的
        // `if (!this.key.isUnbound())`（KeyBindsList.java:161），照抄成一个早退
        // 会让同一条规则有两个家——改坏其中一个，另一个把它盖住，测试就抓不到了
        // （这正是第一轮 sabotage 的实测结果）。少走 24 次循环不值这个代价。
        const bool ownIsDefault = isDefault(action);
        for (const InputAction other : keyBindRows()) {
            if (other == action) {  // 2. 不跟自己比（身份，不是绑定值）
                continue;
            }
            if (isReportableConflict(own, ownIsDefault, bindingOf(other), isDefault(other))) {
                out.push(other);
            }
        }
        return out;
    }

  private:
    InputSystem* system_;
    std::optional<InputAction> capturing_{};
};

}  // namespace mc::input

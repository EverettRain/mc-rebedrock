#pragma once

// PX-5: display names for the Key Binds screen. Vulkan-free and GLFW-free — the
// Controls page lists one row per rebindable InputAction, showing the action's
// label and the physical control currently bound to it. The renderer draws these
// strings; nothing here touches Vulkan, so the naming + the ordering of the
// rebindable rows is headless-testable.
//
// The `*DisplayName` functions are English identifiers and stay that way: they are
// the FALLBACK when a translation is missing, and they are what the headless tests
// assert against. The `*TranslationKey` functions beside them are the i18n layer
// the header note above used to promise — 26.1's own keys (`key.forward`,
// `key.keyboard.space`, `key.mouse.left`), resolved by the UI through the loaded
// language table.
//
// ★ 26.1 does NOT translate the plain printable keys. `InputConstants.Type.KEYSYM`
// asks GLFW for the system key name first and only falls back to
// `Component.translatable(name)` when GLFW has none:
//     systemName != null ? literal(systemName.toUpperCase()) : translatable(name)
// which is why vanilla's lang files carry `key.keyboard.space` but no
// `key.keyboard.w`. This header has no GLFW, so it hands out the translation key
// for every control and lets the missing entry fall back to the English name —
// the same "W" on screen, without dragging a window library into a headless
// header. The one difference: a resource pack that DID ship `key.keyboard.w`
// would be honoured here and ignored by vanilla. Registered, not fixed.
//
// Movement/attack/use/inventory/hotbar/drop/chat/etc. are rebindable;
// Pause (Escape) is intentionally NOT offered for rebinding (vanilla keeps the
// menu key fixed), which is why keyBindRows() lists a curated set.

#include "input/InputAction.hpp"
#include "input/InputBinding.hpp"

#include <array>
#include <string>
#include <string_view>

namespace mc::input {

[[nodiscard]] inline std::string_view actionDisplayName(InputAction action) noexcept {
    switch (action) {
        case InputAction::MoveForward: return "Forward";
        case InputAction::MoveBack: return "Back";
        case InputAction::MoveLeft: return "Left";
        case InputAction::MoveRight: return "Right";
        case InputAction::Jump: return "Jump";
        case InputAction::Sneak: return "Sneak";
        case InputAction::Sprint: return "Sprint";
        case InputAction::Attack: return "Attack / Destroy";
        case InputAction::Use: return "Use Item / Place";
        case InputAction::Inventory: return "Inventory";
        case InputAction::Hotbar1: return "Hotbar Slot 1";
        case InputAction::Hotbar2: return "Hotbar Slot 2";
        case InputAction::Hotbar3: return "Hotbar Slot 3";
        case InputAction::Hotbar4: return "Hotbar Slot 4";
        case InputAction::Hotbar5: return "Hotbar Slot 5";
        case InputAction::Hotbar6: return "Hotbar Slot 6";
        case InputAction::Hotbar7: return "Hotbar Slot 7";
        case InputAction::Hotbar8: return "Hotbar Slot 8";
        case InputAction::Hotbar9: return "Hotbar Slot 9";
        case InputAction::DropItem: return "Drop Item";
        case InputAction::Chat: return "Open Chat";
        case InputAction::Command: return "Open Command";
        case InputAction::Perspective: return "Toggle Perspective";
        case InputAction::Debug: return "Debug Info";
        case InputAction::Pause: return "Pause / Menu";
        case InputAction::Count: break;
    }
    return "?";
}

// 26.1 的 KeyMapping 名字，逐条对应 `Options.java` 里那批 `new KeyMapping("key.…")`。
// 本作有而 vanilla 没有的可绑定动作（Debug）走本项目自有的 `key.rebedrock.*`
// 命名空间（`lang/rebedrock/`），因为它不在任何 vanilla 语言表里。
[[nodiscard]] inline std::string_view actionTranslationKey(InputAction action) noexcept {
    switch (action) {
        case InputAction::MoveForward: return "key.forward";
        case InputAction::MoveBack: return "key.back";
        case InputAction::MoveLeft: return "key.left";
        case InputAction::MoveRight: return "key.right";
        case InputAction::Jump: return "key.jump";
        case InputAction::Sneak: return "key.sneak";
        case InputAction::Sprint: return "key.sprint";
        case InputAction::Attack: return "key.attack";
        case InputAction::Use: return "key.use";
        case InputAction::Inventory: return "key.inventory";
        case InputAction::Hotbar1: return "key.hotbar.1";
        case InputAction::Hotbar2: return "key.hotbar.2";
        case InputAction::Hotbar3: return "key.hotbar.3";
        case InputAction::Hotbar4: return "key.hotbar.4";
        case InputAction::Hotbar5: return "key.hotbar.5";
        case InputAction::Hotbar6: return "key.hotbar.6";
        case InputAction::Hotbar7: return "key.hotbar.7";
        case InputAction::Hotbar8: return "key.hotbar.8";
        case InputAction::Hotbar9: return "key.hotbar.9";
        case InputAction::DropItem: return "key.drop";
        case InputAction::Chat: return "key.chat";
        case InputAction::Command: return "key.command";
        case InputAction::Perspective: return "key.togglePerspective";
        // vanilla 的 F3 是固定键、不可重绑，所以没有对应的 KeyMapping 名字
        case InputAction::Debug: return "key.rebedrock.debug";
        case InputAction::Pause: return "key.rebedrock.pause";
        case InputAction::Count: break;
    }
    return "";
}

[[nodiscard]] inline std::string_view keyName(Key key) noexcept {
    switch (key) {
        case Key::W: return "W";
        case Key::A: return "A";
        case Key::S: return "S";
        case Key::D: return "D";
        case Key::Q: return "Q";
        case Key::E: return "E";
        case Key::T: return "T";
        case Key::Space: return "Space";
        case Key::LeftShift: return "Left Shift";
        case Key::LeftControl: return "Left Ctrl";
        case Key::Escape: return "Escape";
        case Key::Enter: return "Enter";
        case Key::Tab: return "Tab";
        case Key::Backspace: return "Backspace";
        case Key::Slash: return "/";
        case Key::F3: return "F3";
        case Key::F5: return "F5";
        case Key::Digit1: return "1";
        case Key::Digit2: return "2";
        case Key::Digit3: return "3";
        case Key::Digit4: return "4";
        case Key::Digit5: return "5";
        case Key::Digit6: return "6";
        case Key::Digit7: return "7";
        case Key::Digit8: return "8";
        case Key::Digit9: return "9";
        case Key::Unknown: break;
    }
    return "Not Bound";
}

// `InputConstants.Key` 的名字：`key.keyboard.<名>`。左右修饰键中间还有一段
// （`key.keyboard.left.shift`），数字键是裸数字（`key.keyboard.1`）。
[[nodiscard]] inline std::string_view keyTranslationKey(Key key) noexcept {
    switch (key) {
        case Key::W: return "key.keyboard.w";
        case Key::A: return "key.keyboard.a";
        case Key::S: return "key.keyboard.s";
        case Key::D: return "key.keyboard.d";
        case Key::Q: return "key.keyboard.q";
        case Key::E: return "key.keyboard.e";
        case Key::T: return "key.keyboard.t";
        case Key::Space: return "key.keyboard.space";
        case Key::LeftShift: return "key.keyboard.left.shift";
        case Key::LeftControl: return "key.keyboard.left.control";
        case Key::Escape: return "key.keyboard.escape";
        case Key::Enter: return "key.keyboard.enter";
        case Key::Tab: return "key.keyboard.tab";
        case Key::Backspace: return "key.keyboard.backspace";
        case Key::Slash: return "key.keyboard.slash";
        case Key::F3: return "key.keyboard.f3";
        case Key::F5: return "key.keyboard.f5";
        case Key::Digit1: return "key.keyboard.1";
        case Key::Digit2: return "key.keyboard.2";
        case Key::Digit3: return "key.keyboard.3";
        case Key::Digit4: return "key.keyboard.4";
        case Key::Digit5: return "key.keyboard.5";
        case Key::Digit6: return "key.keyboard.6";
        case Key::Digit7: return "key.keyboard.7";
        case Key::Digit8: return "key.keyboard.8";
        case Key::Digit9: return "key.keyboard.9";
        case Key::Unknown: break;
    }
    return "key.keyboard.unknown";
}

[[nodiscard]] inline std::string_view mouseName(MouseButton button) noexcept {
    switch (button) {
        case MouseButton::Left: return "Left Button";
        case MouseButton::Right: return "Right Button";
        case MouseButton::Middle: return "Middle Button";
        case MouseButton::Unknown: break;
    }
    return "Not Bound";
}

[[nodiscard]] inline std::string_view mouseTranslationKey(MouseButton button) noexcept {
    switch (button) {
        case MouseButton::Left: return "key.mouse.left";
        case MouseButton::Right: return "key.mouse.right";
        case MouseButton::Middle: return "key.mouse.middle";
        case MouseButton::Unknown: break;
    }
    return "key.keyboard.unknown";
}

// 一个绑定的翻译键。未绑定时与 vanilla 一样落在 `key.keyboard.unknown`（"未指定"）。
[[nodiscard]] inline std::string_view bindingTranslationKey(const InputBinding& binding) noexcept {
    switch (binding.device) {
        case InputDevice::Keyboard:
            return keyTranslationKey(static_cast<Key>(binding.code));
        case InputDevice::Mouse:
            return mouseTranslationKey(static_cast<MouseButton>(binding.code));
        case InputDevice::GamepadButton:
        case InputDevice::None:
            break;
    }
    return "key.keyboard.unknown";
}

// The physical control a binding currently points at, as a label for the row.
[[nodiscard]] inline std::string bindingDisplayName(const InputBinding& binding) {
    switch (binding.device) {
        case InputDevice::Keyboard:
            return std::string{keyName(static_cast<Key>(binding.code))};
        case InputDevice::Mouse:
            return std::string{mouseName(static_cast<MouseButton>(binding.code))};
        case InputDevice::GamepadButton:
            return "Gamepad";
        case InputDevice::None:
            break;
    }
    return "Not Bound";
}

// ---------------------------------------------------------------------------
// UI-6c：动作分类（26.1 的 KeyMapping.Category）
// ---------------------------------------------------------------------------
//
// 26.1 的按键列表按分类分组，每组前插一条标题行（KeyBindsList.java:30-46）。
// 分类之间的先后由 `Category.SORT_ORDER.indexOf` 决定（KeyMapping.java:143-149），
// 而 SORT_ORDER 就是 `register(...)` 的**声明顺序**（KeyMapping.java:199-221）——
// 不是字母序，也不是「常用在前」。所以下面这个枚举的**声明顺序本身就是组序**，
// 谁都不要按字母重排它。
enum class InputCategory : std::uint8_t {
    Movement = 0,  // KeyMapping.java:200
    Misc,          // KeyMapping.java:201  ← 注意 Misc 排在 Multiplayer/Gameplay **之前**
    Multiplayer,   // KeyMapping.java:202
    Gameplay,      // KeyMapping.java:203
    Inventory,     // KeyMapping.java:204
    Creative,      // KeyMapping.java:205  本作今天没有属于它的动作
    Spectator,     // KeyMapping.java:206  同上
    Debug,         // KeyMapping.java:207

    Count
};

inline constexpr std::size_t kInputCategoryCount = static_cast<std::size_t>(InputCategory::Count);

[[nodiscard]] constexpr std::size_t index(InputCategory category) noexcept {
    return static_cast<std::size_t>(category);
}

// 逐条对 `Options.java` 里那批 `new KeyMapping(..., Category.XXX)`。
//
// ★ Debug：本作的 F3 对应 vanilla 的 `keyDebugOverlay`（Options.java:644，
//   `Category.DEBUG`，键码 292 = F3）。26.1 里 DEBUG **是一个真的分类**，
//   且 keyDebugOverlay 确实在 `keyMappings` 里（Options.java:726），也就是说
//   vanilla 的按键设置界面本来就有「Debug」这一组。所以本作的 Debug 归 Debug，
//   不是 Misc——按名字猜成 Misc 会把它塞进「杂项」组，与 26.1 的分组不符。
[[nodiscard]] constexpr InputCategory actionCategory(InputAction action) noexcept {
    switch (action) {
        // Options.java:602-608（key.forward/left/back/right/jump/sneak/sprint）
        case InputAction::MoveForward:
        case InputAction::MoveBack:
        case InputAction::MoveLeft:
        case InputAction::MoveRight:
        case InputAction::Jump:
        case InputAction::Sneak:
        case InputAction::Sprint:
            return InputCategory::Movement;

        // Options.java:612-613（key.use / key.attack）——★ 不是 Movement 也不是 Misc
        case InputAction::Attack:
        case InputAction::Use:
            return InputCategory::Gameplay;

        // Options.java:609（key.inventory）、:611（key.drop）、:630-638（key.hotbar.N）
        // ★ Drop 在 vanilla 里属于 INVENTORY，不属于 Gameplay。
        case InputAction::Inventory:
        case InputAction::DropItem:
        case InputAction::Hotbar1:
        case InputAction::Hotbar2:
        case InputAction::Hotbar3:
        case InputAction::Hotbar4:
        case InputAction::Hotbar5:
        case InputAction::Hotbar6:
        case InputAction::Hotbar7:
        case InputAction::Hotbar8:
        case InputAction::Hotbar9:
            return InputCategory::Inventory;

        // Options.java:617（key.chat）、:619（key.command）
        // ★ 聊天在 vanilla 里属于 MULTIPLAYER，不属于 Misc。
        case InputAction::Chat:
        case InputAction::Command:
            return InputCategory::Multiplayer;

        // Options.java:622（key.togglePerspective）
        case InputAction::Perspective:
            return InputCategory::Misc;

        // Options.java:644（key.debug.overlay，Category.DEBUG）
        case InputAction::Debug:
            return InputCategory::Debug;

        // Pause（Esc）在 vanilla 里根本不是一个 KeyMapping，没有分类可对。它也不在
        // keyBindRows() 里，所以这条分支只是为了让函数对整个枚举全覆盖；给它 Misc
        // 是个不会被界面看见的占位。
        case InputAction::Pause:
            return InputCategory::Misc;

        case InputAction::Count:
            break;
    }
    return InputCategory::Misc;
}

// 分类标题的翻译键。
//
// ★ 不是 `key.categories.movement`。26.1 的 `Category.label()` 是
//   `id.toLanguageKey("key.category")`（KeyMapping.java:223-225），而
//   `Identifier.toLanguageKey()` = `namespace + "." + path`（Identifier.java:159-161），
//   分类 id 由 `Identifier.withDefaultNamespace(name)` 造出（KeyMapping.java:209-211），
//   所以实际的键是 **`key.category.minecraft.movement`**。
//   旧的 `key.categories.*` 在 26.1 的语言表里**也还在**（en_us.json:4987-4994，
//   给模组留的遗留键），照着它写不会崩、只会整组标题回落成英文兜底——正是最难
//   一眼看出来的那种错。逐条核对过 en_us.json:4995-5002。
[[nodiscard]] constexpr std::string_view categoryTranslationKey(InputCategory category) noexcept {
    switch (category) {
        case InputCategory::Movement: return "key.category.minecraft.movement";      // en_us.json:5000
        case InputCategory::Misc: return "key.category.minecraft.misc";              // en_us.json:4999
        case InputCategory::Multiplayer: return "key.category.minecraft.multiplayer";// en_us.json:5001
        case InputCategory::Gameplay: return "key.category.minecraft.gameplay";      // en_us.json:4997
        case InputCategory::Inventory: return "key.category.minecraft.inventory";    // en_us.json:4998
        case InputCategory::Creative: return "key.category.minecraft.creative";      // en_us.json:4995
        case InputCategory::Spectator: return "key.category.minecraft.spectator";    // en_us.json:5002
        case InputCategory::Debug: return "key.category.minecraft.debug";            // en_us.json:4996
        case InputCategory::Count: break;
    }
    return "";
}

// 翻译缺失时的英文兜底，取 26.1 en_us 的原文（不是枚举名——Misc 显示的是
// "Miscellaneous"，Creative 显示的是 "Creative Mode"）。
[[nodiscard]] constexpr std::string_view categoryDisplayName(InputCategory category) noexcept {
    switch (category) {
        case InputCategory::Movement: return "Movement";        // en_us.json:5000
        case InputCategory::Misc: return "Miscellaneous";       // en_us.json:4999
        case InputCategory::Multiplayer: return "Multiplayer";  // en_us.json:5001
        case InputCategory::Gameplay: return "Gameplay";        // en_us.json:4997
        case InputCategory::Inventory: return "Inventory";      // en_us.json:4998
        case InputCategory::Creative: return "Creative Mode";   // en_us.json:4995
        case InputCategory::Spectator: return "Spectator";      // en_us.json:5002
        case InputCategory::Debug: return "Debug";              // en_us.json:4996
        case InputCategory::Count: break;
    }
    return "?";
}

// ---------------------------------------------------------------------------
// 组内排序用的名字
// ---------------------------------------------------------------------------
//
// 26.1 组内按**本地化之后的显示名**排序：
//     I18n.get(this.name).compareTo(I18n.get(o.name))   —— KeyMapping.java:145
// 也就是说 vanilla 的组内顺序**随语言变**。本作的 keyBindRows() 是 constexpr、
// input 层也没有语言表，做不到「随语言重排」，所以这里把顺序钉在 26.1 的 en_us
// 排序上（默认语言，也是绝大多数人看到的那一版）。这是一处**已登记的偏差**：
// 切成中文时 vanilla 会重排、本作不会。
//
// ★ 为什么不用 actionDisplayName()：那是本作自己的短标签（"Forward"），只在翻译
//   缺失时出现在屏幕上；真机加载了 vanilla 语言表以后，屏幕上是 "Walk Forward"，
//   而决定 vanilla 顺序的正是后者。拿 "Forward" 排会排出一个和 26.1 完全不同的
//   组内序（Back/Forward/Jump/… vs Jump/Sneak/Sprint/Strafe…）。
//   这两张英文表内容不同是有意的，不是抄重了。
[[nodiscard]] constexpr std::string_view actionSortName(InputAction action) noexcept {
    switch (action) {
        case InputAction::MoveForward: return "Walk Forward";           // en_us.json:5028
        case InputAction::MoveBack: return "Walk Backward";             // en_us.json:4986
        case InputAction::MoveLeft: return "Strafe Left";               // en_us.json:5126
        case InputAction::MoveRight: return "Strafe Right";             // en_us.json:5135
        case InputAction::Jump: return "Jump";                          // en_us.json:5040
        case InputAction::Sneak: return "Sneak";                        // en_us.json:5139
        case InputAction::Sprint: return "Sprint";                      // en_us.json:5143
        case InputAction::Attack: return "Attack/Destroy";              // en_us.json:4985
        case InputAction::Use: return "Use Item/Place Block";           // en_us.json:5148
        case InputAction::Inventory: return "Open/Close Inventory";     // en_us.json:5039
        case InputAction::Hotbar1: return "Hotbar Slot 1";              // en_us.json:5030
        case InputAction::Hotbar2: return "Hotbar Slot 2";              // en_us.json:5031
        case InputAction::Hotbar3: return "Hotbar Slot 3";              // en_us.json:5032
        case InputAction::Hotbar4: return "Hotbar Slot 4";              // en_us.json:5033
        case InputAction::Hotbar5: return "Hotbar Slot 5";              // en_us.json:5034
        case InputAction::Hotbar6: return "Hotbar Slot 6";              // en_us.json:5035
        case InputAction::Hotbar7: return "Hotbar Slot 7";              // en_us.json:5036
        case InputAction::Hotbar8: return "Hotbar Slot 8";              // en_us.json:5037
        case InputAction::Hotbar9: return "Hotbar Slot 9";              // en_us.json:5038
        case InputAction::DropItem: return "Drop Selected Item";        // en_us.json:5027
        case InputAction::Chat: return "Open Chat";                     // en_us.json:5003
        case InputAction::Command: return "Open Command";               // en_us.json:5004
        case InputAction::Perspective: return "Toggle Perspective";     // en_us.json:5146
        // 本作自有的动作：vanilla 的 key.debug.overlay 是 "Toggle Overlay"，但那是
        // 另一个东西的名字。Debug 是 Debug 组里**唯一**一行，排序名排不到任何人，
        // 用本作自己的标签即可。
        case InputAction::Debug: return "Debug Info";
        case InputAction::Pause: return "Pause / Menu";
        case InputAction::Count: break;
    }
    return "";
}

// 两行之间的先后：先比分类在 SORT_ORDER 里的位置，同组再比显示名。
// 对应 KeyMapping.compareTo（KeyMapping.java:143-149）。
//
// vanilla 在「同组」时还先比了一个 `order` 字段（KeyMapping.java:145）——那是给
// F3 调试子键排序用的（Options.java:644-645 的 -2/-1，:670-673 的 1..4）。本作
// 一整个 Debug 组只有一行，任何一组里也没有第二个非零 order 的成员，所以那一档
// 比较在这里恒不生效，没有为它加一个没有消费者的 actionOrder()。
[[nodiscard]] constexpr bool keyBindRowLess(InputAction a, InputAction b) noexcept {
    const std::size_t categoryA = index(actionCategory(a));
    const std::size_t categoryB = index(actionCategory(b));
    if (categoryA != categoryB) {
        return categoryA < categoryB;
    }
    return actionSortName(a) < actionSortName(b);
}

// 可重绑动作的花名册——**未排序**，只是「有哪些行」。Pause 不在内（菜单/Esc 键
// 固定不可重绑，与 vanilla 一致：Esc 在 26.1 的按键设置里是解绑手势，不是一行）。
// 加一个可重绑动作只需要往这里加一项，顺序由 keyBindRows() 算。
inline constexpr std::array<InputAction, 24> kRebindableActions = {
    InputAction::MoveForward, InputAction::MoveBack,  InputAction::MoveLeft,
    InputAction::MoveRight,   InputAction::Jump,      InputAction::Sneak,
    InputAction::Sprint,      InputAction::Attack,    InputAction::Use,
    InputAction::Inventory,   InputAction::DropItem,  InputAction::Chat,
    InputAction::Command,     InputAction::Perspective, InputAction::Debug,
    InputAction::Hotbar1,     InputAction::Hotbar2,   InputAction::Hotbar3,
    InputAction::Hotbar4,     InputAction::Hotbar5,   InputAction::Hotbar6,
    InputAction::Hotbar7,     InputAction::Hotbar8,   InputAction::Hotbar9,
};

// 按 26.1 的分组排序之后的行序（`Arrays.sort(keyMappings)`，KeyBindsList.java:29）。
//
// 返回的仍然是**扁平**的动作数组：分类标题行是界面侧的事（vanilla 也是在
// KeyBindsList 的构造里，边遍历排好序的数组边插 CategoryEntry，
// KeyBindsList.java:30-46），input 层不产出标题项、不改返回类型。
// 界面要分组，只需在相邻两项的 actionCategory() 不同处插一条标题。
//
// 顺序是**算出来的**不是手抄的：改 actionCategory() 或 actionSortName() 会直接
// 改变这个函数的返回值，所以那两张表是可断言的。
[[nodiscard]] inline constexpr std::array<InputAction, kRebindableActions.size()> keyBindRows() noexcept {
    auto rows = kRebindableActions;
    // 插入排序：std::sort 要到 C++20 才 constexpr，而且这里只有 24 项，手写一遍
    // 比把这一层的 constexpr 能力绑在标准库实现上更稳，也顺带保证了稳定性。
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const InputAction value = rows[i];
        std::size_t j = i;
        while (j > 0 && keyBindRowLess(value, rows[j - 1])) {
            rows[j] = rows[j - 1];
            --j;
        }
        rows[j] = value;
    }
    return rows;
}

}  // namespace mc::input

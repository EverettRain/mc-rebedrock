#pragma once

// UI-1: the one text input widget. Editing semantics only — no Vulkan, no GLFW,
// no font, no clock, no static state. Every action is a pure function that eats
// (state, rules, metrics, action) and produces a new state, so the whole surface
// is pinned by headless assertions instead of by "it looked right on screen".
//
// It exists because the two ad-hoc input handlers it replaces lived inside
// VulkanRenderer.cpp — a Vulkan translation unit, therefore untestable — and
// each had grown its own half of an EditBox: backspace and append, and nothing
// else. Vanilla's own EditBox is a long-standing bug nest precisely where the
// cursor, the selection and the horizontal scroll interact, which is the part
// neither copy had at all.
//
// Reference: 26.1 client/net/minecraft/client/gui/components/EditBox.java.
// Two deliberate divergences from it are marked DIVERGENCE below.
//
// INDICES ARE CHARACTERS, NOT BYTES. `value` is UTF-8; cursor, highlight and
// displayStart all count codepoints. A backspace therefore removes a whole
// codepoint — the first thing that breaks once the old `codepoint >= 32 &&
// <= 126` filter (which threw away accented letters and all of CJK, though the
// font system has supported them since it grew its unicode pages) is gone.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace mc::ui {

// The whole editable state of one field. Value semantics, copyable, no identity:
// which field has the keyboard is the CALLER's business, not the widget's.
struct TextFieldState final {
    std::string value;              // UTF-8
    std::size_t cursor = 0;         // in characters
    std::size_t highlight = 0;      // the other end of the selection; == cursor means none
    std::size_t displayStart = 0;   // first visible character (horizontal scroll)
};

// One field's own constraints. A plain function pointer, not std::function: a
// filter is a leaf predicate with no state to capture.
using TextFieldFilter = bool (*)(char32_t);

struct TextFieldRules final {
    std::size_t maxLength = 32;             // in characters
    TextFieldFilter accepts = nullptr;      // null = accept anything typeable
    bool editable = true;
};

// Font metrics arrive injected, so this layer never sees a TextFont — that is
// what keeps it headless. `measure` returns the width of a UTF-8 substring in
// whatever unit `innerWidth` is expressed in. A null `measure` (or a
// non-positive innerWidth) means "unbounded": displayStart then stays 0 and the
// view shows the whole value, which is what the headless tests use when they are
// not asserting about scrolling.
struct TextFieldMetrics final {
    std::function<float(std::string_view)> measure;
    float innerWidth = 0.0F;
};

// The editing keys. Copy/cut/paste are NOT here: reaching the clipboard is the
// driver's job (glfwGet/SetClipboardString). The driver copies selectedText()
// out and pastes back in through textFieldApplyText(), so this layer stays free
// of the window system.
enum class TextFieldKey : std::uint8_t {
    Backspace,
    Delete,
    Left,
    Right,
    Home,
    End,
    SelectAll,
};

struct TextFieldModifiers final {
    bool shift = false;
    bool control = false;
};

// Ready-made filters. The LAN port box (GUI spec §9.2) takes digits only.
[[nodiscard]] bool textFieldDigitsOnly(char32_t codepoint);

// vanilla StringUtil.isAllowedChatCharacter: everything from U+0020 up except
// DEL and the section sign (which would open the door to formatting codes).
// Applied under every rules.accepts, so no field can smuggle a control
// character in.
[[nodiscard]] bool textFieldTypeable(char32_t codepoint);

// Seeds a field from a value the program supplies (a world name off disk, a
// prefilled port). Truncated to maxLength but NOT run through the filter, as in
// vanilla's setValue: a name already stored is shown as it is rather than
// silently mangled. Cursor lands at the end.
[[nodiscard]] TextFieldState textFieldWithValue(std::string_view value,
                                                const TextFieldRules& rules,
                                                const TextFieldMetrics& metrics);

// One typed character. A codepoint the filter rejects leaves the state BITWISE
// unchanged — it does not even collapse a selection.
[[nodiscard]] TextFieldState textFieldApplyChar(TextFieldState state, const TextFieldRules& rules,
                                                const TextFieldMetrics& metrics,
                                                char32_t codepoint);

// Insert (paste). Replaces the selection if there is one. Filtering happens
// FIRST and the maxLength truncation second, so characters the filter threw away
// never consumed any of the field's budget.
[[nodiscard]] TextFieldState textFieldApplyText(TextFieldState state, const TextFieldRules& rules,
                                                const TextFieldMetrics& metrics,
                                                std::string_view text);

// One editing key. Every key is a no-op on a field with editable == false.
// DIVERGENCE 1 from vanilla, which lets an uneditable box still be navigated and
// copied out of; a disabled field here is inert, which is what the anvil's
// greyed-out name box needs to mean.
[[nodiscard]] TextFieldState textFieldApplyKey(TextFieldState state, const TextFieldRules& rules,
                                               const TextFieldMetrics& metrics, TextFieldKey key,
                                               TextFieldModifiers modifiers);

// Puts the cursor at a character index (a completion that rewrites the line, and
// later a mouse click). Like every other action, a no-op on an uneditable field.
[[nodiscard]] TextFieldState textFieldMoveCursorTo(TextFieldState state,
                                                   const TextFieldRules& rules,
                                                   const TextFieldMetrics& metrics,
                                                   std::size_t charIndex, bool extendSelection);

// The selected substring, for the driver to hand to the clipboard.
[[nodiscard]] std::string textFieldSelectedText(const TextFieldState& state);

// Characters, not bytes.
[[nodiscard]] std::size_t textFieldCharCount(std::string_view text);

// Byte offset of a character index, for the callers that must speak bytes —
// command completion indexes the raw command line.
[[nodiscard]] std::size_t textFieldByteOffset(std::string_view text, std::size_t charIndex);

// Everything the draw side needs, computed once. Splitting this out keeps the
// renderer down to positioning quads, and lets the visible-window rules be
// asserted headless like the rest.
struct TextFieldView final {
    std::string visible;                // the substring that fits the field
    std::size_t visibleChars = 0;
    std::size_t cursorOffset = 0;       // character offset of the cursor within `visible`
    std::size_t selectionStart = 0;     // selection clamped into `visible`, in characters
    std::size_t selectionEnd = 0;
    bool cursorOnScreen = true;
    // vanilla's `insert`: a cursor in the middle of the text, or in a full
    // field, is a 1px bar; a cursor appending at the end is an underscore.
    bool insertCursor = false;
};

[[nodiscard]] TextFieldView textFieldView(const TextFieldState& state, const TextFieldRules& rules,
                                          const TextFieldMetrics& metrics);

// Text inset, in GUI pixels: 4 inside a bordered box (vanilla EditBox), 2 for a
// bare one. The editing side and the painter MUST agree on the visible width —
// displayStart is stored in the state, so a mismatch shows up as a window that
// scrolls to the wrong place — which is why the padding rule lives here rather
// than in either caller.
// EditBox.java:486 — `textX = getX() + (bordered ? 4 : 0)`, and :487's
// getInnerWidth() is `bordered ? width - 8 : width`. A BARE field insets by
// nothing at all: its frame comes from the screen's own art (the anvil's name
// plate, the chat backdrop), and that art already includes the padding, so
// insetting again pushes the text right of where vanilla puts it. This used to
// be 2, which showed up in play as the anvil's rename text sitting too far
// right.
inline constexpr float kTextFieldBorderedInset = 4.0F;
inline constexpr float kTextFieldBareInset = 0.0F;

[[nodiscard]] constexpr float textFieldTextInset(float scale, bool bordered) {
    return (bordered ? kTextFieldBorderedInset : kTextFieldBareInset) * scale;
}

[[nodiscard]] constexpr float textFieldInnerWidth(float fieldWidth, float scale, bool bordered) {
    return fieldWidth - 2.0F * textFieldTextInset(scale, bordered);
}

// Every place in the game where a player can type, and what each field accepts.
// The single-source rule of UI-1 is enforced here: a new typeable field adds a
// line to this table, and gets the editing semantics for free.
//
// Create/edit world name. 32 characters, as the old handler had it.
inline constexpr TextFieldRules kWorldNameFieldRules{32U, nullptr, true};
// Chat and the command line. 256, matching vanilla's chat length limit.
inline constexpr TextFieldRules kChatFieldRules{256U, nullptr, true};
// The anvil's rename box. vanilla AnvilScreen sets maxLength 50 — but the field
// stays NOT editable here, and that is deliberate: renaming needs a custom name
// stored on the ItemStack, and this build has nowhere to put one. See UI-1's
// card. Flipping this to true without that storage would let a player type a
// name the game then throws away.
// I-3 landed the storage, so the box is live: 50 characters, vanilla's
// `AnvilScreen` `setMaxLength(50)`. It still goes dead when the left slot is
// empty (vanilla's `slotChanged` calls `setEditable(!itemStack.isEmpty())`) —
// the caller picks between this and kAnvilNameFieldDisabled.
inline constexpr TextFieldRules kAnvilNameFieldRules{50U, nullptr, true};
inline constexpr TextFieldRules kAnvilNameFieldDisabled{50U, nullptr, false};
// Open-to-LAN's port box (GUI spec §9.2). The screen itself does not exist yet —
// LAN hosting is a deferred decision, not part of UI-1 — but the field's rules
// belong in this table now, so that screen has nothing to invent when it lands.
inline constexpr TextFieldRules kLanPortFieldRules{5U, textFieldDigitsOnly, true};

} // namespace mc::ui

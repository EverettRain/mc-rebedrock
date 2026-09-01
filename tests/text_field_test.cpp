#include "ui/TextField.hpp"

#include <cassert>
#include <string>
#include <string_view>

// UI-1's editing semantics, pinned headless. The layer is pure values with an
// injected width function, so nothing here needs a font, a window or a clock.
namespace {

using mc::ui::TextFieldKey;
using mc::ui::TextFieldMetrics;
using mc::ui::TextFieldModifiers;
using mc::ui::TextFieldRules;
using mc::ui::TextFieldState;

// Every codepoint is 6 wide, whatever its byte length — a clean grid, and it
// keeps the scrolling assertions arithmetic rather than font-dependent.
float sixPerCharacter(std::string_view piece) {
    return 6.0F * static_cast<float>(mc::ui::textFieldCharCount(piece));
}

// The default: no width bound at all, so displayStart never moves and the
// assertions below are about editing only.
const TextFieldMetrics kUnbounded{};

// A ten-character window, for the horizontal-scroll assertions.
const TextFieldMetrics kNarrow{sixPerCharacter, 60.0F};

const TextFieldRules kDefault{};

TextFieldState typed(TextFieldState state, const TextFieldRules& rules,
                     const TextFieldMetrics& metrics, std::string_view ascii) {
    for (const char character : ascii) {
        state = mc::ui::textFieldApplyChar(std::move(state), rules, metrics,
                                           static_cast<char32_t>(character));
    }
    return state;
}

TextFieldState key(TextFieldState state, const TextFieldRules& rules, TextFieldKey which,
                   bool shift = false, bool control = false) {
    return mc::ui::textFieldApplyKey(std::move(state), rules, kUnbounded, which,
                                     TextFieldModifiers{shift, control});
}

TextFieldState narrowKey(TextFieldState state, TextFieldKey which) {
    return mc::ui::textFieldApplyKey(std::move(state), kDefault, kNarrow, which,
                                     TextFieldModifiers{});
}

bool sameState(const TextFieldState& a, const TextFieldState& b) {
    return a.value == b.value && a.cursor == b.cursor && a.highlight == b.highlight &&
           a.displayStart == b.displayStart;
}

} // namespace

int main() {
    using mc::ui::textFieldApplyChar;
    using mc::ui::textFieldApplyKey;
    using mc::ui::textFieldApplyText;
    using mc::ui::textFieldCharCount;
    using mc::ui::textFieldSelectedText;
    using mc::ui::textFieldView;
    using mc::ui::textFieldWithValue;

    // ---- Non-ASCII survives, and one backspace takes one whole codepoint ----
    // The old handlers filtered to 32..126, so this string could not be typed at
    // all; with the filter gone, a byte-wise backspace would leave a truncated
    // sequence behind. U+4E2D is three bytes, U+00E9 two.
    {
        TextFieldState state = textFieldWithValue("a中é", kDefault, kUnbounded);
        assert(state.value == "a\xE4\xB8\xAD\xC3\xA9");
        assert(textFieldCharCount(state.value) == 3U);
        assert(state.cursor == 3U);

        state = key(std::move(state), kDefault, TextFieldKey::Backspace);
        assert(state.value == "a\xE4\xB8\xAD");
        assert(textFieldCharCount(state.value) == 2U);
        assert(state.value.size() == 4U); // four bytes: not a byte-wise delete

        state = key(std::move(state), kDefault, TextFieldKey::Backspace);
        assert(state.value == "a");
        assert(textFieldCharCount(state.value) == 1U);
        assert(state.value.size() == 1U); // the whole three-byte codepoint went
        assert(state.cursor == 1U && state.highlight == 1U);
    }

    // Typing a multibyte codepoint mid-string lands on a character boundary.
    {
        TextFieldState state = textFieldWithValue("ab", kDefault, kUnbounded);
        state = key(std::move(state), kDefault, TextFieldKey::Left);
        state = textFieldApplyChar(std::move(state), kDefault, kUnbounded, U'中');
        assert(state.value == "a\xE4\xB8\xAD" "b");
        assert(state.cursor == 2U);
    }

    // Delete (forward) is codepoint-wise too.
    {
        TextFieldState state = textFieldWithValue("中z", kDefault, kUnbounded);
        state = key(std::move(state), kDefault, TextFieldKey::Home);
        state = key(std::move(state), kDefault, TextFieldKey::Delete);
        assert(state.value == "z");
        assert(state.cursor == 0U);
    }

    // ---- Arrows, Home, End ----
    {
        TextFieldState state = textFieldWithValue("hello", kDefault, kUnbounded);
        assert(state.cursor == 5U);
        state = key(std::move(state), kDefault, TextFieldKey::Left);
        state = key(std::move(state), kDefault, TextFieldKey::Left);
        assert(state.cursor == 3U && state.highlight == 3U);
        state = key(std::move(state), kDefault, TextFieldKey::Home);
        assert(state.cursor == 0U);
        state = key(std::move(state), kDefault, TextFieldKey::Left); // clamps, no wrap
        assert(state.cursor == 0U);
        state = key(std::move(state), kDefault, TextFieldKey::End);
        assert(state.cursor == 5U);
        state = key(std::move(state), kDefault, TextFieldKey::Right); // clamps
        assert(state.cursor == 5U);
    }

    // ---- Shift+arrow builds a selection; a plain arrow drops it ----
    {
        TextFieldState state = textFieldWithValue("hello", kDefault, kUnbounded);
        state = key(std::move(state), kDefault, TextFieldKey::Left, /*shift=*/true);
        state = key(std::move(state), kDefault, TextFieldKey::Left, /*shift=*/true);
        assert(state.cursor == 3U && state.highlight == 5U);
        assert(textFieldSelectedText(state) == "lo");
        state = key(std::move(state), kDefault, TextFieldKey::Left);
        assert(state.cursor == 2U && state.highlight == 2U);
        assert(textFieldSelectedText(state).empty());
    }

    // Shift+Home / Shift+End select to the ends.
    {
        TextFieldState state = textFieldWithValue("hello", kDefault, kUnbounded);
        state = key(std::move(state), kDefault, TextFieldKey::Home);
        state = key(std::move(state), kDefault, TextFieldKey::End, /*shift=*/true);
        assert(textFieldSelectedText(state) == "hello");
    }

    // ---- Ctrl+A selects all ----
    {
        TextFieldState state = textFieldWithValue("hello", kDefault, kUnbounded);
        state = key(std::move(state), kDefault, TextFieldKey::Home);
        state = key(std::move(state), kDefault, TextFieldKey::SelectAll);
        assert(state.cursor == 5U && state.highlight == 0U);
        assert(textFieldSelectedText(state) == "hello");
    }

    // ---- With a selection: typing, pasting and backspace all delete it first ----
    {
        // Sabotage guard 3: select all, type one character, the value is that
        // one character — not "helloX".
        TextFieldState state = textFieldWithValue("hello", kDefault, kUnbounded);
        state = key(std::move(state), kDefault, TextFieldKey::SelectAll);
        state = textFieldApplyChar(std::move(state), kDefault, kUnbounded, U'X');
        assert(state.value == "X");
        assert(textFieldCharCount(state.value) == 1U);
        assert(state.cursor == 1U && state.highlight == 1U);
    }
    {
        TextFieldState state = textFieldWithValue("hello", kDefault, kUnbounded);
        state = key(std::move(state), kDefault, TextFieldKey::SelectAll);
        state = textFieldApplyText(std::move(state), kDefault, kUnbounded, "world!");
        assert(state.value == "world!");
    }
    {
        TextFieldState state = textFieldWithValue("hello", kDefault, kUnbounded);
        state = key(std::move(state), kDefault, TextFieldKey::Left, /*shift=*/true);
        state = key(std::move(state), kDefault, TextFieldKey::Left, /*shift=*/true);
        state = key(std::move(state), kDefault, TextFieldKey::Backspace);
        assert(state.value == "hel"); // the selection went, not one extra character
        assert(state.cursor == 3U);
    }
    {
        // Cut's editing half: the driver copies selectedText() out, then inserts
        // nothing.
        TextFieldState state = textFieldWithValue("hello", kDefault, kUnbounded);
        state = key(std::move(state), kDefault, TextFieldKey::SelectAll);
        assert(textFieldSelectedText(state) == "hello");
        state = textFieldApplyText(std::move(state), kDefault, kUnbounded, "");
        assert(state.value.empty() && state.cursor == 0U);
    }

    // ---- Ctrl+arrow jumps by word, Ctrl+Backspace/Delete removes one ----
    // vanilla getWordPosition: forward stops past the run of spaces after the
    // word; backward eats the spaces behind the cursor and then the word.
    {
        TextFieldState state = textFieldWithValue("alpha   beta gamma", kDefault, kUnbounded);
        state = key(std::move(state), kDefault, TextFieldKey::Home);
        state = key(std::move(state), kDefault, TextFieldKey::Right, false, /*control=*/true);
        assert(state.cursor == 8U); // past "alpha" AND the three spaces
        state = key(std::move(state), kDefault, TextFieldKey::Right, false, true);
        assert(state.cursor == 13U); // past "beta "
        state = key(std::move(state), kDefault, TextFieldKey::Left, false, true);
        assert(state.cursor == 8U);
        state = key(std::move(state), kDefault, TextFieldKey::Left, false, true);
        assert(state.cursor == 0U); // back over the spaces and "alpha"
    }
    {
        TextFieldState state = textFieldWithValue("alpha   beta", kDefault, kUnbounded);
        state = key(std::move(state), kDefault, TextFieldKey::Backspace, false, /*control=*/true);
        assert(state.value == "alpha   ");
        state = key(std::move(state), kDefault, TextFieldKey::Backspace, false, true);
        assert(state.value.empty()); // the trailing spaces and "alpha" in one press
    }
    {
        TextFieldState state = textFieldWithValue("alpha beta", kDefault, kUnbounded);
        state = key(std::move(state), kDefault, TextFieldKey::Home);
        state = key(std::move(state), kDefault, TextFieldKey::Delete, false, /*control=*/true);
        assert(state.value == "beta");
    }
    // Ctrl+Backspace with a selection deletes the selection, not a word.
    {
        TextFieldState state = textFieldWithValue("alpha beta", kDefault, kUnbounded);
        state = key(std::move(state), kDefault, TextFieldKey::Left, /*shift=*/true);
        state = key(std::move(state), kDefault, TextFieldKey::Backspace, false, /*control=*/true);
        assert(state.value == "alpha bet");
    }

    // ---- maxLength counts characters, and truncates AFTER the filter ----
    {
        const TextFieldRules rules{4U, nullptr, true};
        // Four multibyte characters fit; the fifth does not.
        TextFieldState state;
        for (int index = 0; index < 6; ++index) {
            state = textFieldApplyChar(std::move(state), rules, kUnbounded, U'中');
        }
        assert(textFieldCharCount(state.value) == 4U);
        assert(state.value.size() == 12U); // characters, not bytes, are the budget
    }
    {
        // Sabotage guard 2: a filter that refuses letters, fed letters, must not
        // spend the budget. Truncating before filtering would leave room 0.
        const TextFieldRules digits{4U, mc::ui::textFieldDigitsOnly, true};
        TextFieldState state = typed(TextFieldState{}, digits, kUnbounded, "abcdefgh");
        assert(state.value.empty());
        state = textFieldApplyText(std::move(state), digits, kUnbounded, "abc123def456");
        assert(state.value == "1234"); // the letters cost nothing, the digits fill it
    }
    {
        // A paste longer than the room left is cut to the room left.
        const TextFieldRules rules{6U, nullptr, true};
        TextFieldState state = textFieldWithValue("ab", rules, kUnbounded);
        state = textFieldApplyText(std::move(state), rules, kUnbounded, "cdefghij");
        assert(state.value == "abcdef");
        assert(state.cursor == 6U);
    }
    {
        // Replacing a selection frees its room first, so a full field can still
        // take a paste of the same size.
        const TextFieldRules rules{5U, nullptr, true};
        TextFieldState state = textFieldWithValue("hello", rules, kUnbounded);
        state = key(std::move(state), rules, TextFieldKey::SelectAll);
        state = textFieldApplyText(std::move(state), rules, kUnbounded, "world");
        assert(state.value == "world");
    }

    // ---- A rejected character leaves the state untouched ----
    {
        const TextFieldRules digits{8U, mc::ui::textFieldDigitsOnly, true};
        TextFieldState state = textFieldWithValue("25565", digits, kUnbounded);
        state = key(std::move(state), digits, TextFieldKey::Home);
        state = key(std::move(state), digits, TextFieldKey::Right, /*shift=*/true);
        const TextFieldState before = state;
        assert(textFieldSelectedText(before) == "2");

        // Rejected: it must not even collapse the selection.
        state = textFieldApplyChar(std::move(state), digits, kUnbounded, U'x');
        assert(sameState(state, before));
        state = textFieldApplyChar(std::move(state), digits, kUnbounded, U'中');
        assert(sameState(state, before));

        // Accepted: now the selection goes.
        state = textFieldApplyChar(std::move(state), digits, kUnbounded, U'7');
        assert(state.value == "75565");
    }
    {
        // Control characters and the section sign are refused everywhere, filter
        // or no filter (vanilla StringUtil.isAllowedChatCharacter).
        TextFieldState state;
        state = textFieldApplyChar(std::move(state), kDefault, kUnbounded, U'\n');
        state = textFieldApplyChar(std::move(state), kDefault, kUnbounded, U'\t');
        state = textFieldApplyChar(std::move(state), kDefault, kUnbounded,
                                   static_cast<char32_t>(127));
        state = textFieldApplyChar(std::move(state), kDefault, kUnbounded,
                                   static_cast<char32_t>(167));
        assert(state.value.empty());
        // ...and a paste is filtered the same way, character by character.
        state = textFieldApplyText(std::move(state), kDefault, kUnbounded, "a\nb§c");
        assert(state.value == "abc");
    }

    // ---- editable == false makes every action a no-op ----
    {
        const TextFieldRules locked{32U, nullptr, /*editable=*/false};
        const TextFieldState before = textFieldWithValue("Sharp Sword", kDefault, kUnbounded);
        TextFieldState state = before;
        state = textFieldApplyChar(std::move(state), locked, kUnbounded, U'x');
        assert(sameState(state, before));
        state = textFieldApplyText(std::move(state), locked, kUnbounded, "paste");
        assert(sameState(state, before));
        for (const TextFieldKey which :
             {TextFieldKey::Backspace, TextFieldKey::Delete, TextFieldKey::Left,
              TextFieldKey::Right, TextFieldKey::Home, TextFieldKey::End,
              TextFieldKey::SelectAll}) {
            state = textFieldApplyKey(std::move(state), locked, kUnbounded, which,
                                      TextFieldModifiers{true, true});
            assert(sameState(state, before));
        }
    }

    // ---- displayStart keeps the cursor visible and leaves no dead window ----
    // kNarrow fits ten characters (60px / 6px).
    {
        TextFieldState state = textFieldWithValue("0123456789abcdef", kDefault, kNarrow);
        // Seeded at the end: the window shows the last ten characters.
        assert(state.cursor == 16U);
        assert(state.displayStart == 6U);
        {
            const auto view = textFieldView(state, kDefault, kNarrow);
            assert(view.visible == "6789abcdef");
            assert(view.cursorOnScreen && view.cursorOffset == 10U);
        }

        // Walking left past the left edge drags the window with it.
        for (int index = 0; index < 12; ++index) {
            state = narrowKey(std::move(state), TextFieldKey::Left);
        }
        assert(state.cursor == 4U);
        assert(state.displayStart == 4U);
        assert(textFieldView(state, kDefault, kNarrow).visible == "456789abcd");

        // Home shows the head.
        state = narrowKey(std::move(state), TextFieldKey::Home);
        assert(state.displayStart == 0U);
        // End shows the tail again, cursor still on screen.
        state = narrowKey(std::move(state), TextFieldKey::End);
        assert(state.displayStart == 6U);
        assert(textFieldView(state, kDefault, kNarrow).cursorOnScreen);
    }
    {
        // No dead window: with the view scrolled to the tail, deleting from the
        // end must pull displayStart back rather than leave blank space to the
        // right of the text.
        TextFieldState state = textFieldWithValue("0123456789abcdef", kDefault, kNarrow);
        assert(state.displayStart == 6U);
        for (int index = 0; index < 6; ++index) {
            state = narrowKey(std::move(state), TextFieldKey::Backspace);
        }
        assert(state.value == "0123456789");
        assert(state.displayStart == 0U);
        assert(textFieldView(state, kDefault, kNarrow).visible == "0123456789");
    }
    {
        // A value shorter than the window never scrolls.
        TextFieldState state = textFieldWithValue("abc", kDefault, kNarrow);
        assert(state.displayStart == 0U);
        const auto view = textFieldView(state, kDefault, kNarrow);
        assert(view.visible == "abc" && view.cursorOffset == 3U);
    }
    {
        // Ctrl+A on a long value keeps the cursor on screen (the deliberate
        // divergence from vanilla, which scrolls to the highlight instead).
        TextFieldState state = textFieldWithValue("0123456789abcdef", kDefault, kNarrow);
        state = narrowKey(std::move(state), TextFieldKey::SelectAll);
        assert(state.cursor == 16U && state.highlight == 0U);
        assert(textFieldView(state, kDefault, kNarrow).cursorOnScreen);
    }

    // ---- The view's selection window and cursor shape ----
    {
        TextFieldState state = textFieldWithValue("0123456789abcdef", kDefault, kNarrow);
        state = narrowKey(std::move(state), TextFieldKey::SelectAll);
        const auto view = textFieldView(state, kDefault, kNarrow);
        // The selection starts off screen to the left, so it clamps to the
        // window rather than reaching back past it.
        assert(view.selectionStart == 0U);
        assert(view.selectionEnd == view.visibleChars);
    }
    {
        // Cursor shape: appending at the end of a field with room is the
        // underscore; mid-text, or in a full field, it is the insert bar.
        const TextFieldRules rules{4U, nullptr, true};
        TextFieldState state = textFieldWithValue("ab", rules, kUnbounded);
        assert(!textFieldView(state, rules, kUnbounded).insertCursor);
        state = key(std::move(state), rules, TextFieldKey::Left);
        assert(textFieldView(state, rules, kUnbounded).insertCursor);
        state = textFieldWithValue("abcd", rules, kUnbounded);
        assert(textFieldView(state, rules, kUnbounded).insertCursor); // full
    }

    // ---- Byte offsets, for the callers that must speak bytes ----
    {
        const std::string value = "a" "\xE4\xB8\xAD" "b";
        assert(mc::ui::textFieldByteOffset(value, 0U) == 0U);
        assert(mc::ui::textFieldByteOffset(value, 1U) == 1U);
        assert(mc::ui::textFieldByteOffset(value, 2U) == 4U);
        assert(mc::ui::textFieldByteOffset(value, 3U) == 5U);
        assert(mc::ui::textFieldByteOffset(value, 9U) == value.size()); // clamps
        assert(textFieldCharCount(value) == 3U);
    }

    // ---- Placing the cursor by index (Tab completion rewrites the line) ----
    {
        TextFieldState state = textFieldWithValue("/gamemode surv", kDefault, kUnbounded);
        state = mc::ui::textFieldMoveCursorTo(std::move(state), kDefault, kUnbounded, 5U, false);
        assert(state.cursor == 5U && state.highlight == 5U);
        state = mc::ui::textFieldMoveCursorTo(std::move(state), kDefault, kUnbounded, 9U,
                                              /*extendSelection=*/true);
        assert(textFieldSelectedText(state) == "mode");
        // Out of range clamps rather than reading past the end.
        state = mc::ui::textFieldMoveCursorTo(std::move(state), kDefault, kUnbounded, 999U, false);
        assert(state.cursor == textFieldCharCount(state.value));
        // ...and it is a no-op on an uneditable field, like every other action.
        const TextFieldRules locked{32U, nullptr, false};
        const TextFieldState before = state;
        state = mc::ui::textFieldMoveCursorTo(std::move(state), locked, kUnbounded, 0U, false);
        assert(sameState(state, before));
    }

    // ---- Bare vs bordered geometry (EditBox.java:486-487) ----
    // A bordered field insets its text by 4 and centres it in its own box; a
    // BARE one does neither — its frame is the screen's art, which already
    // contains the padding, and the widget rect it is handed IS the line box.
    // Both used to be treated alike, which put the anvil's rename text low and
    // to the right of where vanilla draws it.
    {
        assert(mc::ui::kTextFieldBorderedInset == 4.0F);
        assert(mc::ui::kTextFieldBareInset == 0.0F);
        assert(mc::ui::textFieldTextInset(1.0F, /*bordered=*/false) == 0.0F);
        assert(mc::ui::textFieldTextInset(2.0F, /*bordered=*/true) == 8.0F);
        // getInnerWidth(): `bordered ? width - 8 : width`.
        assert(mc::ui::textFieldInnerWidth(103.0F, 1.0F, /*bordered=*/false) == 103.0F);
        assert(mc::ui::textFieldInnerWidth(103.0F, 1.0F, /*bordered=*/true) == 95.0F);
    }

    // ---- The field registry says what each place in the game accepts ----
    // The single-source rule of UI-1 lives in this table; a new typeable screen
    // adds a line to it rather than inventing its own limits.
    {
        assert(mc::ui::kWorldNameFieldRules.maxLength == 32U);
        assert(mc::ui::kWorldNameFieldRules.editable);
        assert(mc::ui::kChatFieldRules.maxLength == 256U);
        // I-3 landed the custom-name storage, so the anvil's box is live —
        // but only with something in the left slot, which is why there are two
        // rule sets and the screen picks between them (vanilla's
        // AnvilScreen#slotChanged -> setEditable(!itemStack.isEmpty())).
        assert(mc::ui::kAnvilNameFieldRules.editable);
        assert(!mc::ui::kAnvilNameFieldDisabled.editable);
        assert(mc::ui::kAnvilNameFieldRules.maxLength == 50U); // AnvilScreen.java:43
        assert(mc::ui::kAnvilNameFieldDisabled.maxLength ==
               mc::ui::kAnvilNameFieldRules.maxLength);
        // Open-to-LAN's port box: five digits, nothing else.
        assert(mc::ui::kLanPortFieldRules.maxLength == 5U);
        TextFieldState port =
            typed(TextFieldState{}, mc::ui::kLanPortFieldRules, kUnbounded, "2a5b5c6d5e9f");
        assert(port.value == "25565");
    }

    // ---- The inner width rule is shared by the editing and drawing sides ----
    {
        // A bordered box insets 4 GUI px on each side; a bare one insets
        // nothing (EditBox#getInnerWidth is `bordered ? width - 8 : width`).
        assert(mc::ui::textFieldInnerWidth(200.0F, 1.0F, true) == 192.0F);
        assert(mc::ui::textFieldInnerWidth(200.0F, 2.0F, true) == 184.0F);
        assert(mc::ui::textFieldInnerWidth(200.0F, 1.0F, false) == 200.0F);
        assert(mc::ui::textFieldTextInset(3.0F, true) == 12.0F);
    }

    // ---- Seeding: truncates to maxLength, does not filter (vanilla setValue) ----
    {
        const TextFieldRules rules{3U, mc::ui::textFieldDigitsOnly, true};
        const TextFieldState state = textFieldWithValue("abcdef", rules, kUnbounded);
        assert(state.value == "abc"); // a stored value is shown, not mangled
        assert(state.cursor == 3U && state.highlight == 3U);
    }

    return 0;
}

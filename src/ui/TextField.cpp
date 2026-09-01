#include "ui/TextField.hpp"

// The only reason this layer knows TextFont exists: decodeUtf8/appendUtf8 are
// the project's one UTF-8 codec, and hand-copying an encoder into a second file
// is how the two implementations this node deletes came to disagree in the first
// place. Neither function touches a glyph, a texture or a metric — no TextFont
// object is ever constructed here, so the layer stays headless.
#include "ui/TextFont.hpp"

#include <algorithm>
#include <vector>

namespace mc::ui {
namespace {

// Editing runs on decoded codepoints and re-encodes once at the end. Byte
// juggling on UTF-8 is where an off-by-one splits a character in half; a field
// holds at most a few hundred characters, so the decode costs nothing.
using Chars = std::vector<char32_t>;

[[nodiscard]] std::string encode(const Chars& chars, std::size_t from, std::size_t to) {
    std::string out;
    for (std::size_t index = from; index < to && index < chars.size(); ++index) {
        appendUtf8(out, chars[index]);
    }
    return out;
}

[[nodiscard]] bool accepted(const TextFieldRules& rules, char32_t codepoint) {
    return textFieldTypeable(codepoint) &&
           (rules.accepts == nullptr || rules.accepts(codepoint));
}

// Per-character widths, accumulated. The fonts here carry advances and no
// kerning, so a substring's width is the sum of its characters' — the same
// assumption ui::wrapText already makes, and what turns every fitting question
// below into one linear pass.
[[nodiscard]] std::vector<float> characterWidths(const TextFieldMetrics& metrics,
                                                 const Chars& chars) {
    std::vector<float> widths(chars.size(), 0.0F);
    std::string one;
    for (std::size_t index = 0; index < chars.size(); ++index) {
        one.clear();
        appendUtf8(one, chars[index]);
        widths[index] = metrics.measure(one);
    }
    return widths;
}

[[nodiscard]] bool bounded(const TextFieldMetrics& metrics) {
    return static_cast<bool>(metrics.measure) && metrics.innerWidth > 0.0F;
}

// Last character index (exclusive) still visible when drawing starts at `from`.
[[nodiscard]] std::size_t forwardEnd(const std::vector<float>& widths, std::size_t from,
                                     float innerWidth) {
    float used = 0.0F;
    std::size_t index = std::min(from, widths.size());
    while (index < widths.size() && used + widths[index] <= innerWidth) {
        used += widths[index];
        ++index;
    }
    return index;
}

// First character index that still fits when the window is anchored at its right
// edge `to` — vanilla's plainSubstrByWidth measuring backwards.
[[nodiscard]] std::size_t backwardStart(const std::vector<float>& widths, std::size_t to,
                                        float innerWidth) {
    float used = 0.0F;
    std::size_t index = std::min(to, widths.size());
    while (index > 0 && used + widths[index - 1] <= innerWidth) {
        used += widths[index - 1];
        --index;
    }
    return index;
}

// The horizontal scroll rule, stated as an invariant rather than transcribed:
// `position` must be visible, and the window must never hang past the end of the
// text leaving a dead gap on the right. vanilla's scrollTo nudges displayPos by
// a character count computed against a proportional font, which is why its
// window drifts; three clamps say the same thing without the drift.
[[nodiscard]] std::size_t scrolledTo(std::size_t displayStart, std::size_t position,
                                     const std::vector<float>& widths, float innerWidth) {
    const std::size_t length = widths.size();
    displayStart = std::min(displayStart, length);
    position = std::min(position, length);
    if (position < displayStart) {
        displayStart = position;
    }
    if (position > forwardEnd(widths, displayStart, innerWidth)) {
        displayStart = backwardStart(widths, position, innerWidth);
    }
    // No dead window: once the text shrinks (or the cursor walks back), pull the
    // start back far enough that the field is filled to its right edge again.
    const std::size_t tail = backwardStart(widths, length, innerWidth);
    return std::min(displayStart, tail);
}

// The decoded working form of a state, with every index already clamped to the
// value's length so no operation below has to re-check.
struct Working final {
    Chars chars;
    std::size_t cursor = 0;
    std::size_t highlight = 0;
    std::size_t displayStart = 0;
};

[[nodiscard]] Working open(const TextFieldState& state) {
    Working working{decodeUtf8(state.value), 0, 0, 0};
    const std::size_t length = working.chars.size();
    working.cursor = std::min(state.cursor, length);
    working.highlight = std::min(state.highlight, length);
    working.displayStart = std::min(state.displayStart, length);
    return working;
}

[[nodiscard]] TextFieldState close(const Working& working, const TextFieldMetrics& metrics,
                                   std::size_t scrollTarget) {
    TextFieldState state;
    state.value = encode(working.chars, 0, working.chars.size());
    state.cursor = working.cursor;
    state.highlight = working.highlight;
    state.displayStart =
        bounded(metrics)
            ? scrolledTo(working.displayStart, scrollTarget, characterWidths(metrics, working.chars),
                         metrics.innerWidth)
            : 0U;
    return state;
}

struct Selection final {
    std::size_t start = 0;
    std::size_t end = 0;
};

[[nodiscard]] Selection selectionOf(const Working& working) {
    return {std::min(working.cursor, working.highlight), std::max(working.cursor, working.highlight)};
}

// vanilla EditBox.getWordPosition, transcribed, with |dir| == 1 and stripSpaces
// on — the two forms every caller here uses. Forward lands past the run of
// spaces that follows the word; backward first eats the spaces behind the
// cursor, then the word. That asymmetry is vanilla's and is what makes
// Ctrl+Backspace on "a   " delete the whole tail in one press.
[[nodiscard]] std::size_t wordPosition(const Chars& chars, int direction, std::size_t from) {
    const std::size_t length = chars.size();
    std::size_t result = std::min(from, length);
    if (direction > 0) {
        while (result < length && chars[result] != U' ') {
            ++result;
        }
        while (result < length && chars[result] == U' ') {
            ++result;
        }
    } else {
        while (result > 0 && chars[result - 1] == U' ') {
            --result;
        }
        while (result > 0 && chars[result - 1] != U' ') {
            --result;
        }
    }
    return result;
}

[[nodiscard]] TextFieldState moveCursorTo(Working working, const TextFieldMetrics& metrics,
                                          std::size_t target, bool extendSelection) {
    working.cursor = std::min(target, working.chars.size());
    if (!extendSelection) {
        working.highlight = working.cursor;
    }
    return close(working, metrics, working.cursor);
}

} // namespace

bool textFieldTypeable(char32_t codepoint) {
    return codepoint >= 32U && codepoint != 127U && codepoint != 167U;
}

bool textFieldDigitsOnly(char32_t codepoint) {
    return codepoint >= U'0' && codepoint <= U'9';
}

std::size_t textFieldCharCount(std::string_view text) {
    std::size_t count = 0;
    for (const char byte : text) {
        if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U) {
            ++count;
        }
    }
    return count;
}

std::size_t textFieldByteOffset(std::string_view text, std::size_t charIndex) {
    std::size_t seen = 0;
    for (std::size_t offset = 0; offset < text.size(); ++offset) {
        if ((static_cast<unsigned char>(text[offset]) & 0xC0U) == 0x80U) {
            continue;
        }
        if (seen == charIndex) {
            return offset;
        }
        ++seen;
    }
    return text.size();
}

TextFieldState textFieldMoveCursorTo(TextFieldState state, const TextFieldRules& rules,
                                     const TextFieldMetrics& metrics, std::size_t charIndex,
                                     bool extendSelection) {
    if (!rules.editable) {
        return state;
    }
    return moveCursorTo(open(state), metrics, charIndex, extendSelection);
}

std::string textFieldSelectedText(const TextFieldState& state) {
    const Working working = open(state);
    const auto [start, end] = selectionOf(working);
    return encode(working.chars, start, end);
}

TextFieldState textFieldWithValue(std::string_view value, const TextFieldRules& rules,
                                  const TextFieldMetrics& metrics) {
    Working working;
    working.chars = decodeUtf8(value);
    if (working.chars.size() > rules.maxLength) {
        working.chars.resize(rules.maxLength);
    }
    working.cursor = working.chars.size();
    working.highlight = working.cursor;
    return close(working, metrics, working.cursor);
}

TextFieldState textFieldApplyText(TextFieldState state, const TextFieldRules& rules,
                                  const TextFieldMetrics& metrics, std::string_view text) {
    if (!rules.editable) {
        return state;
    }
    Working working = open(state);
    const auto [start, end] = selectionOf(working);

    // Filter first, truncate second. The other order lets a run of rejected
    // characters eat the field's budget, so a field that refused every keystroke
    // would still report itself full.
    Chars inserted;
    for (const char32_t codepoint : decodeUtf8(text)) {
        if (accepted(rules, codepoint)) {
            inserted.push_back(codepoint);
        }
    }
    const std::size_t kept = working.chars.size() - (end - start);
    const std::size_t room = kept >= rules.maxLength ? 0U : rules.maxLength - kept;
    if (inserted.size() > room) {
        inserted.resize(room);
    }
    if (room == 0U && start == end) {
        return state; // full field, nothing selected: vanilla does not touch the cursor either
    }

    Chars next;
    next.reserve(kept + inserted.size());
    next.insert(next.end(), working.chars.begin(),
                working.chars.begin() + static_cast<std::ptrdiff_t>(start));
    next.insert(next.end(), inserted.begin(), inserted.end());
    next.insert(next.end(), working.chars.begin() + static_cast<std::ptrdiff_t>(end),
                working.chars.end());
    working.chars = std::move(next);
    working.cursor = start + inserted.size();
    working.highlight = working.cursor;
    return close(working, metrics, working.cursor);
}

TextFieldState textFieldApplyChar(TextFieldState state, const TextFieldRules& rules,
                                  const TextFieldMetrics& metrics, char32_t codepoint) {
    if (!rules.editable || !accepted(rules, codepoint)) {
        // A rejected character is not an edit at all: it must not collapse a
        // selection, move the cursor, or scroll the window.
        return state;
    }
    std::string one;
    appendUtf8(one, codepoint);
    return textFieldApplyText(std::move(state), rules, metrics, one);
}

TextFieldState textFieldApplyKey(TextFieldState state, const TextFieldRules& rules,
                                 const TextFieldMetrics& metrics, TextFieldKey key,
                                 TextFieldModifiers modifiers) {
    if (!rules.editable) {
        return state;
    }
    Working working = open(state);
    const std::size_t length = working.chars.size();

    switch (key) {
    case TextFieldKey::Backspace:
    case TextFieldKey::Delete: {
        if (length == 0U) {
            return state;
        }
        // A selection always goes first — deleting "one character" while text is
        // selected is what makes a field eat the wrong thing.
        if (working.cursor != working.highlight) {
            return textFieldApplyText(std::move(state), rules, metrics, "");
        }
        const int direction = key == TextFieldKey::Backspace ? -1 : 1;
        std::size_t target = 0;
        if (modifiers.control) {
            target = wordPosition(working.chars, direction, working.cursor);
        } else if (direction < 0) {
            target = working.cursor > 0U ? working.cursor - 1U : 0U;
        } else {
            target = std::min(working.cursor + 1U, length);
        }
        const std::size_t start = std::min(target, working.cursor);
        const std::size_t end = std::max(target, working.cursor);
        if (start == end) {
            return state;
        }
        working.chars.erase(working.chars.begin() + static_cast<std::ptrdiff_t>(start),
                            working.chars.begin() + static_cast<std::ptrdiff_t>(end));
        working.cursor = start;
        working.highlight = start;
        return close(working, metrics, start);
    }
    case TextFieldKey::Left: {
        const std::size_t target = modifiers.control
                                       ? wordPosition(working.chars, -1, working.cursor)
                                       : (working.cursor > 0U ? working.cursor - 1U : 0U);
        return moveCursorTo(std::move(working), metrics, target, modifiers.shift);
    }
    case TextFieldKey::Right: {
        const std::size_t target = modifiers.control
                                       ? wordPosition(working.chars, 1, working.cursor)
                                       : std::min(working.cursor + 1U, length);
        return moveCursorTo(std::move(working), metrics, target, modifiers.shift);
    }
    case TextFieldKey::Home:
        return moveCursorTo(std::move(working), metrics, 0U, modifiers.shift);
    case TextFieldKey::End:
        return moveCursorTo(std::move(working), metrics, length, modifiers.shift);
    case TextFieldKey::SelectAll:
        working.cursor = length;
        working.highlight = 0U;
        // DIVERGENCE 2: vanilla scrolls to the HIGHLIGHT here (setHighlightPos
        // calls scrollTo), so Ctrl+A on a long value jumps the window to the
        // start and hides the cursor. The cursor stays visible here instead —
        // the invariant the rest of this layer is asserted against.
        return close(working, metrics, length);
    }
    return state;
}

TextFieldView textFieldView(const TextFieldState& state, const TextFieldRules& rules,
                            const TextFieldMetrics& metrics) {
    const Working working = open(state);
    const std::size_t length = working.chars.size();
    const std::size_t start = working.displayStart;
    const std::size_t end =
        bounded(metrics)
            ? forwardEnd(characterWidths(metrics, working.chars), start, metrics.innerWidth)
            : length;

    TextFieldView view;
    view.visible = encode(working.chars, start, end);
    view.visibleChars = end - start;
    view.cursorOnScreen = working.cursor >= start && working.cursor <= end;
    view.cursorOffset = view.cursorOnScreen ? working.cursor - start : 0U;
    const auto selection = selectionOf(working);
    view.selectionStart = std::clamp(selection.start, start, end) - start;
    view.selectionEnd = std::clamp(selection.end, start, end) - start;
    view.insertCursor = working.cursor < length || length >= rules.maxLength;
    return view;
}

} // namespace mc::ui

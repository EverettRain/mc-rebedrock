#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mc::ui {

// Breaks one logical line (assumed free of '\n' — callers split hard breaks
// first) into visual lines that each measure no wider than maxWidth. Word-aware,
// mirroring vanilla TextHandler.wrapLines: the break falls on the last space
// before the overflow, the trailing break space is dropped, and a single word
// wider than maxWidth is split at UTF-8 codepoint boundaries (never mid-byte).
//
// `measure` returns the width of a UTF-8 substring in the same units as
// maxWidth (e.g. unscaled GUI pixels). Width accumulates per codepoint, which is
// exact for the advance-only font (no kerning), so wrapping stays O(n) rather
// than re-measuring growing prefixes. An empty input yields no lines.
[[nodiscard]] std::vector<std::string>
wrapText(std::string_view text, float maxWidth,
         const std::function<float(std::string_view)>& measure);

} // namespace mc::ui

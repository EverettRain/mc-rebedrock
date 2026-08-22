#pragma once

// Pure front-end menu geometry: the rectangles and visible-row counts for the
// title/world-list/language/options pages. Extracted from the renderer so the
// draw pass and the input hit-testing share one Vulkan-free source of truth
// instead of each recomputing the layout. Everything here is a function of the
// framebuffer size, the GUI scale, the current page and a couple of state flags
// passed in by the caller.

#include "ui/HudLayout.hpp"
#include "ui/PageStack.hpp"

#include <cstddef>

namespace mc::ui {

// The number of bottom/menu buttons a front-end page shows. The options page
// gains a Difficulty entry only while a world is open.
[[nodiscard]] std::size_t menuButtonCount(PageId page, bool worldOpen);

// One save-list row in the band between the title and the bottom buttons.
[[nodiscard]] UiRect worldListRow(std::size_t index, const HudLayout& layout,
                                  float framebufferWidth);

// How many save rows fit in the list band at the current canvas size.
[[nodiscard]] std::size_t saveListVisibleRowCount(float framebufferWidth, float framebufferHeight,
                                                  int guiScale);

// The grey warning line's Y, the full-width language box, and one language row.
[[nodiscard]] float languageWarningY(const HudLayout& layout);
[[nodiscard]] UiRect languageListBox(const HudLayout& layout, float framebufferWidth);
[[nodiscard]] UiRect languageRow(std::size_t index, const HudLayout& layout,
                                 float framebufferWidth);
[[nodiscard]] std::size_t languageVisibleRowCount(float framebufferWidth, float framebufferHeight,
                                                  int guiScale);
// Scrollbar geometry and cursor-to-row mapping shared by drawing and input.
// The hit track is wider than the four-pixel thumb, matching vanilla's usable
// list-widget gutter while keeping the visual narrow.
[[nodiscard]] UiRect languageScrollbarTrack(const HudLayout& layout, float framebufferWidth);
[[nodiscard]] UiRect languageScrollbarThumb(const HudLayout& layout, float framebufferWidth,
                                             std::size_t itemCount,
                                             std::size_t visibleRows,
                                             std::size_t firstIndex);
[[nodiscard]] std::size_t languageScrollIndexFromCursor(
    const HudLayout& layout, float framebufferWidth, std::size_t itemCount,
    std::size_t visibleRows, float cursorY);

// PX-6 Bug1: the Controls key-bind list is a SCROLLING list (like the world /
// language lists), not a fixed button grid — 24 rebindable actions would blow
// past the 20-button menu cap and throw. These mirror the language-list geometry:
// a content-sized box between the title and the bottom button band, one row per
// visible action, plus a scrollbar.
[[nodiscard]] UiRect controlsListBox(const HudLayout& layout, float framebufferWidth);
[[nodiscard]] UiRect controlsRow(std::size_t visibleIndex, const HudLayout& layout,
                                 float framebufferWidth);
[[nodiscard]] std::size_t controlsVisibleRowCount(float framebufferWidth, float framebufferHeight,
                                                  int guiScale);
[[nodiscard]] UiRect controlsScrollbarTrack(const HudLayout& layout, float framebufferWidth);
[[nodiscard]] std::size_t controlsScrollIndexFromCursor(const HudLayout& layout,
                                                        float framebufferWidth,
                                                        std::size_t itemCount,
                                                        std::size_t visibleRows, float cursorY);

// Shared button geometry across the front-end pages: bottom-anchored for the
// save/edit/delete/language pages, two-column for video settings, centred menu
// otherwise.
[[nodiscard]] UiRect frontendButtonRect(const HudLayout& layout, PageId page, std::size_t index,
                                        std::size_t buttonCount);

} // namespace mc::ui

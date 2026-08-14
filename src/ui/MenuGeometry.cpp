#include "ui/MenuGeometry.hpp"

#include <algorithm>
#include <cmath>

namespace mc::ui {

std::size_t menuButtonCount(PageId page, bool worldOpen) {
    switch (page) {
    case PageId::Title:
        return 3U;
    case PageId::WorldList:
        return 4U;
    case PageId::CreateWorld:
        return 3U;
    case PageId::EditWorld:
        return 3U;
    case PageId::ConfirmDelete:
        return 2U;
    case PageId::Options:
        // One fewer button without a world open (no Difficulty entry).
        return worldOpen ? 7U : 6U;
    case PageId::Experimental:
        return 5U;
    case PageId::VideoSettings:
        return 11U;
    case PageId::Controls:
        return 3U;
    case PageId::Language:
        return 2U;
    case PageId::Pause:
        return 3U;
    case PageId::Death:
        return 2U;
    default:
        return 0U;
    }
}

UiRect worldListRow(std::size_t index, const HudLayout& layout, float framebufferWidth) {
    const float scale = layout.scale();
    const float width = std::min(300.0F * scale, framebufferWidth - 20.0F * scale);
    return {
        (framebufferWidth - width) * 0.5F,
        (34.0F + static_cast<float>(index) * 22.0F) * scale,
        width,
        20.0F * scale,
    };
}

std::size_t saveListVisibleRowCount(float framebufferWidth, float framebufferHeight, int guiScale) {
    const HudLayout layout{framebufferWidth, framebufferHeight, guiScale};
    const float scale = layout.scale();
    constexpr float kListTop = 34.0F; // first row's top edge, in scale units
    constexpr float kRowStep = 22.0F; // vertical distance between row tops
    // The world list's four function buttons sit in two columns of two, so the
    // block occupies exactly two rows on the bottom band.
    constexpr float kButtonRows = 2.0F;
    constexpr float kButtonHeight = 20.0F;
    constexpr float kButtonStep = 24.0F;
    constexpr float kBottomMargin = 16.0F; // canvas bottom to last button's bottom
    constexpr float kListToButtonGap = 12.0F;
    const float logicalHeight = framebufferHeight / scale;
    const float buttonBlockTop =
        logicalHeight - kBottomMargin - kButtonHeight - (kButtonRows - 1.0F) * kButtonStep;
    const float available = buttonBlockTop - kListToButtonGap - kListTop;
    const float rows = std::max(available / kRowStep, 1.0F);
    return static_cast<std::size_t>(rows);
}

float languageWarningY(const HudLayout& layout) {
    const float scale = layout.scale();
    const auto firstButton = layout.bottomMenuButton(0U, 2U, 2U);
    return firstButton.y - 16.0F * scale;
}

UiRect languageListBox(const HudLayout& layout, float framebufferWidth) {
    const float scale = layout.scale();
    constexpr float kRowStep = 22.0F;
    const float topBound = 44.0F * scale;
    const float warningY = languageWarningY(layout);
    const float bottomBound = warningY - 8.0F * scale;
    const float width = framebufferWidth;
    // Content-sized height: as many rows as fit in the band.
    const std::size_t rows = std::max<std::size_t>(
        static_cast<std::size_t>((bottomBound - topBound) / (kRowStep * scale)), 1U);
    const float height = static_cast<float>(rows) * kRowStep * scale;
    const float top = topBound + (bottomBound - topBound - height) * 0.5F;
    return {0.0F, top, width, height};
}

UiRect languageRow(std::size_t index, const HudLayout& layout, float framebufferWidth) {
    const float scale = layout.scale();
    const auto box = languageListBox(layout, framebufferWidth);
    constexpr float kRowStep = 22.0F;
    // LanguageSelectionList keeps its background full-width, but vanilla's
    // entry selection rectangle is a centred 270 logical pixels. Treating the
    // whole background strip as the entry made hover/selection run from edge
    // to edge and also turned empty side gutters into click targets.
    constexpr float kVanillaRowWidth = 270.0F;
    const float rowWidth = std::min(kVanillaRowWidth * scale,
                                    std::max(box.width - 32.0F * scale, 1.0F));
    return {
        box.x + (box.width - rowWidth) * 0.5F,
        box.y + static_cast<float>(index) * kRowStep * scale,
        rowWidth,
        20.0F * scale,
    };
}

std::size_t languageVisibleRowCount(float framebufferWidth, float framebufferHeight, int guiScale) {
    const HudLayout layout{framebufferWidth, framebufferHeight, guiScale};
    const float scale = layout.scale();
    constexpr float kRowStep = 22.0F;
    const float rows = std::max(languageListBox(layout, framebufferWidth).height / (kRowStep * scale),
                                1.0F);
    return static_cast<std::size_t>(rows);
}

UiRect languageScrollbarTrack(const HudLayout& layout, float framebufferWidth) {
    const float scale = layout.scale();
    const auto box = languageListBox(layout, framebufferWidth);
    // Vanilla places the scrollbar just outside the centred language entries,
    // not against the full-width background edge. The visual thumb is centred
    // 144 logical pixels to the right of screen centre.
    const float desiredCenter = box.x + box.width * 0.5F + 144.0F * scale;
    const float center = std::clamp(desiredCenter, box.x + 5.0F * scale,
                                    box.x + box.width - 5.0F * scale);
    return {center - 5.0F * scale, box.y + 2.0F * scale,
            10.0F * scale, std::max(box.height - 4.0F * scale, 1.0F)};
}

UiRect languageScrollbarThumb(const HudLayout& layout, float framebufferWidth,
                              std::size_t itemCount, std::size_t visibleRows,
                              std::size_t firstIndex) {
    const float scale = layout.scale();
    const auto track = languageScrollbarTrack(layout, framebufferWidth);
    if (itemCount <= visibleRows || itemCount == 0U) {
        return {track.x + 3.0F * scale, track.y, 4.0F * scale, track.height};
    }
    const std::size_t maximumFirst = itemCount - visibleRows;
    const float thumbHeight = std::max(
        track.height * static_cast<float>(visibleRows) / static_cast<float>(itemCount),
        8.0F * scale);
    const float travel = std::max(track.height - thumbHeight, 1.0F);
    const float normalized = static_cast<float>(std::min(firstIndex, maximumFirst)) /
                             static_cast<float>(maximumFirst);
    return {track.x + 3.0F * scale, track.y + normalized * travel,
            4.0F * scale, thumbHeight};
}

std::size_t languageScrollIndexFromCursor(const HudLayout& layout, float framebufferWidth,
                                          std::size_t itemCount, std::size_t visibleRows,
                                          float cursorY) {
    if (itemCount <= visibleRows) {
        return 0U;
    }
    const std::size_t maximumFirst = itemCount - visibleRows;
    const auto track = languageScrollbarTrack(layout, framebufferWidth);
    const auto thumb = languageScrollbarThumb(layout, framebufferWidth, itemCount, visibleRows, 0U);
    const float travel = std::max(track.height - thumb.height, 1.0F);
    const float normalized =
        std::clamp((cursorY - track.y - thumb.height * 0.5F) / travel, 0.0F, 1.0F);
    return static_cast<std::size_t>(
        std::lround(normalized * static_cast<float>(maximumFirst)));
}

UiRect frontendButtonRect(const HudLayout& layout, PageId page, std::size_t index,
                          std::size_t buttonCount) {
    if (page == PageId::WorldList) {
        return layout.bottomMenuButton(index, buttonCount, 2U);
    }
    // The video page grew past one column's worth of buttons: its settings stack
    // in two centred columns with "Done" on its own row beneath.
    if (page == PageId::VideoSettings) {
        return layout.videoSettingsButton(index, buttonCount);
    }
    if (page == PageId::EditWorld || page == PageId::ConfirmDelete) {
        return layout.bottomMenuButton(index, buttonCount);
    }
    // 1.16.1's LanguageOptionsScreen places "Force Unicode Font" and "Done" side
    // by side at the bottom, not stacked.
    if (page == PageId::Language) {
        return layout.bottomMenuButton(index, buttonCount, 2U);
    }
    return layout.menuButton(index, buttonCount);
}

} // namespace mc::ui

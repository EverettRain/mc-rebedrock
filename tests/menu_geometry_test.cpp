#include "ui/MenuGeometry.hpp"

#include <cassert>

int main() {
    constexpr float width = 1280.0F;
    constexpr float height = 720.0F;
    const mc::ui::HudLayout layout{width, height, 2};
    const std::size_t visible = mc::ui::languageVisibleRowCount(width, height, 2);
    constexpr std::size_t itemCount = 40U;
    assert(visible > 0U && visible < itemCount);

    const auto box = mc::ui::languageListBox(layout, width);
    const auto row = mc::ui::languageRow(0U, layout, width);
    assert(row.width == 270.0F * layout.scale());
    assert(row.x > box.x);
    assert(row.x + row.width < box.x + box.width);
    assert(row.x + row.width * 0.5F == box.x + box.width * 0.5F);

    const auto track = mc::ui::languageScrollbarTrack(layout, width);
    assert(track.x > row.x + row.width);
    assert(track.x + track.width < box.x + box.width);
    const auto firstThumb = mc::ui::languageScrollbarThumb(
        layout, width, itemCount, visible, 0U);
    const auto lastThumb = mc::ui::languageScrollbarThumb(
        layout, width, itemCount, visible, itemCount - visible);
    assert(track.contains(firstThumb.x + firstThumb.width * 0.5F,
                          firstThumb.y + firstThumb.height * 0.5F));
    assert(lastThumb.y > firstThumb.y);

    assert(mc::ui::languageScrollIndexFromCursor(
               layout, width, itemCount, visible, track.y) == 0U);
    assert(mc::ui::languageScrollIndexFromCursor(
               layout, width, itemCount, visible, track.y + track.height) ==
           itemCount - visible);
    const auto middle = mc::ui::languageScrollIndexFromCursor(
        layout, width, itemCount, visible, track.y + track.height * 0.5F);
    assert(middle > 0U && middle < itemCount - visible);
    return 0;
}

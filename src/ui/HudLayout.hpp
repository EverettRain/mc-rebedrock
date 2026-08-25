#pragma once

#include <cstddef>

namespace mc::ui {

struct UiRect final {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;

    [[nodiscard]] bool contains(float pointX, float pointY) const;
};

struct UiPoint final {
    float x = 0.0F;
    float y = 0.0F;
};

struct PlayerPreviewLayout final {
    UiPoint feetAnchor;
    UiPoint lookOrigin;
    UiRect clip;
    float entityScale = 20.0F;
};

[[nodiscard]] UiPoint windowToFramebuffer(
    double cursorX,
    double cursorY,
    int windowWidth,
    int windowHeight,
    int framebufferWidth,
    int framebufferHeight);

[[nodiscard]] UiRect framebufferToClip(
    const UiRect& rectangle,
    float framebufferWidth,
    float framebufferHeight);

[[nodiscard]] UiRect tiledBackgroundSource(
    float framebufferWidth,
    float framebufferHeight,
    float guiScale);

class HudLayout final {
  public:
    static constexpr std::size_t kHotbarSlots = 9;
    static constexpr std::size_t kInventorySlots = 36;
    static constexpr std::size_t kCreativeVisibleSlots = 45;
    // The largest menu a frontend page may carry. The video settings page runs
    // ten entries; two-column layouts keep a larger set on screen.
    static constexpr std::size_t kMaximumMenuButtons = 20U;

    // A requested scale of zero is Minecraft's "Auto" setting. The effective
    // scale is always an integer and keeps at least a 320x240 logical canvas.
    HudLayout(float width, float height, int requestedScale = 0);

    [[nodiscard]] static int calculateGuiScale(
        int framebufferWidth,
        int framebufferHeight,
        int requestedScale = 0);

    [[nodiscard]] UiRect hotbarSlot(std::size_t index) const;
    [[nodiscard]] UiRect hotbarBackground() const;
    [[nodiscard]] UiRect hotbarSelection(std::size_t index) const;
    // The 1.16.1 experience bar: a 182x5 bar centred on the hotbar, seven
    // logical pixels above its top edge (Gui#renderExperienceBar draws it at
    // scaledHeight - 29 with the hotbar at scaledHeight - 22), so it sits in
    // the gap between the hotbar and the hearts row.
    [[nodiscard]] UiRect experienceBar() const;
    [[nodiscard]] UiRect inventoryPanel() const;
    [[nodiscard]] UiRect inventorySlot(std::size_t index) const;
    // EQ-1: InventoryScreen's four armor slots and the offhand slot, GUI spec
    // §10's `(8,8)(8,26)(8,44)(8,62)` (index 0=Head..3=Feet, the screen's own
    // top-to-bottom draw order) and `(77,62)`. The same numeric offsets in the
    // creative Inventory tab, but relative to the wider creativePanel() — 1.16.1
    // CreativeInventoryScreen adds these slots at the same (8,8+row*18)/(77,62)
    // against its own 195x136 background, so passing creative=true anchors them
    // to that panel instead of the 176x166 survival one (the two panels are
    // centred differently, which is the source of the creative-mode misplacement).
    [[nodiscard]] UiRect armorSlot(std::size_t index, bool creative = false) const;
    [[nodiscard]] UiRect offhandSlot(bool creative = false) const;
    [[nodiscard]] UiRect playerCraftingSlot(std::size_t index) const;
    [[nodiscard]] UiRect playerCraftingOutput() const;
    [[nodiscard]] PlayerPreviewLayout playerPreview(bool creative) const;
    [[nodiscard]] UiRect tableCraftingSlot(std::size_t index) const;
    [[nodiscard]] UiRect tableCraftingOutput() const;
    [[nodiscard]] UiRect furnaceInputSlot() const;
    [[nodiscard]] UiRect furnaceFuelSlot() const;
    [[nodiscard]] UiRect furnaceOutputSlot() const;
    [[nodiscard]] UiRect chestSlot(std::size_t index) const;
    [[nodiscard]] UiRect chestInventorySlot(std::size_t index) const;
    [[nodiscard]] UiRect creativePanel() const;
    [[nodiscard]] UiRect creativeSlot(std::size_t index) const;
    [[nodiscard]] UiRect creativeHotbarSlot(std::size_t index) const;
    [[nodiscard]] UiRect creativeInventorySlot(std::size_t index) const;
    [[nodiscard]] UiRect creativeDeleteSlot() const;
    [[nodiscard]] UiRect creativeTab(std::size_t index) const;
    [[nodiscard]] UiRect creativeScrollbarTrack() const;
    [[nodiscard]] UiRect creativeScrollbarThumb(float scrollPosition) const;
    [[nodiscard]] UiRect chatInput() const;
    [[nodiscard]] UiRect menuButton(
        std::size_t index,
        std::size_t buttonCount = 3U) const;
    // Bottom-anchored variant for the save screen's function-button band: the
    // block keeps a fixed gap from the canvas bottom, so it never drifts or
    // collides with the world list as resolution or GUI scale change. Buttons
    // are packed column-first; with columnCount > 1 the columns sit side by
    // side with a basic gap and the whole block is centred as one unit (the
    // world list uses two columns of two), like 1.16.1's adjacent button rows.
    [[nodiscard]] UiRect bottomMenuButton(
        std::size_t index,
        std::size_t buttonCount = 3U,
        std::size_t columnCount = 1U) const;
    // The video-settings page's two-column grid: every button but the last
    // stacks in two centred columns (column-first, like the save screen), and
    // the final "Done" button sits centred on its own row beneath the grid.
    // The whole block is vertically centred like a menu, not bottom-anchored.
    [[nodiscard]] UiRect videoSettingsButton(
        std::size_t index,
        std::size_t buttonCount) const;
    [[nodiscard]] float scale() const { return scale_; }

  private:
    float width_;
    float height_;
    float scale_;
};

} // namespace mc::ui

#include "ui/HudLayout.hpp"

#include <algorithm>
#include <stdexcept>

namespace mc::ui {

bool UiRect::contains(float pointX, float pointY) const {
    return pointX >= x && pointX < x + width && pointY >= y && pointY < y + height;
}

UiPoint windowToFramebuffer(double cursorX, double cursorY, int windowWidth, int windowHeight,
                            int framebufferWidth, int framebufferHeight) {
    if (windowWidth <= 0 || windowHeight <= 0 || framebufferWidth <= 0 || framebufferHeight <= 0) {
        return {};
    }
    return {
        static_cast<float>(cursorX) * static_cast<float>(framebufferWidth) /
            static_cast<float>(windowWidth),
        static_cast<float>(cursorY) * static_cast<float>(framebufferHeight) /
            static_cast<float>(windowHeight),
    };
}

UiRect framebufferToClip(const UiRect& rectangle, float framebufferWidth, float framebufferHeight) {
    if (framebufferWidth <= 0.0F || framebufferHeight <= 0.0F) {
        return {};
    }
    // Vulkan's positive-height viewport maps NDC -1 to the framebuffer top and
    // NDC +1 to the bottom. HUD layout coordinates also grow down from the top.
    return {
        rectangle.x / framebufferWidth * 2.0F - 1.0F,
        rectangle.y / framebufferHeight * 2.0F - 1.0F,
        rectangle.width / framebufferWidth * 2.0F,
        rectangle.height / framebufferHeight * 2.0F,
    };
}

UiRect tiledBackgroundSource(float framebufferWidth, float framebufferHeight, float guiScale) {
    // The generated 26.1 menu atlas contains 16 px tiles. Sampling 1/guiScale
    // maps one tile onto 16 * guiScale framebuffer pixels while
    // keeping the pattern square and independent of the window aspect ratio.
    const float sourceScale = 1.0F / std::max(guiScale, 1.0F);
    return {
        0.0F,
        0.0F,
        std::max(framebufferWidth, 0.0F) * sourceScale,
        std::max(framebufferHeight, 0.0F) * sourceScale,
    };
}

int HudLayout::calculateGuiScale(int framebufferWidth, int framebufferHeight, int requestedScale) {
    const int safeWidth = std::max(framebufferWidth, 1);
    const int safeHeight = std::max(framebufferHeight, 1);
    const int requested = std::max(requestedScale, 0);
    int scale = 1;
    while ((requested == 0 || scale < requested) && safeWidth / (scale + 1) >= 320 &&
           safeHeight / (scale + 1) >= 240) {
        ++scale;
    }
    return scale;
}

HudLayout::HudLayout(float width, float height, int requestedScale)
    : width_(width), height_(height),
      scale_(static_cast<float>(
          calculateGuiScale(static_cast<int>(width), static_cast<int>(height), requestedScale))) {}

UiRect HudLayout::hotbarSlot(std::size_t index) const {
    if (index >= kHotbarSlots) {
        throw std::out_of_range("HUD hotbar slot index is outside 0..8");
    }
    const auto background = hotbarBackground();
    return {
        background.x + (3.0F + static_cast<float>(index) * 20.0F) * scale_,
        background.y + 3.0F * scale_,
        16.0F * scale_,
        16.0F * scale_,
    };
}

UiRect HudLayout::hotbarBackground() const {
    const float backgroundWidth = 182.0F * scale_;
    const float backgroundHeight = 22.0F * scale_;
    return {
        (width_ - backgroundWidth) * 0.5F,
        height_ - backgroundHeight - 4.0F * scale_,
        backgroundWidth,
        backgroundHeight,
    };
}

UiRect HudLayout::hotbarSelection(std::size_t index) const {
    if (index >= kHotbarSlots) {
        throw std::out_of_range("HUD hotbar selection index is outside 0..8");
    }
    const auto background = hotbarBackground();
    return {
        background.x + (-1.0F + static_cast<float>(index) * 20.0F) * scale_,
        background.y - scale_,
        24.0F * scale_,
        24.0F * scale_,
    };
}

UiRect HudLayout::experienceBar() const {
    const auto hotbar = hotbarBackground();
    return {
        hotbar.x,
        hotbar.y - 7.0F * scale_,
        182.0F * scale_,
        5.0F * scale_,
    };
}

UiRect HudLayout::inventoryPanel() const {
    const float panelWidth = 176.0F * scale_;
    const float panelHeight = 166.0F * scale_;
    return {(width_ - panelWidth) * 0.5F, (height_ - panelHeight) * 0.5F, panelWidth, panelHeight};
}

UiRect HudLayout::inventorySlot(std::size_t index) const {
    if (index >= kInventorySlots) {
        throw std::out_of_range("HUD inventory slot index is outside 0..35");
    }
    const auto panel = inventoryPanel();
    const std::size_t row = index < 9 ? 0U : (index - 9U) / 9U;
    const std::size_t column = index < 9 ? index : (index - 9U) % 9U;
    const float top = index < 9 ? 142.0F : 84.0F + static_cast<float>(row) * 18.0F;
    return {
        panel.x + (8.0F + static_cast<float>(column) * 18.0F) * scale_,
        panel.y + top * scale_,
        16.0F * scale_,
        16.0F * scale_,
    };
}

UiRect HudLayout::armorSlot(std::size_t index) const {
    if (index >= 4U)
        throw std::out_of_range("armor slot index is outside 0..3");
    const auto panel = inventoryPanel();
    // GUI spec §10: (8,8) (8,26) (8,44) (8,62), top-to-bottom Head/Chest/Legs/
    // Feet — the screen's own draw order, an 18px row pitch like every other
    // slot grid.
    return {panel.x + 8.0F * scale_, panel.y + (8.0F + static_cast<float>(index) * 18.0F) * scale_,
            16.0F * scale_, 16.0F * scale_};
}

UiRect HudLayout::offhandSlot() const {
    const auto panel = inventoryPanel();
    return {panel.x + 77.0F * scale_, panel.y + 62.0F * scale_, 16.0F * scale_, 16.0F * scale_};
}

UiRect HudLayout::chestSlot(std::size_t index) const {
    if (index >= 27U)
        throw std::out_of_range("chest slot index is outside 0..26");
    const auto panel = inventoryPanel();
    return {panel.x + (8.0F + static_cast<float>(index % 9U) * 18.0F) * scale_,
            panel.y + (18.0F + static_cast<float>(index / 9U) * 18.0F) * scale_, 16.0F * scale_,
            16.0F * scale_};
}

UiRect HudLayout::chestInventorySlot(std::size_t index) const {
    if (index >= kInventorySlots) {
        throw std::out_of_range("chest inventory slot index is outside 0..35");
    }
    const auto panel = inventoryPanel();
    const std::size_t row = index < 9U ? 0U : (index - 9U) / 9U;
    const std::size_t column = index < 9U ? index : (index - 9U) % 9U;
    const float top = index < 9U ? 143.0F : 85.0F + static_cast<float>(row) * 18.0F;
    return {panel.x + (8.0F + static_cast<float>(column) * 18.0F) * scale_, panel.y + top * scale_,
            16.0F * scale_, 16.0F * scale_};
}

UiRect HudLayout::playerCraftingSlot(std::size_t index) const {
    if (index >= 4U)
        throw std::out_of_range("player crafting slot index");
    const auto panel = inventoryPanel();
    return {panel.x + (98.0F + static_cast<float>(index % 2U) * 18.0F) * scale_,
            panel.y + (18.0F + static_cast<float>(index / 2U) * 18.0F) * scale_, 16.0F * scale_,
            16.0F * scale_};
}

UiRect HudLayout::playerCraftingOutput() const {
    const auto panel = inventoryPanel();
    return {panel.x + 154.0F * scale_, panel.y + 28.0F * scale_, 16.0F * scale_, 16.0F * scale_};
}

PlayerPreviewLayout HudLayout::playerPreview(bool creative) const {
    const auto panel = creative ? creativePanel() : inventoryPanel();
    if (creative) {
        // CreativeInventoryScreen 1.16.1 draws the player at x+88, y+45,
        // size 20.  The black preview well in tab_inventory.png occupies the
        // surrounding 34x39 logical-pixel rectangle.
        return {
            {panel.x + 88.0F * scale_, panel.y + 45.0F * scale_},
            {panel.x + 88.0F * scale_, panel.y + 15.0F * scale_},
            {panel.x + 72.0F * scale_, panel.y + 7.0F * scale_, 34.0F * scale_, 39.0F * scale_},
            20.0F,
        };
    }
    // InventoryScreen 1.16.1 uses x+51, y+75, size 30.
    return {
        {panel.x + 51.0F * scale_, panel.y + 75.0F * scale_},
        {panel.x + 51.0F * scale_, panel.y + 25.0F * scale_},
        {panel.x + 26.0F * scale_, panel.y + 8.0F * scale_, 49.0F * scale_, 70.0F * scale_},
        30.0F,
    };
}

UiRect HudLayout::tableCraftingSlot(std::size_t index) const {
    if (index >= 9U)
        throw std::out_of_range("table crafting slot index");
    const auto panel = inventoryPanel();
    return {panel.x + (30.0F + static_cast<float>(index % 3U) * 18.0F) * scale_,
            panel.y + (17.0F + static_cast<float>(index / 3U) * 18.0F) * scale_, 16.0F * scale_,
            16.0F * scale_};
}

UiRect HudLayout::tableCraftingOutput() const {
    const auto panel = inventoryPanel();
    return {panel.x + 124.0F * scale_, panel.y + 35.0F * scale_, 16.0F * scale_, 16.0F * scale_};
}

UiRect HudLayout::furnaceInputSlot() const {
    const auto panel = inventoryPanel();
    return {panel.x + 56.0F * scale_, panel.y + 17.0F * scale_, 16.0F * scale_, 16.0F * scale_};
}
UiRect HudLayout::furnaceFuelSlot() const {
    const auto panel = inventoryPanel();
    return {panel.x + 56.0F * scale_, panel.y + 53.0F * scale_, 16.0F * scale_, 16.0F * scale_};
}
UiRect HudLayout::furnaceOutputSlot() const {
    const auto panel = inventoryPanel();
    return {panel.x + 116.0F * scale_, panel.y + 35.0F * scale_, 16.0F * scale_, 16.0F * scale_};
}

UiRect HudLayout::creativePanel() const {
    const float panelWidth = 195.0F * scale_;
    const float panelHeight = 136.0F * scale_;
    return {(width_ - panelWidth) * 0.5F, (height_ - panelHeight) * 0.5F, panelWidth, panelHeight};
}

UiRect HudLayout::creativeSlot(std::size_t index) const {
    if (index >= kCreativeVisibleSlots) {
        throw std::out_of_range("creative slot index is outside 0..44");
    }
    const auto panel = creativePanel();
    const std::size_t row = index / 9U;
    const std::size_t column = index % 9U;
    return {
        panel.x + (9.0F + static_cast<float>(column) * 18.0F) * scale_,
        panel.y + (18.0F + static_cast<float>(row) * 18.0F) * scale_,
        16.0F * scale_,
        16.0F * scale_,
    };
}

UiRect HudLayout::creativeHotbarSlot(std::size_t index) const {
    if (index >= kHotbarSlots) {
        throw std::out_of_range("creative hotbar slot index is outside 0..8");
    }
    const auto panel = creativePanel();
    return {
        panel.x + (9.0F + static_cast<float>(index) * 18.0F) * scale_,
        panel.y + 112.0F * scale_,
        16.0F * scale_,
        16.0F * scale_,
    };
}

UiRect HudLayout::creativeInventorySlot(std::size_t index) const {
    if (index >= kInventorySlots) {
        throw std::out_of_range("creative inventory slot index is outside 0..35");
    }
    if (index < kHotbarSlots) {
        return creativeHotbarSlot(index);
    }
    const auto panel = creativePanel();
    const std::size_t inventoryIndex = index - kHotbarSlots;
    const std::size_t row = inventoryIndex / 9U;
    const std::size_t column = inventoryIndex % 9U;
    return {
        panel.x + (9.0F + static_cast<float>(column) * 18.0F) * scale_,
        panel.y + (54.0F + static_cast<float>(row) * 18.0F) * scale_,
        16.0F * scale_,
        16.0F * scale_,
    };
}

UiRect HudLayout::creativeDeleteSlot() const {
    const auto panel = creativePanel();
    return {
        panel.x + 173.0F * scale_,
        panel.y + 112.0F * scale_,
        16.0F * scale_,
        16.0F * scale_,
    };
}

UiRect HudLayout::creativeTab(std::size_t index) const {
    if (index >= 8U) {
        throw std::out_of_range("creative tab index is outside 0..7");
    }
    const auto panel = creativePanel();
    // Indices 6 (Spawn Eggs) and 7 (Inventory) share the bottom row: Spawn Eggs
    // sits at the bottom-left, Inventory at the bottom-right.
    if (index == 6U) {
        return {
            panel.x,
            panel.y + panel.height - 4.0F * scale_,
            28.0F * scale_,
            32.0F * scale_,
        };
    }
    if (index == 7U) {
        return {
            panel.x + panel.width - 28.0F * scale_,
            panel.y + panel.height - 4.0F * scale_,
            28.0F * scale_,
            32.0F * scale_,
        };
    }
    return {
        panel.x + static_cast<float>(index) * 29.0F * scale_,
        panel.y - 28.0F * scale_,
        28.0F * scale_,
        32.0F * scale_,
    };
}

UiRect HudLayout::creativeScrollbarTrack() const {
    const auto panel = creativePanel();
    return {
        panel.x + 175.0F * scale_,
        panel.y + 18.0F * scale_,
        14.0F * scale_,
        112.0F * scale_,
    };
}

UiRect HudLayout::creativeScrollbarThumb(float scrollPosition) const {
    const auto track = creativeScrollbarTrack();
    const float clamped = std::clamp(scrollPosition, 0.0F, 1.0F);
    return {
        track.x,
        track.y + 97.0F * scale_ * clamped,
        12.0F * scale_,
        15.0F * scale_,
    };
}

UiRect HudLayout::chatInput() const {
    return {
        2.0F * scale_,
        height_ - 14.0F * scale_,
        width_ - 4.0F * scale_,
        12.0F * scale_,
    };
}

UiRect HudLayout::menuButton(std::size_t index, std::size_t buttonCount) const {
    if (buttonCount == 0U || buttonCount > kMaximumMenuButtons || index >= buttonCount) {
        throw std::out_of_range("menu button index or count is invalid");
    }
    constexpr float buttonWidth = 200.0F;
    constexpr float buttonHeight = 20.0F;
    constexpr float buttonStep = 24.0F;
    const float scaledWidth = buttonWidth * scale_;
    return {
        (width_ - scaledWidth) * 0.5F,
        height_ * 0.5F - static_cast<float>(buttonCount) * 12.0F * scale_ +
            static_cast<float>(index) * buttonStep * scale_,
        scaledWidth,
        buttonHeight * scale_,
    };
}

UiRect HudLayout::bottomMenuButton(std::size_t index, std::size_t buttonCount,
                                   std::size_t columnCount) const {
    if (buttonCount == 0U || buttonCount > kMaximumMenuButtons || index >= buttonCount ||
        columnCount == 0U || columnCount > buttonCount) {
        throw std::out_of_range("menu button index or count is invalid");
    }
    constexpr float buttonWidth = 200.0F;
    constexpr float buttonHeight = 20.0F;
    constexpr float buttonStep = 24.0F;
    constexpr float buttonGap = 4.0F;     // gap between adjacent buttons, like the vertical step
    constexpr float bottomMargin = 16.0F; // canvas bottom to last button's bottom
    constexpr float screenMargin = 16.0F; // min gap from the button block to the screen edge
    const std::size_t rows = (buttonCount + columnCount - 1U) / columnCount;
    const std::size_t column = index / rows;
    const std::size_t row = index % rows;
    // Columns sit next to each other with a basic gap and the whole block is
    // centred as one unit, matching 1.16.1's adjacent button rows rather than
    // spreading the columns across their own screen halves. The width clamps
    // so a narrow canvas never pushes the block past the edges.
    const float maxScaledWidth = (width_ - 2.0F * screenMargin * scale_ -
                                  static_cast<float>(columnCount - 1U) * buttonGap * scale_) /
                                 static_cast<float>(columnCount);
    const float scaledWidth = std::min(buttonWidth * scale_, maxScaledWidth);
    const float blockWidth = static_cast<float>(columnCount) * scaledWidth +
                             static_cast<float>(columnCount - 1U) * buttonGap * scale_;
    const float blockX = (width_ - blockWidth) * 0.5F;
    const float blockBottom = height_ - bottomMargin * scale_;
    const float blockTop =
        blockBottom - buttonHeight * scale_ - static_cast<float>(rows - 1U) * buttonStep * scale_;
    return {
        blockX + static_cast<float>(column) * (scaledWidth + buttonGap * scale_),
        blockTop + static_cast<float>(row) * buttonStep * scale_,
        scaledWidth,
        buttonHeight * scale_,
    };
}

UiRect HudLayout::videoSettingsButton(std::size_t index, std::size_t buttonCount) const {
    if (buttonCount == 0U || buttonCount > kMaximumMenuButtons || index >= buttonCount) {
        throw std::out_of_range("menu button index or count is invalid");
    }
    constexpr float buttonWidth = 200.0F;
    constexpr float buttonHeight = 20.0F;
    constexpr float buttonStep = 24.0F;
    constexpr float buttonGap = 4.0F;
    constexpr float screenMargin = 16.0F; // min gap from the button block to the screen edge
    // The last button is "Done", centred on its own row below the grid; the
    // rest pack column-first into two columns like the save screen's buttons.
    const std::size_t settingCount = buttonCount - 1U;
    const std::size_t rows = (settingCount + 1U) / 2U;
    const std::size_t totalRows = rows + 1U;
    // Vertically centre the whole block the way menuButton does (the first row
    // sits the same half-block above centre the single column would place).
    const float blockTop = height_ * 0.5F - static_cast<float>(totalRows) * 12.0F * scale_;
    const float maxScaledWidth =
        (width_ - 2.0F * screenMargin * scale_ - buttonGap * scale_) * 0.5F;
    const float scaledWidth = std::min(buttonWidth * scale_, maxScaledWidth);
    const float blockWidth = 2.0F * scaledWidth + buttonGap * scale_;
    const float blockX = (width_ - blockWidth) * 0.5F;
    if (index == buttonCount - 1U) {
        // Done spans its own full-width row, centred.
        return {
            (width_ - scaledWidth) * 0.5F,
            blockTop + static_cast<float>(rows) * buttonStep * scale_,
            scaledWidth,
            buttonHeight * scale_,
        };
    }
    const std::size_t column = index / rows;
    const std::size_t row = index % rows;
    return {
        blockX + static_cast<float>(column) * (scaledWidth + buttonGap * scale_),
        blockTop + static_cast<float>(row) * buttonStep * scale_,
        scaledWidth,
        buttonHeight * scale_,
    };
}

} // namespace mc::ui

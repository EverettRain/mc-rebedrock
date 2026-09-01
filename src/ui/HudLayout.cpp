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
    // Vulkan 的正高度视口把 NDC 的 -1 映到帧缓冲顶部、+1 映到底部
    // HUD 的布局坐标同样从顶部往下增长
    return {
        rectangle.x / framebufferWidth * 2.0F - 1.0F,
        rectangle.y / framebufferHeight * 2.0F - 1.0F,
        rectangle.width / framebufferWidth * 2.0F,
        rectangle.height / framebufferHeight * 2.0F,
    };
}

UiRect tiledBackgroundSource(float framebufferWidth, float framebufferHeight, float guiScale) {
    // 生成的 26.1 菜单图集里是 16 像素的瓦片
    // 按 1/guiScale 采样会把一个瓦片映到 16 * guiScale 个帧缓冲像素上
    // 同时保持花纹是正方的，且与窗口宽高比无关
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

UiRect HudLayout::armorSlot(std::size_t index, bool creative) const {
    if (index >= 4U)
        throw std::out_of_range("armor slot index is outside 0..3");
    if (creative) {
        // 26.1 的 CreativeModeInventoryScreen 背包页签并不像生存界面那样把盔甲堆成一列
        // 它把这些槽重排成一个 2x2 的块，分列在玩家模型两侧
        // 对菜单槽 5 到 8，也就是头、胸、腿、脚
        // 坐标是 x = 54 + (pos/2)*54 与 y = 6 + (pos%2)*27，其中 pos = i-5
        // 坐标是 x = 54 + (pos/2)*54 与 y = 6 + (pos%2)*27，其中 pos = i-5
        // 这里的界面绘制顺序，0 是头、3 是脚，正是同一个 pos
        // 于是头在 (54,6)、胸在 (54,33)、腿在 (108,6)、脚在 (108,33)
        const auto panel = creativePanel();
        const float x = 54.0F + static_cast<float>(index / 2U) * 54.0F;
        const float y = 6.0F + static_cast<float>(index % 2U) * 27.0F;
        return {panel.x + x * scale_, panel.y + y * scale_, 16.0F * scale_, 16.0F * scale_};
    }
    const auto panel = inventoryPanel();
    // GUI 规格 §10 给的是 (8,8) (8,26) (8,44) (8,62)，自上而下依次是头、胸、腿、脚
    // 这就是生存 InventoryScreen 自身的绘制顺序，行距 18 像素
    return {panel.x + 8.0F * scale_, panel.y + (8.0F + static_cast<float>(index) * 18.0F) * scale_,
            16.0F * scale_, 16.0F * scale_};
}

UiRect HudLayout::offhandSlot(bool creative) const {
    if (creative) {
        // 26.1 的 CreativeModeInventoryScreen 把副手槽，即菜单槽 45，放在它自己面板的 (35,20)
        // 而不是生存界面的 (77,62)
        const auto panel = creativePanel();
        return {panel.x + 35.0F * scale_, panel.y + 20.0F * scale_, 16.0F * scale_, 16.0F * scale_};
    }
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
        // vanilla 的 CreativeInventoryScreen 把玩家画在 x+88, y+45，尺寸 20
        // tab_inventory.png 里那口黑色的预览井占据周围 34x39 个逻辑像素的矩形
        return {
            {panel.x + 88.0F * scale_, panel.y + 45.0F * scale_},
            {panel.x + 88.0F * scale_, panel.y + 15.0F * scale_},
            {panel.x + 72.0F * scale_, panel.y + 7.0F * scale_, 34.0F * scale_, 39.0F * scale_},
            20.0F,
        };
    }
    // vanilla 的 InventoryScreen 用的是 x+51, y+75，尺寸 30
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

UiRect HudLayout::enchantingItemSlot() const {
    const auto panel = inventoryPanel();
    return {panel.x + 15.0F * scale_, panel.y + 47.0F * scale_, 16.0F * scale_, 16.0F * scale_};
}
UiRect HudLayout::enchantingLapisSlot() const {
    const auto panel = inventoryPanel();
    return {panel.x + 35.0F * scale_, panel.y + 47.0F * scale_, 16.0F * scale_, 16.0F * scale_};
}
UiRect HudLayout::enchantingOption(std::size_t index) const {
    if (index >= 3U) {
        throw std::out_of_range("enchanting option index is outside 0..2");
    }
    const auto panel = inventoryPanel();
    return {panel.x + 60.0F * scale_,
            panel.y + (14.0F + static_cast<float>(index) * 19.0F) * scale_, 108.0F * scale_,
            19.0F * scale_};
}

UiRect HudLayout::anvilLeftSlot() const {
    const auto panel = inventoryPanel();
    return {panel.x + 27.0F * scale_, panel.y + 47.0F * scale_, 16.0F * scale_, 16.0F * scale_};
}
UiRect HudLayout::anvilRightSlot() const {
    const auto panel = inventoryPanel();
    return {panel.x + 76.0F * scale_, panel.y + 47.0F * scale_, 16.0F * scale_, 16.0F * scale_};
}
UiRect HudLayout::anvilOutputSlot() const {
    const auto panel = inventoryPanel();
    return {panel.x + 134.0F * scale_, panel.y + 47.0F * scale_, 16.0F * scale_, 16.0F * scale_};
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
    if (index >= 11U) {
        throw std::out_of_range("creative tab index is outside 0..10");
    }
    const auto panel = creativePanel();
    // 26.1 的页签条：前七个页签在面板上方的顶行，其余四个在下方的底行
    // 每个宽 28 像素，按各自的列号排布
    constexpr std::size_t kTopRowTabs = 7U;
    const bool bottom = index >= kTopRowTabs;
    // 背包页签是通往生存背包的入口，也是这条页签条所依据的 CreativeTab 枚举里的最后一个下标 10
    // vanilla 把它锚在底行最右侧的列上，右对齐，正好落在顶行最后一个页签下方
    // 它不跟左边的食物、原材料、刷怪蛋挤在一起
    // 其余每个底行页签都保持自己天然的从左到右的列位
    constexpr std::size_t kInventoryTabIndex = 10U;
    const std::size_t column = !bottom ? index
        : (index == kInventoryTabIndex ? kTopRowTabs - 1U : index - kTopRowTabs);
    const float y =
        bottom ? panel.y + panel.height - 4.0F * scale_ : panel.y - 28.0F * scale_;
    return {
        panel.x + static_cast<float>(column) * 28.0F * scale_,
        y,
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
    // 各列并排、留一个基本间距，整块作为一个单位居中
    // 这与 vanilla 相邻的按钮行一致，而不是把两列各自摊到半边屏幕上
    // 宽度会被夹紧，窄画布因此绝不会把整块挤出边界
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
    // 最后一个按钮是"完成"，单独居中占网格下方一行
    // 其余的按列优先塞进两列，与存档界面的按钮一样
    const std::size_t settingCount = buttonCount - 1U;
    const std::size_t rows = (settingCount + 1U) / 2U;
    const std::size_t totalRows = rows + 1U;
    // 整块按 menuButton 的方式垂直居中
    // 首行落在中线上方半块处，与单列布局会摆的位置相同
    const float blockTop = height_ * 0.5F - static_cast<float>(totalRows) * 12.0F * scale_;
    const float maxScaledWidth =
        (width_ - 2.0F * screenMargin * scale_ - buttonGap * scale_) * 0.5F;
    const float scaledWidth = std::min(buttonWidth * scale_, maxScaledWidth);
    const float blockWidth = 2.0F * scaledWidth + buttonGap * scale_;
    const float blockX = (width_ - blockWidth) * 0.5F;
    if (index == buttonCount - 1U) {
        // 完成按钮独占一整行，居中
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

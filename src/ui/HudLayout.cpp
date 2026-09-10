#include "ui/HudLayout.hpp"

#include <algorithm>
#include <cmath>
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

int HudLayout::calculateGuiScale(int framebufferWidth, int framebufferHeight, int requestedScale,
                                 bool forceUnicode) {
    const int safeWidth = std::max(framebufferWidth, 1);
    const int safeHeight = std::max(framebufferHeight, 1);
    const int requested = std::max(requestedScale, 0);
    int scale = 1;
    // 26.1 `Window.calculateScale` 的递增循环，逐字同形——包括那两条只在极小画布上
    // 才起作用的兜底（`scale < framebufferWidth/Height`），少了它们，一个比缩放档还窄的
    // 帧缓冲会一直放大下去
    // `scale != requested` 而不是 `scale < requested`，与 vanilla 同形：requested 为 0（Auto）
    // 时这个条件恒真，循环因此一路涨到上限——那正是 Auto 档的定义，不需要另一条分支
    while (scale != requested && scale < safeWidth && scale < safeHeight &&
           safeWidth / (scale + 1) >= 320 && safeHeight / (scale + 1) >= 240) {
        ++scale;
    }
    // 打开强制 Unicode 字体后档位被抬到偶数：unicode 字形按半尺寸绘制，奇数档会让
    // 半像素落不到整数纹素上。spec §1.1 把这条记成「26.1 已无此逻辑 [?]」，那是错的
    if (forceUnicode && scale % 2 != 0) {
        ++scale;
    }
    return scale;
}

HudLayout::HudLayout(float width, float height, int requestedScale, bool forceUnicode)
    : width_(width), height_(height),
      scale_(static_cast<float>(calculateGuiScale(static_cast<int>(width),
                                                  static_cast<int>(height), requestedScale,
                                                  forceUnicode))) {}

int HudLayout::logicalWidth() const {
    return static_cast<int>(std::ceil(width_ / scale_));
}

int HudLayout::logicalHeight() const {
    return static_cast<int>(std::ceil(height_ / scale_));
}

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
    constexpr int kWidth = 182;
    constexpr int kHeight = 22;
    // ★ UI-6f（D5）：**贴底**，没有边距。26.1 `Gui.java:554`：
    //     blitSprite(HOTBAR_SPRITE, screenCenter - 91, guiHeight() - 22, 182, 22)
    //   本作从前在底边留 4 逻辑像素——那是自造值。UI-3 整数化时**有意没动**它，
    //   因为顺手改会让前后对照图无法解读；现在它自己是被对齐的那一项。
    //
    //   快捷栏格、选中框、经验条与绘制侧三处全部**相对本矩形**定位，所以这一个常量
    //   一改，整排 HUD 一起下移 4 像素、相对关系不变——这也正是它们当初就该这么写的理由。
    return {
        toFramebuffer(centredLogicalX(kWidth)),
        toFramebuffer(logicalHeight() - kHeight),
        toFramebuffer(kWidth),
        toFramebuffer(kHeight),
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
    // spec §5 范式 L6：`leftPos = (W - imageWidth) / 2`，整数除法
    constexpr int kWidth = 176;
    constexpr int kHeight = 166;
    return {toFramebuffer(centredLogicalX(kWidth)), toFramebuffer(centredLogicalY(kHeight)),
            toFramebuffer(kWidth), toFramebuffer(kHeight)};
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

// AR-M6: MerchantScreen's own 276x166 panel, centred by the same integer rule
// every other screen uses.
UiRect HudLayout::tradingPanel() const {
    constexpr int kWidth = 276;
    constexpr int kHeight = 166;
    return {toFramebuffer(centredLogicalX(kWidth)), toFramebuffer(centredLogicalY(kHeight)),
            toFramebuffer(kWidth), toFramebuffer(kHeight)};
}
// MerchantMenu: `addSlot(..., 136, 37)` and `addSlot(..., 162, 37)`.
UiRect HudLayout::tradingPaymentSlot(std::size_t index) const {
    if (index >= 2U) {
        throw std::out_of_range("trading payment slot index is outside 0..1");
    }
    const auto panel = tradingPanel();
    const float x = index == 0U ? 136.0F : 162.0F;
    return {panel.x + x * scale_, panel.y + 37.0F * scale_, 16.0F * scale_, 16.0F * scale_};
}
// MerchantMenu: `addSlot(new MerchantResultSlot(..., 220, 37))`.
UiRect HudLayout::tradingResultSlot() const {
    const auto panel = tradingPanel();
    return {panel.x + 220.0F * scale_, panel.y + 37.0F * scale_, 16.0F * scale_, 16.0F * scale_};
}
// MerchantScreen: NUMBER_OF_OFFER_BUTTONS = 7, TRADE_BUTTON_X = 5,
// TRADE_BUTTON_WIDTH = 88, TRADE_BUTTON_HEIGHT = 20, first row at y = 18.
UiRect HudLayout::tradingOffer(std::size_t index) const {
    if (index >= kTradingOfferButtons) {
        throw std::out_of_range("trading offer index is outside 0..6");
    }
    const auto panel = tradingPanel();
    return {panel.x + 5.0F * scale_,
            panel.y + (18.0F + static_cast<float>(index) * 20.0F) * scale_, 88.0F * scale_,
            20.0F * scale_};
}

// MerchantScreen's player inventory: the same 9x4 grid every screen draws, but
// anchored at x = 107 inside the 276-wide panel (its `inventoryLabelX`).
UiRect HudLayout::tradingInventorySlot(std::size_t index) const {
    if (index >= kInventorySlots) {
        throw std::out_of_range("trading inventory slot index is outside 0..35");
    }
    const auto panel = tradingPanel();
    const std::size_t row = index < 9 ? 0U : (index - 9U) / 9U;
    const std::size_t column = index < 9 ? index : (index - 9U) % 9U;
    const float top = index < 9 ? 142.0F : 84.0F + static_cast<float>(row) * 18.0F;
    return {
        panel.x + (107.0F + static_cast<float>(column) * 18.0F) * scale_,
        panel.y + top * scale_,
        16.0F * scale_,
        16.0F * scale_,
    };
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
    constexpr int kWidth = 195;
    constexpr int kHeight = 136;
    return {toFramebuffer(centredLogicalX(kWidth)), toFramebuffer(centredLogicalY(kHeight)),
            toFramebuffer(kWidth), toFramebuffer(kHeight)};
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

UiRect HudLayout::worldNameField() const {
    constexpr int kWidth = 200;
    return {
        toFramebuffer(centredLogicalX(kWidth)),
        toFramebuffer(logicalHeight() / 2 - 58),
        toFramebuffer(kWidth),
        toFramebuffer(20),
    };
}

UiRect HudLayout::chatInput() const {
    return {
        toFramebuffer(2),
        toFramebuffer(logicalHeight() - 14),
        toFramebuffer(logicalWidth() - 4),
        toFramebuffer(12),
    };
}

UiRect HudLayout::menuButton(std::size_t index, std::size_t buttonCount) const {
    if (buttonCount == 0U || buttonCount > kMaximumMenuButtons || index >= buttonCount) {
        throw std::out_of_range("menu button index or count is invalid");
    }
    constexpr int buttonWidth = 200;
    constexpr int buttonHeight = 20;
    constexpr int buttonStep = 24;
    return {
        toFramebuffer(centredLogicalX(buttonWidth)),
        toFramebuffer(logicalHeight() / 2 - static_cast<int>(buttonCount) * 12 +
                      static_cast<int>(index) * buttonStep),
        toFramebuffer(buttonWidth),
        toFramebuffer(buttonHeight),
    };
}

UiRect HudLayout::bottomMenuButton(std::size_t index, std::size_t buttonCount,
                                   std::size_t columnCount) const {
    if (buttonCount == 0U || buttonCount > kMaximumMenuButtons || index >= buttonCount ||
        columnCount == 0U || columnCount > buttonCount) {
        throw std::out_of_range("menu button index or count is invalid");
    }
    constexpr int buttonWidth = 200;
    constexpr int buttonHeight = 20;
    constexpr int buttonStep = 24;
    constexpr int buttonGap = 4;     // gap between adjacent buttons, like the vertical step
    constexpr int bottomMargin = 16; // canvas bottom to last button's bottom
    constexpr int screenMargin = 16; // min gap from the button block to the screen edge
    const auto rows = static_cast<int>((buttonCount + columnCount - 1U) / columnCount);
    const auto columns = static_cast<int>(columnCount);
    const auto column = static_cast<int>(index) / rows;
    const auto row = static_cast<int>(index) % rows;
    // 各列并排、留一个基本间距，整块作为一个单位居中
    // 这与 vanilla 相邻的按钮行一致，而不是把两列各自摊到半边屏幕上
    // 宽度会被夹紧，窄画布因此绝不会把整块挤出边界
    // UI-3：全程逻辑像素整数——夹紧那一步也要取整，否则整块的宽度带小数，居中又会回到
    // 半像素上，而 spec §1.2 要求居中一律整数除法
    const int maxWidth =
        (logicalWidth() - 2 * screenMargin - (columns - 1) * buttonGap) / columns;
    const int width = std::min(buttonWidth, maxWidth);
    const int blockWidth = columns * width + (columns - 1) * buttonGap;
    const int blockX = (logicalWidth() - blockWidth) / 2;
    const int blockBottom = logicalHeight() - bottomMargin;
    const int blockTop = blockBottom - buttonHeight - (rows - 1) * buttonStep;
    return {
        toFramebuffer(blockX + column * (width + buttonGap)),
        toFramebuffer(blockTop + row * buttonStep),
        toFramebuffer(width),
        toFramebuffer(buttonHeight),
    };
}

UiRect HudLayout::videoSettingsButton(std::size_t index, std::size_t buttonCount) const {
    if (buttonCount == 0U || buttonCount > kMaximumMenuButtons || index >= buttonCount) {
        throw std::out_of_range("menu button index or count is invalid");
    }
    constexpr int buttonWidth = 200;
    constexpr int buttonHeight = 20;
    constexpr int buttonStep = 24;
    constexpr int buttonGap = 4;
    constexpr int screenMargin = 16; // min gap from the button block to the screen edge
    // 最后一个按钮是"完成"，单独居中占网格下方一行
    // 其余的按列优先塞进两列，与存档界面的按钮一样
    const auto settingCount = static_cast<int>(buttonCount) - 1;
    const int rows = (settingCount + 1) / 2;
    const int totalRows = rows + 1;
    // 整块按 menuButton 的方式垂直居中
    // 首行落在中线上方半块处，与单列布局会摆的位置相同
    const int blockTop = logicalHeight() / 2 - totalRows * 12;
    const int maxWidth = (logicalWidth() - 2 * screenMargin - buttonGap) / 2;
    const int width = std::min(buttonWidth, maxWidth);
    const int blockWidth = 2 * width + buttonGap;
    const int blockX = (logicalWidth() - blockWidth) / 2;
    if (index == buttonCount - 1U) {
        // 完成按钮独占一整行，居中
        return {
            toFramebuffer(centredLogicalX(width)),
            toFramebuffer(blockTop + rows * buttonStep),
            toFramebuffer(width),
            toFramebuffer(buttonHeight),
        };
    }
    const int column = static_cast<int>(index) / rows;
    const int row = static_cast<int>(index) % rows;
    return {
        toFramebuffer(blockX + column * (width + buttonGap)),
        toFramebuffer(blockTop + row * buttonStep),
        toFramebuffer(width),
        toFramebuffer(buttonHeight),
    };
}

} // namespace mc::ui

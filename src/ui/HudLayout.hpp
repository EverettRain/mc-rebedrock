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
    // 一个前端页面最多能承载多大的菜单
    // 视频设置页有十项，分两列的布局能在屏幕上放下更多
    static constexpr std::size_t kMaximumMenuButtons = 20U;

    // 请求的缩放为零表示 Minecraft 的"自动"档
    // 实际生效的缩放永远是整数，并且至少保住 320x240 的逻辑画布
    HudLayout(float width, float height, int requestedScale = 0, bool forceUnicode = false);

    // 26.1 `Window.calculateScale`。`forceUnicode` 打开时档位被抬到偶数——
    // spec §1.1 说「26.1 已无此逻辑 [?]」，那是错的，源码里就在 Window.java:424-426。
    [[nodiscard]] static int calculateGuiScale(
        int framebufferWidth,
        int framebufferHeight,
        int requestedScale = 0,
        bool forceUnicode = false);

    [[nodiscard]] UiRect hotbarSlot(std::size_t index) const;
    [[nodiscard]] UiRect hotbarBackground() const;
    [[nodiscard]] UiRect hotbarSelection(std::size_t index) const;
    // vanilla 的经验条：182x5，以快捷栏为中心，位于其上边缘往上七个逻辑像素处
    // Gui#renderExperienceBar 把它画在 scaledHeight - 29，而快捷栏在 scaledHeight - 22
    // 它因此正好落在快捷栏与生命值行之间的空隙里
    [[nodiscard]] UiRect experienceBar() const;
    [[nodiscard]] UiRect inventoryPanel() const;
    [[nodiscard]] UiRect inventorySlot(std::size_t index) const;
    // InventoryScreen 的四个盔甲槽与副手槽
    // 取自 GUI 规格 §10 的 (8,8)(8,26)(8,44)(8,62) 与 (77,62)
    // 下标 0 是头、3 是脚，即界面自身的自上而下绘制顺序
    // 创造模式背包页签用同一组数值偏移，但相对的是更宽的 creativePanel()
    // vanilla 的 CreativeInventoryScreen 就是按同样的 (8,8+row*18) 与 (77,62) 摆这些槽
    // 只不过它加在自己那张 195x136 的底图上
    // 因此传 creative=true 会把它们锚到那个面板，而不是生存模式那个 176x166 的
    // 两个面板的居中方式不同，创造模式下槽位错位的根源就在这里
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
    // ENCH-2: the enchanting table's two inputs and its three option bars.
    // Straight from GUI spec §10's table row: panel 176x166, item slot (15,47),
    // lapis slot (35,47), and three 108x19 option bars at (60,14) / (60,33) /
    // (60,52). The bars are the clickable buttons, not just decoration, so they
    // are geometry the click router shares with the drawing code.
    [[nodiscard]] UiRect enchantingItemSlot() const;
    [[nodiscard]] UiRect enchantingLapisSlot() const;
    [[nodiscard]] UiRect enchantingOption(std::size_t index) const;
    // ENCH-3: the anvil's three slots, from GUI spec §10's anvil row — left
    // (27,47), right (76,47), output (134,47) on the same 176x166 panel.
    [[nodiscard]] UiRect anvilLeftSlot() const;
    [[nodiscard]] UiRect anvilRightSlot() const;
    [[nodiscard]] UiRect anvilOutputSlot() const;
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
    // 创建/编辑世界那个名称输入框，GUI spec §2.4 的 200x20 常用尺寸
    // 几何从 HudRenderer 搬到这里，是因为 UI-1 之后有两个消费者：画它的一侧，
    // 和驱动编辑的一侧——后者要用同一个宽度算横向滚动
    [[nodiscard]] UiRect worldNameField() const;
    [[nodiscard]] UiRect menuButton(
        std::size_t index,
        std::size_t buttonCount = 3U) const;
    // 存档界面那条功能按钮带用的贴底变体
    // 整块与画布底边保持固定间距，分辨率或 GUI 缩放变化时它既不会漂移，也不会撞上世界列表
    // 按钮按列优先排布，columnCount 大于 1 时各列并排、留一个基本间距，整块作为一个单位居中
    // 世界列表用的是两列各两个，与 vanilla 相邻的按钮行一致
    [[nodiscard]] UiRect bottomMenuButton(
        std::size_t index,
        std::size_t buttonCount = 3U,
        std::size_t columnCount = 1U) const;
    // 视频设置页的两列网格
    // 除最后一个之外的每个按钮都堆进两个居中的列，按列优先，与存档界面一致
    // 最后那个"完成"按钮单独居中占一行，落在网格下方
    // 整块像菜单那样垂直居中，而不是贴底
    [[nodiscard]] UiRect videoSettingsButton(
        std::size_t index,
        std::size_t buttonCount) const;
    [[nodiscard]] float scale() const { return scale_; }
    // spec §1.1 的逻辑画布：scaledWidth/Height = ceil(帧缓冲 / 生效缩放)
    // 本仓其余版面在帧缓冲像素上用浮点算，那套算不出 `H/4 + 48` 这种以逻辑高度做整数
    // 除法的基线，而 spec §1.2 明确要求居中一律整数除法，否则与原版差 1px
    // 因此需要照 vanilla 摆位的屏幕先取这两个量，解完版面再乘 scale 回到帧缓冲像素
    [[nodiscard]] int logicalWidth() const;
    [[nodiscard]] int logicalHeight() const;

  private:
    // UI-3：逻辑像素 → 帧缓冲像素。GUI spec §1.1 的映射就是一次乘 scale：
    // 26.1 的 GUI 正交投影用的是 `width / guiScale` 的**未取整**值
    // （`GuiRenderer.java:203`），不是 ceil 后的 scaledWidth——spec §1.1 那句
    // 「ortho(0, scaledWidth, scaledHeight, 0, …)」是错的。两者故意不一致：
    // **版面用 ceil 后的逻辑画布做整数运算，绘制用精确的 ×scale**，右/下边缘那不足
    // 一像素的一列因此被切掉，这是原版行为。
    [[nodiscard]] float toFramebuffer(int logical) const {
        return static_cast<float>(logical) * scale_;
    }
    // 在逻辑画布上居中一个宽/高为 `extent` 的东西，**整数除法**（spec §1.2）。
    // 从前这里是 `(帧缓冲宽 - 内容宽) * 0.5F`：既没取整，也没用 ceil 后的画布，
    // 于是 1280x720 @scale 3 下快捷栏落在 367 而不是 vanilla 的 366。
    [[nodiscard]] int centredLogicalX(int extent) const { return (logicalWidth() - extent) / 2; }
    [[nodiscard]] int centredLogicalY(int extent) const { return (logicalHeight() - extent) / 2; }

    float width_;
    float height_;
    float scale_;
};

} // namespace mc::ui

#pragma once

// 需要运行期拉伸的 26.1 控件精灵在兼容 GUI 图集里的位置，以及各自的填充方式
//
// TextureManager 把 26.1 的 `gui/sprites/*` 重新拼回本渲染器采样的 256x256 层，再用固定像素矩形取
// 对心、快捷栏、容器背景这类尺寸固定的元素，这样就足够精确
// 但菜单按钮和滑条轨道的宽度会随窗口与 GUI 缩放变化
// 26.1 为此给每个精灵配了 `gui.scaling` 元数据，只有图集矩形还不够
// HUD 还需要精灵的参考尺寸和边框才能做九宫格
// 这个头文件就是那份共享词汇表
// 放在 TextureManager 之外，HudRenderer 才不必依赖整个纹理上传子系统

#include "assets/GuiSpriteScaling.hpp"
#include "ui/HudLayout.hpp"

#include <array>
#include <cstddef>

namespace mc::render {

// 一个 26.1 精灵在兼容图集中的落位
// `region` 是它在 256x256 层内的像素矩形，`scaling` 是解析出来的 `.png.mcmeta`
// ui::forEachGuiSpriteQuad 把这一对变成绘制调用
struct GuiAtlasSprite final {
    ui::UiRect region{};
    assets::GuiSpriteScaling scaling{};
};

// 前端会在运行期决定尺寸的那些精灵：控件，以及提示框的底衬
// 其余 GUI 精灵沿用写死的图集矩形——它们按原始尺寸绘制，这时九宫格和直接拉伸没有区别
enum class GuiWidgetSprite : std::size_t {
    Button,
    ButtonHighlighted,
    ButtonDisabled,
    Slider,
    SliderHandle,
    SliderHandleHighlighted,
    // 26.1 的 TooltipRenderUtil 画的两张：填充在下、边框在上，同一个矩形叠两遍
    // 它们的尺寸完全由内容决定，因此必须走九宫格而不是整张拉伸
    TooltipBackground,
    TooltipFrame,
    // UI-4：滚动条。26.1 的 AbstractScrollArea 用**精灵**画它，不是手绘颜色——
    // GUI spec §2.7 写的「轨道 0xFF000000、滑块 0xFF808080 + 亮边 0xFFC0C0C0」
    // 是 1.20.2 之前的画法。两张都是 6x32、九宫格 border 1，因此滑块能按内容拉长
    // 而两端那 1px 的亮边始终是 1px。
    Scroller,
    ScrollerBackground,
    // UI-4：主菜单那两个图标钮里的 15x15 图标（CommonButtons.language/accessibility）
    IconLanguage,
    IconAccessibility,
    // UI-8 / D30：光标下那一格的高亮。26.1 `AbstractContainerScreen:183-190` 画的是
    // **两张 24x24 九宫格（border 4）**，一张在槽位内容之前、一张在之后，都画在
    // 槽位的 (x-4, y-4)——也就是比 16x16 的格子大一圈。
    // ★ 本作此前是一层白色 0.34 的方块，正好盖住格子、且在物品**下面**。
    SlotHighlightBack,
    SlotHighlightFront,
    Count,
};

using GuiWidgetSpriteTable =
    std::array<GuiAtlasSprite, static_cast<std::size_t>(GuiWidgetSprite::Count)>;

[[nodiscard]] inline const GuiAtlasSprite& guiWidgetSprite(const GuiWidgetSpriteTable& table,
                                                           GuiWidgetSprite sprite) {
    return table[static_cast<std::size_t>(sprite)];
}

} // namespace mc::render

#pragma once

// 渲染器内核（VulkanRenderer.cpp）与 HUD 绘制子系统（HudRenderer.hpp）共用的 HUD/前端类型与常量
// 放在 mc::render 而不是某个 .cpp 的匿名命名空间里，两边才能指同一份定义

#include "gameplay/ScreenHandler.hpp"
#include "ui/HudLayout.hpp"
#include "world/ItemModel.hpp"

#include <cstddef>
#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace mc::render {

// guiTextures 数组的层号，与 createGuiTexture() 保持同步
// 11 是 misc/vignette.png，12 是烘焙好的 Screen.renderBackground 暗角渐变
inline constexpr float kVignetteGuiLayer = 11.0F;
inline constexpr float kScreenDimGuiLayer = 12.0F;
inline constexpr float kMenuListBackgroundGuiLayer = 13.0F;
// ENCH-2: gui/container/enchanting_table.png, with the level numerals and the
// three option-bar states packed into the space its 176x166 panel leaves. The
// pack positions below are shared by the baker (TextureManager::createGuiTexture)
// and the reader (HudRenderer's enchanting screen) so neither can drift.
inline constexpr float kEnchantingGuiLayer = 14.0F;
// The 3x2 grid of 16x16 level numerals: enabled on the first row, disabled on
// the second, to the right of the panel.
inline constexpr int kEnchantingLevelSpriteX = 176;
inline constexpr int kEnchantingLevelSpriteY = 0;
// The three 108x19 option-bar states, stacked 20px apart below the panel:
// normal, disabled, highlighted.
inline constexpr int kEnchantingBarSpriteY = 168;
// ENCH-3: gui/container/anvil.png, with its text-field and error sprites packed
// into the space its 176x166 panel leaves — same arrangement, same reason.
inline constexpr float kAnvilGuiLayer = 15.0F;
// The 110x16 text field (normal then disabled) below the panel, and the 28x21
// "too expensive" error marker to the right of them.
inline constexpr int kAnvilTextFieldSpriteY = 168;
inline constexpr int kAnvilErrorSpriteX = 176;
inline constexpr int kAnvilErrorSpriteY = 0;
// I-2: gui/sprites/tooltip/background.png 与 tooltip/frame.png，两张 100x100
// 并排放在同一层（背景在左、边框在右）。它们是提示框的全部底衬，画法见
// HudRenderer::drawTooltipBox。
inline constexpr float kTooltipGuiLayer = 16.0F;
// 标题界面的六张全景面，拼成 logo 背后的那个世界；标题轮播把它们当幻灯片循环
// 也是 TextureManager 上传全景数组层时的层数（此前两处各写一份，ENCH-2 并到这里）
inline constexpr std::size_t kPanoramaFaces = 6U;
// 26.1 的十个内容页签加上"背包"伪页签，七个在上排、四个在下排
// 对应 tab_top_1..7 与 tab_bottom_1..4
inline constexpr std::size_t kCreativeTabCount = 11U;

// "当前开着哪个界面"是玩法事实，槽位路由要据此分派
// 所以枚举跟 ScreenHandler 放在一起，渲染器只是引用它
using ContainerScreen = gameplay::ContainerScreen;

// hud.frag dispatches on data.x with `>` comparisons against half-way values, so
// the modes are spaced apart. Named here rather than written as bare floats at
// each call site, which is how a mode number ends up meaning two things.
inline constexpr float kHudModeFlat = 0.0F;         // untextured quad
inline constexpr float kHudModeBlockTexture = 1.0F; // a block atlas sprite
inline constexpr float kHudModeFontGlyph = 2.0F;
inline constexpr float kHudModeGuiSprite = 3.0F;
inline constexpr float kHudModeBlockIcon = 4.25F; // RN-14's 3D item model icon
inline constexpr float kHudModeCrosshair = 5.0F;

// The HUD's push constants, and the ONE meaning each field has.
//
// **A field's meaning does not change with the draw mode.** A field may go unused
// in a mode; it may never be reinterpreted. RN-14 broke that — it put the icon's
// box into `color` and its box maximum into `uvRect`, told hud.vert, and left
// hud.frag reading `color` as a tint. Every block icon in the inventory came out
// a black diamond: the tint was the box's minimum corner, and the alpha was a UV
// component that is zero on two of the three visible faces.
//
// Three consumers declare this block — here, hud.vert and hud.frag — and
// `hud_push_constant_test` holds all three together.
struct HudPush final {
    // Clip-space rectangle: origin xy, size zw. Every mode.
    glm::vec4 rect;
    // Tint, multiplied into the sampled texel. Every mode. The block icon passes
    // opaque white; anything else there is the regression above.
    glm::vec4 color;
    // Sprite source rectangle: origin xy, size zw. The sprite modes; unused by
    // the block icon, which carries per-corner UVs instead.
    glm::vec4 uvRect;
    // x = draw mode (the kHudMode* constants), y = atlas layer. Every mode.
    glm::vec4 data;
    // The block icon draws ONE face of ONE box of the block's item model per
    // call. These four carry that box and that face; every other mode leaves them
    // zero. The box is in 0..1 cell coordinates, already turned into the
    // inventory pose.
    glm::vec4 iconBoxMin; // xyz
    glm::vec4 iconBoxMax; // xyz
    // The face's four corner UVs in the order the quad emits them, resolved on
    // the CPU (mc::world::iconBoxOf). Resolving them there is what let hud.vert
    // drop its per-cube-model UV tables, and those tables were the reason a block
    // item could only ever be a cube.
    glm::vec4 iconUv01; // uv[0].xy, uv[1].xy
    glm::vec4 iconUv23; // uv[2].xy, uv[3].xy
};

// 128 bytes is Vulkan's guaranteed minimum and this block is now exactly that
// size. There is no room left: another field has to shrink something here, or
// move the icon's payload to a uniform buffer.
static_assert(sizeof(HudPush) == 128U, "HUD push constants must fit Vulkan's guaranteed minimum");

// One face of one box of a block's item icon, as push constants.
//
// It is a free function in a Vulkan-free header so that it can be TESTED. The
// call site is HudRenderer::drawHudBlockIcon, inside a translation unit no test
// links; keeping the arithmetic out here is the same move SceneReadback made with
// PreviewImageBytes, and for the same reason — the part that can go wrong quietly
// should not live where nothing can look at it.
[[nodiscard]] inline HudPush makeBlockIconPush(const ui::UiRect& clip,
                                               const world::IconBox& icon, std::size_t face,
                                               float atlasLayer) {
    const auto& uv = icon.uvCorner[face];
    HudPush push{};
    push.rect = {clip.x, clip.y, clip.width, clip.height};
    // White. hud.frag multiplies this into the texel, so the icon's colour is its
    // texture's colour — which is the whole of what an icon is.
    push.color = {1.0F, 1.0F, 1.0F, 1.0F};
    push.data = {kHudModeBlockIcon, atlasLayer, 0.0F, 0.0F};
    push.uvRect = {}; // a sprite rectangle; this mode has none
    push.iconBoxMin = {icon.from.x, icon.from.y, icon.from.z, 0.0F};
    push.iconBoxMax = {icon.to.x, icon.to.y, icon.to.z, 0.0F};
    push.iconUv01 = {uv[0].x, uv[0].y, uv[1].x, uv[1].y};
    push.iconUv23 = {uv[2].x, uv[2].y, uv[3].x, uv[3].y};
    return push;
}

// 标题全景立方体：x = 偏航、y = 俯仰（弧度）、z = tan(fov/2)、w = 宽高比
// blur.x 是只作用于背景的模糊半径，单位为帧缓冲像素（26.1 默认 5），其余分量保留
struct PanoramaPush final {
    glm::vec4 rotationFov;
    glm::vec4 blur;
};

struct ItemPush final {
    glm::vec4 positionSize;
    glm::vec4 textureLayersRotation;
    glm::vec4 data;
    // 非等比长方体的可选 xyz 尺寸；零向量表示沿用 positionSize.w 里的标量尺寸
    glm::vec4 dimensions;
    glm::mat4 viewModelTransform{1.0F};
};

static_assert(sizeof(ItemPush) <= 128U, "Item push constants must fit Vulkan's guaranteed minimum");

} // namespace mc::render

#pragma once

// 渲染器内核（VulkanRenderer.cpp）与 HUD 绘制子系统（HudRenderer.hpp）共用的 HUD/前端类型与常量
// 放在 mc::render 而不是某个 .cpp 的匿名命名空间里，两边才能指同一份定义

#include "gameplay/ScreenHandler.hpp"
#include "ui/HudLayout.hpp"
#include "world/ItemModel.hpp"

#include <array>
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

// --- item_entity.vert's draw modes, and the categories over them
//
// One pipeline serves fifteen kinds of draw, selected by `data.x`. Two things
// live here and they are NOT the same thing:
//
//   * a MODE SELECTOR. A draw is exactly one mode. The set is exclusive and, over
//     the modes that have producers, complete.
//   * a CATEGORY. A property several modes share — "is placed by a matrix", "is a
//     block item's box". A category covers several modes ON PURPOSE.
//
// Conflating them is not hypothetical. `blockItemBox` deliberately covers both 10
// and 11: a held block IS a block-item box and has to reach the branch that
// resolves the UV rect. Rewriting that predicate as exclusive would put the
// regression this file's history is about straight back.
//
// What is removed instead is the THRESHOLD as a way of expressing membership.
// `data.x > 9.5` covers 10 and 11 today and would silently swallow a mode 12
// tomorrow — a new draw kind changing an existing branch's meaning by arithmetic
// accident. Every category below lists its members.
//
// The shader mirrors this block between its own begin/end markers and derives its
// comparison bounds from the constants (it keeps float `> m - 0.5 && < m + 0.5`
// comparisons — that is the right way to compare floats, and `==` is not).
// `hud_push_constant_test` parses both and holds them together.
//
// ---- item draw modes: begin ----
inline constexpr float kItemModeWorldBillboard = 0.0F;      // camera-facing, whole layer
inline constexpr float kItemModeBlockCube = 1.0F;           // falling block, breaking overlay
inline constexpr float kItemModeEntityShadow = 2.0F;        // the round shadow blob
inline constexpr float kItemModeHeldSprite = 3.0F;          // flat sprite in view space
inline constexpr float kItemModeViewSkinCuboid = 4.0F;      // skinned cuboid in view space
inline constexpr float kItemModeArticulatedCuboid = 5.0F;   // skinned cuboid in world space
inline constexpr float kItemModeMatrixViewModel = 6.0F;     // the view matrix carries the pose
inline constexpr float kItemModeGeneratedItem = 7.0F;       // extruded flat-sprite item model
inline constexpr float kItemModeWorldMatrixCuboid = 8.0F;   // chest lid, articulated bones
inline constexpr float kItemModeBoxUvEntity = 9.0F;         // mobs and NPCs
inline constexpr float kItemModeBlockItemDropped = 10.0F;   // RN-14: one face of one box
inline constexpr float kItemModeBlockItemHeld = 11.0F;      // the same, placed by the matrix
inline constexpr float kItemModeAtlasBillboard = -1.0F;     // sub-rect UV + opacity (xp orb)
inline constexpr float kItemModeHeldBillboard = -2.0F;      // billboard in view space
inline constexpr float kItemModeMatrixHeldBillboard = -3.0F; // billboard placed by the matrix
// ---- item draw modes: end ----

inline constexpr std::array kItemModes{
    kItemModeMatrixHeldBillboard, kItemModeHeldBillboard,    kItemModeAtlasBillboard,
    kItemModeWorldBillboard,      kItemModeBlockCube,        kItemModeEntityShadow,
    kItemModeHeldSprite,          kItemModeViewSkinCuboid,   kItemModeArticulatedCuboid,
    kItemModeMatrixViewModel,     kItemModeGeneratedItem,    kItemModeWorldMatrixCuboid,
    kItemModeBoxUvEntity,         kItemModeBlockItemDropped, kItemModeBlockItemHeld,
};

// Modes the shader still recognises that nothing pushes. Recorded rather than
// deleted: removing a branch of a shader that cannot be run in this container is
// a change to make with eyes on a screen. Each is a shape the renderer once had
// or was built toward; the test asserts this list is exactly the difference
// between what the shader knows and what the renderer sends, so a new orphan
// cannot appear quietly.
inline constexpr std::array kItemModesWithoutProducer{
    kItemModeWorldBillboard,   // every billboard today carries a sub-rect (mode -1)
    kItemModeHeldSprite,       // held flat items go through the extruded model (7)
    kItemModeViewSkinCuboid,   // no view-space skinned cuboid is drawn
    kItemModeArticulatedCuboid,// articulated bones use the world matrix (8)
    kItemModeHeldBillboard,    // no view-space billboard is drawn
    kItemModeMatrixHeldBillboard,
};

// The float comparison the shader makes, mirrored. Ranges, not equality: the mode
// arrives as a float and `==` on floats is how a mode silently stops matching.
[[nodiscard]] inline constexpr bool isItemMode(float value, float mode) {
    return value > mode - 0.5F && value < mode + 0.5F;
}

// --- The three top-level dispatch branches. Every mode takes exactly one. ---
[[nodiscard]] inline constexpr bool itemBranchGeneratedItem(float mode) {
    return isItemMode(mode, kItemModeGeneratedItem);
}
[[nodiscard]] inline constexpr bool itemBranchShadow(float mode) {
    return isItemMode(mode, kItemModeEntityShadow);
}
[[nodiscard]] inline constexpr bool itemBranchCuboid(float mode) {
    return isItemMode(mode, kItemModeBlockCube) || isItemMode(mode, kItemModeHeldSprite) ||
           isItemMode(mode, kItemModeViewSkinCuboid) ||
           isItemMode(mode, kItemModeArticulatedCuboid) ||
           isItemMode(mode, kItemModeMatrixViewModel) ||
           isItemMode(mode, kItemModeWorldMatrixCuboid) ||
           isItemMode(mode, kItemModeBoxUvEntity) ||
           isItemMode(mode, kItemModeBlockItemDropped) ||
           isItemMode(mode, kItemModeBlockItemHeld);
}
// The shader reaches this one by falling through, but it is written here as its
// own member list on purpose. Defined as the complement it would make the
// partition assertion vacuous — every mode would take exactly one branch by
// construction, including a mode that had just been dropped from the cuboid list
// and was now being drawn as a billboard.
[[nodiscard]] inline constexpr bool itemBranchBillboard(float mode) {
    return isItemMode(mode, kItemModeWorldBillboard) ||
           isItemMode(mode, kItemModeAtlasBillboard) ||
           isItemMode(mode, kItemModeHeldBillboard) ||
           isItemMode(mode, kItemModeMatrixHeldBillboard);
}

// --- Categories: several modes on purpose, each member named. ---
[[nodiscard]] inline constexpr bool itemBlockItemBox(float mode) {
    // 10 and 11 together, deliberately: a held block is a block-item box and must
    // reach the UV-rect resolution. This one is load-bearing history.
    return isItemMode(mode, kItemModeBlockItemDropped) ||
           isItemMode(mode, kItemModeBlockItemHeld);
}
[[nodiscard]] inline constexpr bool itemUsesMatrix(float mode) {
    return isItemMode(mode, kItemModeMatrixViewModel) ||
           isItemMode(mode, kItemModeWorldMatrixCuboid) ||
           isItemMode(mode, kItemModeBoxUvEntity) ||
           isItemMode(mode, kItemModeBlockItemHeld);
}
[[nodiscard]] inline constexpr bool itemHeldInViewSpace(float mode) {
    return isItemMode(mode, kItemModeHeldSprite) ||
           isItemMode(mode, kItemModeViewSkinCuboid) ||
           isItemMode(mode, kItemModeMatrixViewModel) ||
           isItemMode(mode, kItemModeBlockItemHeld);
}
// The one category with a condition beyond membership: mode 6 joins only when
// `data.w` says the draw is a skin. Kept as it is; the members are still listed.
[[nodiscard]] inline constexpr bool itemPlayerSkinCuboid(float mode, float dataW) {
    return isItemMode(mode, kItemModeViewSkinCuboid) ||
           isItemMode(mode, kItemModeArticulatedCuboid) ||
           isItemMode(mode, kItemModeWorldMatrixCuboid) ||
           (isItemMode(mode, kItemModeMatrixViewModel) && !itemBlockItemBox(mode) &&
            dataW > 0.5F);
}
// The billboard tail's own categories. `itemAtlasBillboard` was written out twice
// in the shader (the UV and the opacity each spelled the compound condition), and
// two copies of a condition is one of them drifting.
[[nodiscard]] inline constexpr bool itemHeldBillboard(float mode) {
    return isItemMode(mode, kItemModeHeldBillboard) ||
           isItemMode(mode, kItemModeMatrixHeldBillboard);
}
[[nodiscard]] inline constexpr bool itemMatrixHeldBillboard(float mode) {
    return isItemMode(mode, kItemModeMatrixHeldBillboard);
}
[[nodiscard]] inline constexpr bool itemAtlasBillboard(float mode) {
    return isItemMode(mode, kItemModeAtlasBillboard);
}

// --- Where a block-item face's UV rect lives, and why it is written in one place
//
// The regression this exists to prevent: mode 10 put the rect in
// `data.yzw` + `positionSize.w`, mode 11 put all four numbers in
// `positionSize.xyzw`, and the shader reads only the first arrangement. Held
// blocks therefore got rect (0, 0, 0, maxV) — zero width in U — and every face
// stretched the atlas layer's u=0 column across itself. Nothing else showed,
// because a held block takes its position from the matrix and never looks at
// `positionSize.xyz`, which is why it survived the round that fixed the icons.
//
// The two modes' push blocks are declared identically and were checked to be so.
// Identical declarations do not make two producers agree about which field holds
// what; that is a separate property and it needs its own guardrail.
//
// So neither call site decides. Both call `setBlockItemFaceRect`, and
// `blockItemFaceRectOf` reads back what item_entity.vert reads — the two are
// mirrors of shader lines that `hud_push_constant_test` re-reads from the source.
inline constexpr void setBlockItemFaceRect(ItemPush& push, const world::ItemModelFace& face) {
    // 0..16 model units into 0..1 of the atlas layer, which is what the shader's
    // `rectCornerUv` expects.
    constexpr float kModelUnitsPerSprite = 16.0F;
    push.data.y = face.uv.minU / kModelUnitsPerSprite;
    push.data.z = face.uv.minV / kModelUnitsPerSprite;
    push.data.w = face.uv.maxU / kModelUnitsPerSprite;
    push.positionSize.w = face.uv.maxV / kModelUnitsPerSprite;
}

// The rect exactly as item_entity.vert reconstructs it:
//   vec4 itemRect = vec4(item.data.y, item.data.z, item.data.w, item.positionSize.w);
[[nodiscard]] inline constexpr glm::vec4 blockItemFaceRectOf(const ItemPush& push) {
    return {push.data.y, push.data.z, push.data.w, push.positionSize.w};
}

// The whole push for one face, per mode. The two differ in how the box is placed
// and in nothing else; both get their rect from the writer above, and neither
// call site assembles an ItemPush of its own any more — which is what keeps
// "these two modes fill the block the same way" a property of the code rather
// than of two people remembering.
[[nodiscard]] inline ItemPush makeDroppedBlockItemFacePush(const world::ItemModelFace& face,
                                                           float atlasLayer, glm::vec3 boxCentre,
                                                           glm::vec3 size, float yawRadians,
                                                           float packedLight) {
    ItemPush push{};
    // World space: the box's centre travels in positionSize.xyz and the yaw in
    // textureLayersRotation.w, because this draw has no matrix.
    push.positionSize = {boxCentre.x, boxCentre.y, boxCentre.z, 0.0F};
    push.textureLayersRotation = {atlasLayer, static_cast<float>(face.quadrant), 0.0F, yawRadians};
    push.data = {kItemModeBlockItemDropped, 0.0F, 0.0F, 0.0F};
    push.dimensions = {size.x, size.y, size.z, packedLight};
    setBlockItemFaceRect(push, face);
    return push;
}

[[nodiscard]] inline ItemPush makeHeldBlockItemFacePush(const world::ItemModelFace& face,
                                                        float atlasLayer, glm::vec3 size,
                                                        float packedLight,
                                                        const glm::mat4& boxTransform) {
    ItemPush push{};
    // The matrix places this one, so positionSize.xyz stays zero. That is exactly
    // why the old code could write the UV rect across all four of its components
    // and produce no symptom except a texture stretched from one column of texels.
    push.positionSize = {0.0F, 0.0F, 0.0F, 0.0F};
    push.textureLayersRotation = {atlasLayer, static_cast<float>(face.quadrant), 0.0F, 0.0F};
    push.data = {kItemModeBlockItemHeld, 0.0F, 0.0F, 0.0F};
    push.dimensions = {size.x, size.y, size.z, packedLight};
    push.viewModelTransform = boxTransform;
    setBlockItemFaceRect(push, face);
    return push;
}

// --- ItemPush's per-mode field map (the accounting the shader comment promised)
//
// One pipeline serves twelve draw kinds, so most of this block IS mode-dependent
// — unlike HudPush, where the reuse was a defect and was removed. Removing it
// here would mean either several pipelines or a uniform buffer, and that is a
// change to make deliberately rather than as a side effect of a bug fix. What is
// recorded instead is which fields carry different things, so the next reader
// does not have to reconstruct it from the dispatch chain:
//
//   positionSize.xyz  world position (0/1/2/10) · the UNINFLATED cube size that
//                     the box-UV net is built from (9) · unused where a matrix
//                     places the draw (6/8/11)
//   positionSize.w    scale (0/1/2) · the block-item face's maxV (10/11) ·
//                     a packed 0xRRGGBB wool tint (9)
//   textureLayersRotation.xyz  three atlas layers, or one layer plus the model's
//                     declared texture width/height (9)
//   textureLayersRotation.w    yaw (most) · the face-override bits (9)
//   data.x            the draw mode. Never anything else.
//   data.y            pitch (most) · shadow opacity (2) · the block-item face's
//                     minU (10/11) · the box-UV net origin U (9)
//   data.z            roll (most) · the block-item face's minV (10/11) ·
//                     the net origin V (9)
//   data.w            a player-skin flag (6) · the block-item face's maxU (10/11)
//                     · the net mirror flag (9)
//   dimensions.xyz    the drawn cube extent, when it is not cubic
//   dimensions.w      the packed scene lightmap, plus 512 for the hurt row (9)
//
// The four block-item entries are the ones that had two producers, and they are
// the ones now written in one place above. The rest have a single producer each.

} // namespace mc::render

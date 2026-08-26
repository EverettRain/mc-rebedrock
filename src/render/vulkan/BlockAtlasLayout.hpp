#pragma once

// The fixed layer layout of the block/entity/effect texture array, shared by
// the texture baker (TextureManager, which places each source image on its
// layer) and the renderer's draw passes and sky shader (which sample those
// layers). Keeping the bases in one header means the two sides can never drift
// when the atlas layout changes.

#include <cstddef>
#include <cstdint>

namespace mc::render {

// The animated water/lava frame counts baked into the special section.
inline constexpr std::uint32_t kWaterAnimationFrameCount = 32;
inline constexpr std::uint32_t kLavaStillFrameCount = 20;
inline constexpr std::uint32_t kLavaFlowFrameCount = 16;

// The block/entity/effect atlas opens with a fixed special section (the
// animated water/lava frames, the player-skin cuboids, the chest parts, the
// destroy stages, the sun and the moon phases), in a deterministic order so the
// mesher and the sky shader can keep constexpr bases for them. After it come the
// block textures, resolved by name from the block registry at startup (no
// placeholders — every layer is a real texture), then one layer per registered
// item. The total is computed at runtime from the built atlas.
inline constexpr std::uint32_t kWaterStillLayer = 0U;
inline constexpr std::uint32_t kWaterFlowLayer = 32U;
inline constexpr std::uint32_t kLavaStillLayer = 64U;
inline constexpr std::uint32_t kLavaFlowLayer = 84U;
// The first block-texture layer: everything before it is the fixed special
// section (water 0-63, lava 64-99, player skin 100-135, destroy 136-145, chest
// parts 146-163, chest item faces 164-166, moon 167-174, sun 175, experience
// orb 176). The furnace front used to sit at 167-168 but is now a normal
// name-driven block texture (a DirectionalCube slot), so the section is two
// layers shorter and everything after the chest items shifted down by two.
inline constexpr std::uint32_t kFirstBlockTextureLayer = 177U;
inline constexpr float kPlayerHeadFirstLayer = 100.0F;
inline constexpr float kPlayerBodyFirstLayer = 106.0F;
inline constexpr float kPlayerRightArmFirstLayer = 112.0F;
inline constexpr float kPlayerLeftArmFirstLayer = 118.0F;
inline constexpr float kPlayerRightLegFirstLayer = 124.0F;
inline constexpr float kPlayerLeftLegFirstLayer = 130.0F;
inline constexpr float kDestroyStageFirstLayer = 136.0F;
inline constexpr float kChestBaseFirstLayer = 146.0F;
inline constexpr float kChestLidFirstLayer = 152.0F;
inline constexpr float kChestItemTopLayer = 164.0F;
inline constexpr float kChestItemFrontLayer = 165.0F;
inline constexpr float kChestItemSideLayer = 166.0F;
// Eight 26.1 environment/celestial/moon/<phase>.png sprites fill fixed layers
// 167-174 and environment/celestial/sun.png sits at 175. The sky shader reads them through the
// CameraUniform.celestialLayers uniform (which the renderer fills from these
// bases), so the two never drift when the atlas layout changes.
inline constexpr float kMoonPhaseFirstLayer = 167.0F;
inline constexpr float kSunLayer = 175.0F;
// One 16x16 orb sprite lifted from entity/experience/experience_orb.png (a 4x4
// sheet); the experience-orb billboards in WorldRenderer sample this whole layer.
inline constexpr float kExperienceOrbLayer = 176.0F;

} // namespace mc::render

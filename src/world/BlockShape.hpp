#pragma once

#include "world/Block.hpp"
#include "world/BlockState.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mc::world {

// The one place a block's shape is answered.
//
// What this replaces, and why it exists: a block's geometry used to be re-stated
// independently at every consumer — `collisionSpan` for the walk, a torch/crop/
// farmland switch inside VoxelRaycast for the pick ray, another switch for the
// selection outline, `appendSlab` in the mesher. Adding a slab to four of those
// and missing the fifth is exactly how the pick ray came to treat a half slab as
// a full cube: the outline hugged the half box while the ray hit the whole cell.
//
// 26.1 has no such split: `BlockBehaviour.getShape` is the single override and
// collision, interaction, visual, occlusion and support shapes all derive from
// it. This is that single source. It deliberately does *not* port 26.1's
// VoxelShape machinery (a subdivided-grid bitset plus the `Shapes.join` boolean
// algebra): that algebra exists to compose datapack-defined shapes at load time,
// and this project has a fixed, closed set of shapes, so a small AABB box set is
// enough — as the mechanism-fix rule "don't copy an abstraction with no
// consumer" already dictates.
//
// The representation is a three-way discriminant, chosen so the hot path pays
// nothing new. The overwhelming majority of cells are a full cube or empty; a
// slab and farmland fill their whole 1x1 footprint and differ only in height.
// Those are `Column`, answered with two floats and no box iteration — the exact
// cost of today's `collisionSpan`. Only genuinely multi-box shapes (stairs,
// fences, doors, and the torch's offset box) are `Boxes`, and only the cells
// that hold them ever walk a box list.
//
// `blockShape` answers a block's *base* shape — 26.1's `getShape`, the geometry
// the pick ray, the selection outline and (filtered by `hasCollision`) the
// collision box all derive from. A no-collision decoration such as a torch or a
// flower still has a base shape here (its slim interaction box); it simply
// contributes nothing to collision. That is why the ray now hits a slab's half
// box, a flower's stalk and a chest's 14/16 box rather than the whole cell: the
// one source answers all three consumers, so none can drift from the others.

// A single axis-aligned box in cell-local 0..1 units. Plain floats rather than a
// glm vector so the whole shape table stays constexpr and header-only without
// pulling glm into every translation unit that asks a block its height.
struct ShapeBox final {
    float minX = 0.0F;
    float minY = 0.0F;
    float minZ = 0.0F;
    float maxX = 1.0F;
    float maxY = 1.0F;
    float maxZ = 1.0F;
};

enum class ShapeKind : std::uint8_t {
    Empty,  // no shape at all — air, and the collision shape of a torch or plant
    Column, // fills the whole 1x1 footprint, [bottom, top] tall: cube/slab/farmland
    Boxes,  // an explicit set of boxes: stairs, fences, doors, offset torch box
};

// A block state's shape. `bottom`/`top` carry the Column span; `boxes` carries
// the Boxes set. The two never both apply — the kind says which to read — so
// they share the struct rather than a variant, keeping it a trivial value.
struct BlockShape final {
    ShapeKind kind = ShapeKind::Empty;
    float bottom = 0.0F;
    float top = 0.0F;
    std::span<const ShapeBox> boxes{};
};

// The fixed box sets a Boxes shape spans, in rodata. The torch boxes are the
// exact axis-aligned bounds the old torchSelectionBox computed (a floor torch,
// and a wall torch that leans out into its supporting neighbour), precomputed so
// this stays constexpr rather than running normalise/cross per query.
namespace detail {

inline constexpr ShapeBox kCrossBox{0.1F, 0.0F, 0.1F, 0.9F, 0.8F, 0.9F};
inline constexpr ShapeBox kChestBox{0.0625F, 0.0F, 0.0625F, 0.9375F, 0.875F, 0.9375F};
inline constexpr ShapeBox kFloorTorchBox{0.4375F, 0.0F, 0.4375F, 0.5625F, 0.625F, 0.5625F};

// Indexed by BlockOrientation North/East/South/West (0..3).
inline constexpr std::array<ShapeBox, 4> kWallTorchBox{{
    {0.437500F, 0.152828F, 0.663716F, 0.562500F, 0.787172F, 1.056284F}, // North
    {-0.056284F, 0.152828F, 0.437500F, 0.336284F, 0.787172F, 0.562500F}, // East
    {0.437500F, 0.152828F, -0.056284F, 0.562500F, 0.787172F, 0.336284F}, // South
    {0.663716F, 0.152828F, 0.437500F, 1.056284F, 0.787172F, 0.562500F}, // West
}};

// One shape handler per BlockModel. B1-2 replaces the shape's `switch(model)`
// with a table indexed by the block's model, the same DOD move kRandomTickTable
// makes for random ticks: the model is a definition field, so a block selects
// its shape through one array load instead of a switch the R1 audit flagged as a
// parallel list. Each handler derives the state's base shape for its model.
[[nodiscard]] constexpr BlockShape shapeCross(BlockState) {
    return {ShapeKind::Boxes, 0.0F, 0.0F, {&kCrossBox, 1}};
}
[[nodiscard]] constexpr BlockShape shapeChest(BlockState) {
    return {ShapeKind::Boxes, 0.0F, 0.0F, {&kChestBox, 1}};
}
[[nodiscard]] constexpr BlockShape shapeTorch(BlockState state) {
    if (state.block() == Block::WallTorch) {
        const auto index = static_cast<std::size_t>(state.orientation());
        const auto& box = kWallTorchBox[index < 4U ? index : 0U];
        return {ShapeKind::Boxes, 0.0F, 0.0F, {&box, 1}};
    }
    return {ShapeKind::Boxes, 0.0F, 0.0F, {&kFloorTorchBox, 1}};
}
[[nodiscard]] constexpr BlockShape shapeSlab(BlockState state) {
    switch (state.slabPortion()) {
    case SlabPortion::Bottom:
        return {ShapeKind::Column, 0.0F, 0.5F, {}};
    case SlabPortion::Top:
        return {ShapeKind::Column, 0.5F, 1.0F, {}};
    case SlabPortion::Double:
        return {ShapeKind::Column, 0.0F, 1.0F, {}};
    }
    return {ShapeKind::Column, 0.0F, 1.0F, {}};
}
[[nodiscard]] constexpr BlockShape shapeCrop(BlockState state) {
    return {ShapeKind::Column, 0.0F, cropSelectionHeight(state.age()), {}};
}
// The Cube model and the catch-all: farmland's 15/16 box, air/fluid's empty
// shape, or a solid full cube.
[[nodiscard]] constexpr BlockShape shapeCube(BlockState state) {
    const Block block = state.block();
    if (isFarmland(block)) {
        return {ShapeKind::Column, 0.0F, kFarmlandModelHeight, {}};
    }
    // A cube-model block: a solid full cube, or air/fluid which has no shape.
    if (!isSelectable(block) && !hasCollision(block)) {
        return {ShapeKind::Empty, 0.0F, 0.0F, {}};
    }
    return {ShapeKind::Column, 0.0F, 1.0F, {}};
}

using BlockShapeFn = BlockShape (*)(BlockState);

// The per-model shape handlers indexed by BlockModel ordinal — shape dispatch as
// data. `blockShape` loads the block's model and calls through this, so the shape
// stays a single source with no switch(block...) to drift.
inline constexpr std::array<BlockShapeFn, 6> kShapeByModel{{
    &shapeCube,  // BlockModel::Cube
    &shapeCross, // BlockModel::Cross
    &shapeCrop,  // BlockModel::Crop
    &shapeTorch, // BlockModel::Torch
    &shapeChest, // BlockModel::Chest
    &shapeSlab,  // BlockModel::Slab
}};
static_assert(static_cast<std::size_t>(BlockModel::Cube) == 0U);
static_assert(static_cast<std::size_t>(BlockModel::Cross) == 1U);
static_assert(static_cast<std::size_t>(BlockModel::Crop) == 2U);
static_assert(static_cast<std::size_t>(BlockModel::Torch) == 3U);
static_assert(static_cast<std::size_t>(BlockModel::Chest) == 4U);
static_assert(static_cast<std::size_t>(BlockModel::Slab) == 5U);

} // namespace detail

// A block state's base shape. Column blocks (full cube, slab, farmland, a crop's
// per-stage box) fill their whole 1x1 footprint and differ only in height; the
// torch, chest and cross-plant are explicit Boxes; air and fluids are Empty (a
// fluid is not a solid the pick ray tests here, and neither collides).
[[nodiscard]] constexpr BlockShape blockShape(BlockState state) {
    const auto model = static_cast<std::size_t>(blockDefinition(state.block()).model);
    return detail::kShapeByModel[model](state);
}

// The vertical span [bottom, top] of a collision box within its cell, in 0..1
// cell-local units. An empty span (top <= bottom) means no collision.
struct BlockCollisionSpan final {
    float bottom = 0.0F;
    float top = 0.0F;
};

// The vertical extent [bottom, top] a shape occupies in its cell. Column is its
// own span; Boxes is the union of its boxes' y ranges; Empty is degenerate. This
// is what the vertical-only consumers (the walk, placement occupancy) read, and
// it lets a Boxes shape answer the "how tall" question without those consumers
// learning the box set until Slice C teaches them the full footprint.
[[nodiscard]] constexpr BlockCollisionSpan verticalSpanOf(const BlockShape& shape) {
    switch (shape.kind) {
    case ShapeKind::Empty:
        return {0.0F, 0.0F};
    case ShapeKind::Column:
        return {shape.bottom, shape.top};
    case ShapeKind::Boxes: {
        if (shape.boxes.empty()) {
            return {0.0F, 0.0F};
        }
        float bottom = shape.boxes.front().minY;
        float top = shape.boxes.front().maxY;
        for (const ShapeBox& box : shape.boxes) {
            bottom = box.minY < bottom ? box.minY : bottom;
            top = box.maxY > top ? box.maxY : top;
        }
        return {bottom, top};
    }
    }
    return {0.0F, 0.0F};
}

// The vertical span [bottom, top] of a state's collision box within its cell, in
// 0..1 cell-local units. A full cube is {0, 1}, a non-colliding block {0, 0},
// farmland the vanilla 15/16 box, a slab its half box. An empty span
// (top <= bottom) means no collision. Derived from `blockShape` — the block's
// base shape, filtered by whether the block collides — so the player walk, the
// creature walk and the placement occupancy check read the one shape source
// instead of each assuming a full cube. A torch or flower has a base shape but
// no collision, so it filters to an empty span.
[[nodiscard]] constexpr BlockCollisionSpan collisionSpan(BlockState state) {
    if (!hasCollision(state.block())) {
        return {};
    }
    return verticalSpanOf(blockShape(state));
}

// The state's collision shape: its base shape if it collides, else Empty. This
// is what the walk and placement test against — a torch or flower has a base
// shape for the pick ray but collides with nothing.
[[nodiscard]] constexpr BlockShape collisionShape(BlockState state) {
    if (!hasCollision(state.block())) {
        return {ShapeKind::Empty, 0.0F, 0.0F, {}};
    }
    return blockShape(state);
}

// Whether an axis-aligned query box overlaps a shape whose cell origin is
// (ox,oy,oz), all in world coordinates. A Column is tested on Y only — it fills
// its whole 1x1 footprint, so a caller that already iterates the cells the query
// box covers has established the horizontal overlap, and this stays the exact
// vertical-span test the walk used before box shapes existed. Boxes are tested
// in all three axes, since a fence post or stair step only fills part of its
// cell horizontally.
[[nodiscard]] constexpr bool shapeOverlaps(const BlockShape& shape, float ox, float oy, float oz,
                                           float qMinX, float qMinY, float qMinZ, float qMaxX,
                                           float qMaxY, float qMaxZ) {
    switch (shape.kind) {
    case ShapeKind::Empty:
        return false;
    case ShapeKind::Column:
        return qMinY < oy + shape.top && qMaxY > oy + shape.bottom;
    case ShapeKind::Boxes:
        for (const ShapeBox& box : shape.boxes) {
            if (qMinX < ox + box.maxX && qMaxX > ox + box.minX && qMinY < oy + box.maxY &&
                qMaxY > oy + box.minY && qMinZ < oz + box.maxZ && qMaxZ > oz + box.minZ) {
                return true;
            }
        }
        return false;
    }
    return false;
}

} // namespace mc::world

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

// A box rotated 90 degrees clockwise (viewed from above, +Y up) about the
// cell's horizontal centre: `(x, z) -> (1 - z, x)`. This is the exact mapping
// `kWallTorchBox`'s North->East pair above already exercises (its East box is
// this formula applied to its North box), so stair/door/gate boxes rotate
// through the same convention rather than a second, independently-checked one.
[[nodiscard]] constexpr ShapeBox rotatedClockwise(const ShapeBox& box) {
    const float x1 = 1.0F - box.maxZ;
    const float z1 = box.minX;
    const float x2 = 1.0F - box.minZ;
    const float z2 = box.maxX;
    return {x1 < x2 ? x1 : x2, box.minY, z1 < z2 ? z1 : z2,
            x1 < x2 ? x2 : x1, box.maxY, z1 < z2 ? z2 : z1};
}
[[nodiscard]] constexpr ShapeBox rotatedBy(const ShapeBox& box, BlockOrientation facing) {
    switch (facing) {
    case BlockOrientation::North:
        return box;
    case BlockOrientation::East:
        return rotatedClockwise(box);
    case BlockOrientation::South:
        return rotatedClockwise(rotatedClockwise(box));
    case BlockOrientation::West:
        return rotatedClockwise(rotatedClockwise(rotatedClockwise(box)));
    case BlockOrientation::Up:
    case BlockOrientation::Down:
        return box;
    }
    return box;
}
// A box mirrored top<->bottom about the cell's vertical centre (y -> 1 - y),
// the way a top stair/gate-post mirrors its bottom counterpart.
[[nodiscard]] constexpr ShapeBox invertedY(const ShapeBox& box) {
    return {box.minX, 1.0F - box.maxY, box.minZ, box.maxX, 1.0F - box.minY, box.maxZ};
}

// StairBlock's fixed box set, ported box-for-box from 26.1's SHAPE_OUTER /
// SHAPE_STRAIGHT / SHAPE_INNER (StairBlock.java): each is built at facing=North
// by composing the previous shape with its own 90-degree rotation, matching the
// Java `Shapes.or(previous, Shapes.rotate(previous, ROT_Y_90))` construction —
// see the derivation in AR-B2's design notes. `kStairFullBottom` is the shared
// full-footprint lower half every shape includes; the others are the
// shape-specific step box(es) on top of it (Straight's two adjacent
// quarter-boxes merge into one full-depth half, exactly as SHAPE_STRAIGHT's
// union does), each already in its North-facing orientation.
inline constexpr ShapeBox kStairFullBottom{0.0F, 0.0F, 0.0F, 1.0F, 0.5F, 1.0F};
inline constexpr ShapeBox kStairOuterStep{0.0F, 0.5F, 0.0F, 0.5F, 1.0F, 0.5F};
inline constexpr ShapeBox kStairStraightStep{0.0F, 0.5F, 0.0F, 1.0F, 1.0F, 0.5F};
inline constexpr ShapeBox kStairInnerStep2{0.5F, 0.5F, 0.5F, 1.0F, 1.0F, 1.0F};

// One (facing, half, shape) combination's box list, up to 3 boxes long
// (Straight/Outer use 2, Inner uses 3 — the trailing slots of a shorter entry
// are zero-initialised and never read past `count`).
struct StairBoxEntry final {
    std::array<ShapeBox, 3> boxes{};
    std::uint8_t count = 0U;
};

// Every stair box list, interned once at compile time and indexed by
// `stairShapeIndex` — the same "build the whole table in rodata, index it with
// arithmetic" move `kWallTorchBox` makes for its four entries, just wider.
// `blockShape` returns a `std::span` into whichever row this table computed, so
// the row must be a static array (never a per-call local) or the span would
// point at a destroyed stack temporary the moment the handler returns.
[[nodiscard]] constexpr std::size_t stairShapeIndex(BlockOrientation facing, SlabPortion half,
                                                    StairShape shape) {
    // facing(4) outermost, half(2), shape(5) innermost — an arbitrary but fixed
    // mixed-radix order, private to this table.
    return ((static_cast<std::size_t>(facing) * 2U) + static_cast<std::size_t>(half)) * 5U +
           static_cast<std::size_t>(shape);
}
[[nodiscard]] constexpr StairBoxEntry buildStairBoxEntry(BlockOrientation facing, SlabPortion half,
                                                         StairShape shape) {
    // INNER_LEFT/OUTER_RIGHT read the *rotated* facing key, exactly as
    // StairBlock#getShape's inner switch selects (StairBlock.java:85-89) —
    // the shape name is relative to the placed facing, not an independent axis.
    const BlockOrientation key = shape == StairShape::InnerLeft
        ? counterClockwiseOrientation(facing)
        : (shape == StairShape::OuterRight ? clockwiseOrientation(facing) : facing);
    const bool top = half == SlabPortion::Top;
    const auto place = [top](ShapeBox box) { return top ? invertedY(box) : box; };

    StairBoxEntry entry;
    entry.boxes[entry.count++] = place(rotatedBy(kStairFullBottom, key));
    entry.boxes[entry.count++] = place(rotatedBy(
        shape == StairShape::Straight ? kStairStraightStep : kStairOuterStep, key));
    if (shape == StairShape::InnerLeft || shape == StairShape::InnerRight) {
        entry.boxes[entry.count++] = place(rotatedBy(kStairInnerStep2, key));
    }
    return entry;
}
inline constexpr std::array<StairBoxEntry, 4U * 2U * 5U> kStairBoxTable = [] {
    std::array<StairBoxEntry, 4U * 2U * 5U> table{};
    for (std::size_t f = 0; f < 4U; ++f) {
        for (std::size_t h = 0; h < 2U; ++h) {
            for (std::size_t s = 0; s < 5U; ++s) {
                const auto facing = static_cast<BlockOrientation>(f);
                const auto half = static_cast<SlabPortion>(h);
                const auto shape = static_cast<StairShape>(s);
                table[stairShapeIndex(facing, half, shape)] = buildStairBoxEntry(facing, half, shape);
            }
        }
    }
    return table;
}();

// DoorBlock's single thin box (26.1's `Block.boxZ(16,13,16)`, which resolves to
// full X, full Y, z 13/16..1 at facing=North — a slab-thin leaf flush against
// the cell's far face), rotated by the *effective* swing direction: closed
// reads `facing` directly, open reads facing rotated toward the hinge exactly
// as DoorBlock#getShape computes `doorDirection` (right hinge turns
// counter-clockwise, left hinge clockwise) before indexing the same
// North-keyed table `SHAPES.get(doorDirection)` every closed door shares.
inline constexpr ShapeBox kDoorClosedBox{0.0F, 0.0F, 0.8125F, 1.0F, 1.0F, 1.0F};
[[nodiscard]] constexpr std::size_t doorShapeIndex(BlockOrientation facing, bool open,
                                                   DoorHinge hinge) {
    return (static_cast<std::size_t>(facing) * 2U + (open ? 1U : 0U)) * 2U +
           static_cast<std::size_t>(hinge);
}
inline constexpr std::array<ShapeBox, 4U * 2U * 2U> kDoorBoxTable = [] {
    std::array<ShapeBox, 4U * 2U * 2U> table{};
    for (std::size_t f = 0; f < 4U; ++f) {
        for (std::size_t o = 0; o < 2U; ++o) {
            for (std::size_t h = 0; h < 2U; ++h) {
                const auto facing = static_cast<BlockOrientation>(f);
                const bool open = o != 0U;
                const auto hinge = static_cast<DoorHinge>(h);
                BlockOrientation swing = facing;
                if (open) {
                    swing = hinge == DoorHinge::Right ? counterClockwiseOrientation(facing)
                                                       : clockwiseOrientation(facing);
                }
                table[doorShapeIndex(facing, open, hinge)] = rotatedBy(kDoorClosedBox, swing);
            }
        }
    }
    return table;
}();

// FenceGateBlock's post-pair box (26.1's `Block.cube(16,16,4)`: full X/Y, z
// 6/16..10/16 at the Z axis key — a gate spanning the cell on the axis
// perpendicular to travel), rotated by facing when closed and empty when open
// (FenceGateBlock#getCollisionShape/getBlockSupportShape both go `Shapes.empty()`
// on OPEN — the gate swings fully out of the way, unlike a door which still
// occupies a thin sliver). `kFenceGateEmpty` is an empty box list an open gate's
// entry points at, so shapeFenceGate stays a single-branch subscript rather
// than an if/else on `open` at read time.
inline constexpr ShapeBox kFenceGateClosedBox{0.0F, 0.0F, 0.375F, 1.0F, 1.0F, 0.625F};
inline constexpr std::array<ShapeBox, 4> kFenceGateBoxByFacing = [] {
    std::array<ShapeBox, 4> table{};
    for (std::size_t f = 0; f < 4U; ++f) {
        table[f] = rotatedBy(kFenceGateClosedBox, static_cast<BlockOrientation>(f));
    }
    return table;
}();

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
// AR-B2: StairBlock's Boxes shape, keyed by Facing x Half x StairShape into the
// interned `kStairBoxTable` row built above — the mesher, the pick ray and
// (filtered by hasCollision) the walk all see the same 2-or-3 box list.
[[nodiscard]] constexpr BlockShape shapeStairs(BlockState state) {
    const auto& entry = kStairBoxTable[stairShapeIndex(state.orientation(), state.stairHalf(),
                                                        state.stairShape())];
    return {ShapeKind::Boxes, 0.0F, 0.0F, {entry.boxes.data(), entry.count}};
}
// AR-B2: DoorBlock's Boxes shape — one thin leaf box, keyed by Facing x Open x
// Hinge into `kDoorBoxTable`. Both door halves (Half::Bottom/Top standing in
// for lower/upper) share the same in-cell box; only their Y position in the
// world differs, which is the cell origin the caller adds, not this shape.
[[nodiscard]] constexpr BlockShape shapeDoor(BlockState state) {
    const auto& box =
        kDoorBoxTable[doorShapeIndex(state.orientation(), state.open(), state.hinge())];
    return {ShapeKind::Boxes, 0.0F, 0.0F, {&box, 1}};
}
// AR-B2: FenceGateBlock's Boxes shape — the post-pair box on the Facing axis
// when closed, empty when open (the gate swings fully clear, unlike a door).
[[nodiscard]] constexpr BlockShape shapeFenceGate(BlockState state) {
    if (state.open()) {
        return {ShapeKind::Boxes, 0.0F, 0.0F, {}};
    }
    const auto index = static_cast<std::size_t>(state.orientation());
    const auto& box = kFenceGateBoxByFacing[index < 4U ? index : 0U];
    return {ShapeKind::Boxes, 0.0F, 0.0F, {&box, 1}};
}

using BlockShapeFn = BlockShape (*)(BlockState);

// The per-model shape handlers indexed by BlockModel ordinal — shape dispatch as
// data. `blockShape` loads the block's model and calls through this, so the shape
// stays a single source with no switch(block...) to drift.
inline constexpr std::array<BlockShapeFn, 9> kShapeByModel{{
    &shapeCube,      // BlockModel::Cube
    &shapeCross,     // BlockModel::Cross
    &shapeCrop,      // BlockModel::Crop
    &shapeTorch,     // BlockModel::Torch
    &shapeChest,     // BlockModel::Chest
    &shapeSlab,      // BlockModel::Slab
    &shapeStairs,    // BlockModel::Stairs
    &shapeDoor,      // BlockModel::Door
    &shapeFenceGate, // BlockModel::FenceGate
}};
static_assert(static_cast<std::size_t>(BlockModel::Cube) == 0U);
static_assert(static_cast<std::size_t>(BlockModel::Cross) == 1U);
static_assert(static_cast<std::size_t>(BlockModel::Crop) == 2U);
static_assert(static_cast<std::size_t>(BlockModel::Torch) == 3U);
static_assert(static_cast<std::size_t>(BlockModel::Chest) == 4U);
static_assert(static_cast<std::size_t>(BlockModel::Slab) == 5U);
static_assert(static_cast<std::size_t>(BlockModel::Stairs) == 6U);
static_assert(static_cast<std::size_t>(BlockModel::Door) == 7U);
static_assert(static_cast<std::size_t>(BlockModel::FenceGate) == 8U);

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

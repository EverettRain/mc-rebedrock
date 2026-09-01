#pragma once

#include "world/Block.hpp"
#include "world/BlockState.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mc::world {

// The six cell faces, in the mesher's own order. This lives here, next to the
// shape source, because RN-8a made face occlusion a question about geometry:
// `faceOccludesFully` below and the mesher's `shouldRenderFace` have to name the
// same six directions, and a second enum is how the two would drift apart.
enum class Face : std::uint8_t { PositiveX, NegativeX, PositiveY, NegativeY, PositiveZ, NegativeZ };
inline constexpr std::size_t kFaceCount = 6;

[[nodiscard]] constexpr Face oppositeFace(Face face) {
    switch (face) {
    case Face::PositiveX: return Face::NegativeX;
    case Face::NegativeX: return Face::PositiveX;
    case Face::PositiveY: return Face::NegativeY;
    case Face::NegativeY: return Face::PositiveY;
    case Face::PositiveZ: return Face::NegativeZ;
    case Face::NegativeZ: return Face::PositiveZ;
    }
    return face;
}

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

// TrapDoorBlock (AR-B3): the same `Block.boxZ(16,13,16)` thin-leaf box a door
// uses (26.1 TrapDoorBlock.java:51 — literally the identical formula, a 3/16
// slab-thin leaf flush against one face of the cell), but a trapdoor is one
// cell rather than two and its *closed* orientation lies flat against the
// floor or ceiling instead of always standing against the facing wall.
// `kTrapdoorBottomBox`/`kTrapdoorTopBox` are that formula rotated onto the
// horizontal plane (Down/Up in vanilla's `Shapes.rotateAll` terms) rather than
// reusing `invertedY` on the door's vertical box, since a 90-degree axis swap
// (vertical leaf -> horizontal slab) is not a Y-mirror. Open reuses the door's
// exact vertical box family (`rotatedBy(kDoorClosedBox, facing)` — a trapdoor
// has no Hinge axis, so it swings flush against its own Facing side, never a
// hinge-rotated one) via the shared `kDoorClosedBox`/`rotatedBy` primitives,
// which is the single-source point: two callers (door, trapdoor) reading one
// interned box rather than two independently-typed 0.8125 constants.
inline constexpr ShapeBox kTrapdoorBottomBox{0.0F, 0.0F, 0.0F, 1.0F, 0.1875F, 1.0F};
inline constexpr ShapeBox kTrapdoorTopBox{0.0F, 0.8125F, 0.0F, 1.0F, 1.0F, 1.0F};
[[nodiscard]] constexpr std::size_t trapdoorShapeIndex(BlockOrientation facing, SlabPortion half,
                                                       bool open) {
    return (static_cast<std::size_t>(facing) * 2U + static_cast<std::size_t>(half)) * 2U +
           (open ? 1U : 0U);
}
inline constexpr std::array<ShapeBox, 4U * 2U * 2U> kTrapdoorBoxTable = [] {
    std::array<ShapeBox, 4U * 2U * 2U> table{};
    for (std::size_t f = 0; f < 4U; ++f) {
        for (std::size_t h = 0; h < 2U; ++h) {
            for (std::size_t o = 0; o < 2U; ++o) {
                const auto facing = static_cast<BlockOrientation>(f);
                const auto half = static_cast<SlabPortion>(h);
                const bool open = o != 0U;
                ShapeBox box;
                if (open) {
                    // TrapDoorBlock#getShape: `SHAPES.get(state.getValue(FACING))`
                    // when OPEN — the swing side is Facing itself, no hinge.
                    box = rotatedBy(kDoorClosedBox, facing);
                } else {
                    // `SHAPES.get(HALF == TOP ? DOWN : UP)` — a closed trapdoor's
                    // key ignores Facing entirely; only which face it hangs from
                    // matters.
                    box = half == SlabPortion::Top ? kTrapdoorTopBox : kTrapdoorBottomBox;
                }
                table[trapdoorShapeIndex(facing, half, open)] = box;
            }
        }
    }
    return table;
}();

// BasePressurePlateBlock (AR-B3): `Block.column(14.0, 0.0, 1.0)` raised /
// `Block.column(14.0, 0.0, 0.5)` pressed (26.1 BasePressurePlateBlock.java:26-27)
// — a Column shape (full 1x1 footprint, height-only), exactly like a slab's,
// so it needs no box table at all; `shapePressurePlate` reads Powered directly.
inline constexpr float kPressurePlateRaisedHeight = 1.0F / 16.0F;
inline constexpr float kPressurePlatePressedHeight = 0.5F / 16.0F;

// ButtonBlock (AR-B3): `Block.boxZ(6.0, 4.0, 8.0, 16.0)` unpressed / pressed
// shrinks the box's protrusion by 2/16 (26.1 ButtonBlock.java:76-78 —
// `Shapes.join(attachFace..., pressed ? cube(14) : cube(12), ONLY_FIRST)`
// intersects the wall-face box against a slightly larger/smaller test cube,
// which for the wall-mounted case reduces to "how far the box protrudes off
// the wall shrinks by 2px while pressed"). Wall-mounted only, matching the
// button() builder's Lever-style simplification: no FACE (floor/ceiling) axis,
// so the box always protrudes from the wall behind `oppositeOrientation(facing)`
// the same way `kWallTorchBox`/`kFloorTorchBox` already encode a wall/floor
// split by hand rather than a data-driven attach-face table.
inline constexpr ShapeBox kButtonUnpressedBox{0.3125F, 0.375F, 0.75F, 0.6875F, 0.625F, 1.0F};
inline constexpr ShapeBox kButtonPressedBox{0.3125F, 0.375F, 0.8125F, 0.6875F, 0.625F, 1.0F};
[[nodiscard]] constexpr std::size_t buttonShapeIndex(BlockOrientation facing, bool powered) {
    return static_cast<std::size_t>(facing) * 2U + (powered ? 1U : 0U);
}
inline constexpr std::array<ShapeBox, 6U * 2U> kButtonBoxTable = [] {
    std::array<ShapeBox, 6U * 2U> table{};
    for (std::size_t f = 0; f < 6U; ++f) {
        for (std::size_t p = 0; p < 2U; ++p) {
            const auto facing = static_cast<BlockOrientation>(f);
            const bool powered = p != 0U;
            const ShapeBox& base = powered ? kButtonPressedBox : kButtonUnpressedBox;
            // Only the horizontal four are meaningfully distinct (a button's
            // Facing axis technically spans six values through the shared
            // .state(Facing, 6) call, but the wall-only placement this pass
            // wires only ever produces a horizontal one) — rotatedBy already
            // answers Up/Down as identity, which is a safe, inert default for
            // a facing this content never actually places.
            table[buttonShapeIndex(facing, powered)] = rotatedBy(base, facing);
        }
    }
    return table;
}();

// WallBlock (AR-B3): a centre post plus one arm per connected side, the same
// "post + per-direction arm" shape family the AR-B1 handoff notes describe for
// a fence (`FenceBoxSet`/`makeFenceBoxSet` in the abandoned wip/fence-m1-m6
// branch), sized to vanilla's actual wall pixels rather than the fence's
// (26.1 WallBlock.java:71-74: an 8px-square post 0..16 tall, and a 6px-wide arm
// reaching from the post to the cell edge, 0..16 tall — this pass folds
// vanilla's separate LOW(14px)/TALL(16px) visual-vs-collision split into one
// full-height arm, since the WallNorth/East/South/West axis here is a plain
// connected/not-connected bool rather than vanilla's three-value WallSide).
// The post is unconditional (this pass does not carry vanilla's UP
// raise/lower logic either — a post-less wall segment is a later refinement,
// tracked in the task's known-simplifications).
inline constexpr ShapeBox kWallPostBox{0.25F, 0.0F, 0.25F, 0.75F, 1.0F, 0.75F};
// North arm: reaches from the post's near edge (z=0.25) to the cell's far
// edge (z=0.0), 6px wide (x 0.3125..0.6875), full height.
inline constexpr ShapeBox kWallArmNorth{0.3125F, 0.0F, 0.0F, 0.6875F, 1.0F, 0.25F};
[[nodiscard]] constexpr unsigned wallConnectionMask(bool north, bool east, bool south, bool west) {
    return (north ? 1U : 0U) | (east ? 2U : 0U) | (south ? 4U : 0U) | (west ? 8U : 0U);
}
struct WallBoxSet final {
    std::array<ShapeBox, 5> boxes{}; // post + up to 4 arms
    std::uint8_t count = 0U;
};
[[nodiscard]] constexpr WallBoxSet buildWallBoxSet(unsigned mask) {
    WallBoxSet set;
    set.boxes[set.count++] = kWallPostBox;
    if ((mask & 1U) != 0U) { // North
        set.boxes[set.count++] = kWallArmNorth;
    }
    if ((mask & 2U) != 0U) { // East: North arm rotated 90 clockwise
        set.boxes[set.count++] = rotatedClockwise(kWallArmNorth);
    }
    if ((mask & 4U) != 0U) { // South: North arm rotated 180
        set.boxes[set.count++] = rotatedClockwise(rotatedClockwise(kWallArmNorth));
    }
    if ((mask & 8U) != 0U) { // West: North arm rotated 270 clockwise
        set.boxes[set.count++] =
            rotatedClockwise(rotatedClockwise(rotatedClockwise(kWallArmNorth)));
    }
    return set;
}
inline constexpr std::array<WallBoxSet, 16> kWallBoxTable = [] {
    std::array<WallBoxSet, 16> table{};
    for (unsigned mask = 0; mask < 16U; ++mask) {
        table[mask] = buildWallBoxSet(mask);
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
    // Any wall-mounted torch (WallTorch and RedstoneWallTorch alike) leans off
    // its wall, so its pick/outline/collision box is the leaning wall box at its
    // FACING; the upright floor box is for the ground torches. Keyed on the
    // isWallTorch trait, not a block identity, so the redstone wall torch is not
    // left with a floor box in the wrong cell (which made it unhittable by the
    // pick ray — you could not break it).
    if (isWallTorch(state.block())) {
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
    // A truncated cube is whatever declared `.height()` — read the property, do
    // not ask which block this is. The identity check this replaces named only
    // Farmland, so DirtPath (Block.hpp, `.height(0.9375F)`) got a full-cube shape
    // and stood 1/16 too tall under the pick ray and the walk. Reading the
    // declaration fixes both blocks at once and makes the next `.height()` block
    // correct on arrival — the same "no identity check, read the declared
    // property" rule `canBeSubmerged` is built on.
    const float declaredHeight = blockDefinition(block).modelHeight;
    if (declaredHeight < 1.0F) {
        return {ShapeKind::Column, 0.0F, declaredHeight, {}};
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
    // The outline / visual / pick shape is the facing post-pair box whether the
    // gate is open or closed: vanilla FenceGateBlock#getShape ignores OPEN, so an
    // open gate is still drawn and still selectable. Only the *collision* shape
    // empties when open (see `collisionShape`), which is what lets an entity walk
    // through. Returning empty here — as this used to — made an open gate vanish
    // from the mesh and the pick ray, so it could no longer be seen or broken.
    const auto index = static_cast<std::size_t>(state.orientation());
    const auto& box = kFenceGateBoxByFacing[index < 4U ? index : 0U];
    return {ShapeKind::Boxes, 0.0F, 0.0F, {&box, 1}};
}
// AR-B3: TrapDoorBlock's Boxes shape — one thin box keyed by Facing x Half x
// Open into kTrapdoorBoxTable, mirroring shapeDoor's structure exactly (one
// table lookup, one-box span) since a trapdoor's shape is likewise always a
// single thin leaf, just repositioned rather than reshaped.
[[nodiscard]] constexpr BlockShape shapeTrapdoor(BlockState state) {
    const auto& box = kTrapdoorBoxTable[trapdoorShapeIndex(state.orientation(),
                                                            state.trapdoorHalf(), state.open())];
    return {ShapeKind::Boxes, 0.0F, 0.0F, {&box, 1}};
}
// AR-B3: BasePressurePlateBlock's Column shape — full 1x1 footprint, height
// only, exactly like a slab's; Powered picks the raised or pressed height
// (the same "state answers a different Column span" move a slab's SlabType
// already makes).
[[nodiscard]] constexpr BlockShape shapePressurePlate(BlockState state) {
    const float top = state.powered() ? kPressurePlatePressedHeight : kPressurePlateRaisedHeight;
    return {ShapeKind::Column, 0.0F, top, {}};
}
// AR-B3: ButtonBlock's Boxes shape — one small box keyed by Facing x Powered
// into kButtonBoxTable, protruding slightly less off the wall while pressed.
[[nodiscard]] constexpr BlockShape shapeButton(BlockState state) {
    const auto& box = kButtonBoxTable[buttonShapeIndex(state.orientation(), state.powered())];
    return {ShapeKind::Boxes, 0.0F, 0.0F, {&box, 1}};
}
// AR-B3: WallBlock's Boxes shape — a post plus one arm per connected side,
// keyed by the four WallNorth/East/South/West axes packed into a 4-bit mask
// into kWallBoxTable, the same "connection state -> interned box-set table"
// shape shapeStairs already established for its own (facing,half,shape) key.
[[nodiscard]] constexpr BlockShape shapeWall(BlockState state) {
    const auto mask = wallConnectionMask(state.wallConnected(BlockOrientation::North),
                                        state.wallConnected(BlockOrientation::East),
                                        state.wallConnected(BlockOrientation::South),
                                        state.wallConnected(BlockOrientation::West));
    const auto& set = kWallBoxTable[mask];
    return {ShapeKind::Boxes, 0.0F, 0.0F, {set.boxes.data(), set.count}};
}

// RN-4a-2: the pick-ray / selection-outline shape of an ElementModel block. The
// diodes (repeater/comparator) are a thin full-footprint slab — 2/16 tall, the
// same as vanilla's collision — while the lever is a small centred nub. All three
// are noCollision, so this is only the interaction box, never a physics box. This
// also fixes the lever, which defaulted to Cube and so used a full-cube pick box.
// ENCH-3: AnvilBlock's four stacked boxes, in the Z-facing orientation
// (`Shapes.or(column(12,0,4), column(8,10,4,5), column(4,8,5,10),
// column(10,16,10,16))`). Vanilla rotates the whole set by the facing's AXIS
// only — the anvil is symmetric front-to-back — so two arrays cover all four
// facings.
inline constexpr std::array<ShapeBox, 4> kAnvilBoxesZ{{
    {2.0F / 16.0F, 0.0F, 2.0F / 16.0F, 14.0F / 16.0F, 4.0F / 16.0F, 14.0F / 16.0F},
    {4.0F / 16.0F, 4.0F / 16.0F, 3.0F / 16.0F, 12.0F / 16.0F, 5.0F / 16.0F, 13.0F / 16.0F},
    {6.0F / 16.0F, 5.0F / 16.0F, 4.0F / 16.0F, 10.0F / 16.0F, 10.0F / 16.0F, 12.0F / 16.0F},
    {3.0F / 16.0F, 10.0F / 16.0F, 0.0F, 13.0F / 16.0F, 1.0F, 1.0F},
}};
// The same four boxes with x and z swapped, for a block facing east or west.
inline constexpr std::array<ShapeBox, 4> kAnvilBoxesX{{
    {2.0F / 16.0F, 0.0F, 2.0F / 16.0F, 14.0F / 16.0F, 4.0F / 16.0F, 14.0F / 16.0F},
    {3.0F / 16.0F, 4.0F / 16.0F, 4.0F / 16.0F, 13.0F / 16.0F, 5.0F / 16.0F, 12.0F / 16.0F},
    {4.0F / 16.0F, 5.0F / 16.0F, 6.0F / 16.0F, 12.0F / 16.0F, 10.0F / 16.0F, 10.0F / 16.0F},
    {0.0F, 10.0F / 16.0F, 3.0F / 16.0F, 1.0F, 1.0F, 13.0F / 16.0F},
}};

[[nodiscard]] constexpr BlockShape shapeElementModel(BlockState state) {
    if (state.block() == Block::Lever) {
        return {ShapeKind::Boxes, 0.0F, 0.0F, {&kFloorTorchBox, 1}};
    }
    // ENCH-2: EnchantingTableBlock's SHAPE is `Block.column(16.0, 0.0, 12.0)` —
    // the full footprint, 12/16 tall. Unlike the diodes and the lever it is a
    // collidable block, so this Column is both the pick box and the box the
    // player stands on; walking onto a table steps up 3/4 of a block, as in
    // vanilla.
    if (state.block() == Block::EnchantingTable) {
        return {ShapeKind::Column, 0.0F, 12.0F / 16.0F, {}};
    }
    if (state.block() == Block::Anvil || state.block() == Block::ChippedAnvil ||
        state.block() == Block::DamagedAnvil) {
        const bool alongX = state.orientation() == BlockOrientation::East ||
                            state.orientation() == BlockOrientation::West;
        return {ShapeKind::Boxes, 0.0F, 0.0F,
                alongX ? std::span<const ShapeBox>{kAnvilBoxesX}
                       : std::span<const ShapeBox>{kAnvilBoxesZ}};
    }
    return {ShapeKind::Column, 0.0F, 2.0F / 16.0F, {}};
}
// RN-6: redstone dust — a 1/16-thin full-footprint pick box on the floor
// (RedStoneWireBlock's flat shape). noCollision, so this is only the pick ray.
[[nodiscard]] constexpr BlockShape shapeRedstoneWire(BlockState) {
    return {ShapeKind::Column, 0.0F, 1.0F / 16.0F, {}};
}
// RN-7: fire is noCollision (collisionShape filters it to Empty via hasCollision,
// so the player walks straight through), but it still needs a *selection* box so
// the pick ray can target it — that is what lets a left-click break it, i.e.
// extinguish it, the way vanilla lets you punch fire out. The earlier Empty
// shape made fire unselectable and therefore impossible to put out by hand. A
// full-cell outline (the flames fill the cell footprint) is the simplest box the
// ray can hit; collision stays off because Fire is declared noCollision().
[[nodiscard]] constexpr BlockShape shapeFire(BlockState) {
    return {ShapeKind::Column, 0.0F, 1.0F, {}};
}

using BlockShapeFn = BlockShape (*)(BlockState);

// The per-model shape handlers indexed by BlockModel ordinal — shape dispatch as
// data. `blockShape` loads the block's model and calls through this, so the shape
// stays a single source with no switch(block...) to drift.
inline constexpr std::array<BlockShapeFn, 17> kShapeByModel{{
    &shapeCube,          // BlockModel::Cube
    &shapeCross,         // BlockModel::Cross
    &shapeCrop,          // BlockModel::Crop
    &shapeTorch,         // BlockModel::Torch
    &shapeChest,         // BlockModel::Chest
    &shapeSlab,          // BlockModel::Slab
    &shapeStairs,        // BlockModel::Stairs
    &shapeDoor,          // BlockModel::Door
    &shapeFenceGate,     // BlockModel::FenceGate
    &shapeTrapdoor,      // BlockModel::TrapDoor
    &shapePressurePlate, // BlockModel::PressurePlate
    &shapeButton,        // BlockModel::Button
    &shapeWall,          // BlockModel::Wall
    &shapeCube,          // BlockModel::DirectionalCube (RN-4a: a full cube shape)
    &shapeElementModel,  // BlockModel::ElementModel (RN-4a-2: diode slab / lever nub)
    &shapeRedstoneWire,  // BlockModel::RedstoneWire (RN-6: flat 1/16 floor box)
    &shapeFire,          // BlockModel::Fire (RN-7: empty — no interaction box)
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
static_assert(static_cast<std::size_t>(BlockModel::TrapDoor) == 9U);
static_assert(static_cast<std::size_t>(BlockModel::PressurePlate) == 10U);
static_assert(static_cast<std::size_t>(BlockModel::Button) == 11U);
static_assert(static_cast<std::size_t>(BlockModel::Wall) == 12U);
static_assert(static_cast<std::size_t>(BlockModel::DirectionalCube) == 13U);
static_assert(static_cast<std::size_t>(BlockModel::ElementModel) == 14U);
static_assert(static_cast<std::size_t>(BlockModel::RedstoneWire) == 15U);
static_assert(static_cast<std::size_t>(BlockModel::Fire) == 16U);
// Every BlockModel ordinal must have a shape handler; a missing entry is the
// out-of-bounds function-pointer read (a SIGBUS) that a new model would cause.
static_assert(kShapeByModel.size() == static_cast<std::size_t>(BlockModel::Fire) + 1U);

} // namespace detail

// A block state's base shape. Column blocks (full cube, slab, farmland, a crop's
// per-stage box) fill their whole 1x1 footprint and differ only in height; the
// torch, chest and cross-plant are explicit Boxes; air and fluids are Empty (a
// fluid is not a solid the pick ray tests here, and neither collides).
[[nodiscard]] constexpr BlockShape blockShape(BlockState state) {
    const auto model = static_cast<std::size_t>(blockDefinition(state.block()).model);
    return detail::kShapeByModel[model](state);
}

namespace detail {

// Does this one box cover the whole 1x1 cell wall on `face`? It must reach that
// wall on the face's own axis and span the cell edge-to-edge on the other two.
[[nodiscard]] constexpr bool boxSealsFace(const ShapeBox& box, Face face) {
    const bool fullX = box.minX <= 0.0F && box.maxX >= 1.0F;
    const bool fullY = box.minY <= 0.0F && box.maxY >= 1.0F;
    const bool fullZ = box.minZ <= 0.0F && box.maxZ >= 1.0F;
    switch (face) {
    case Face::PositiveX: return box.maxX >= 1.0F && fullY && fullZ;
    case Face::NegativeX: return box.minX <= 0.0F && fullY && fullZ;
    case Face::PositiveY: return box.maxY >= 1.0F && fullX && fullZ;
    case Face::NegativeY: return box.minY <= 0.0F && fullX && fullZ;
    case Face::PositiveZ: return box.maxZ >= 1.0F && fullX && fullY;
    case Face::NegativeZ: return box.minZ <= 0.0F && fullX && fullY;
    }
    return false;
}

} // namespace detail

// RN-8a: does this shape seal `face` — cover that whole cell wall, so a
// neighbour's facing quad can never be seen through it? This is 26.1's
// `getFaceOcclusionShape(dir) == Shapes.block()` test (Block.shouldRenderFace,
// Block.java:304), and it is what replaces the mesher's old renderLayer guess.
//
// It deliberately does not port 26.1's `Shapes.join` union algebra, for the
// reason this header's preamble already gives: a face counts as sealed when a
// *single* box in the set covers it. Today's roster has exactly one shape where
// that differs from the union answer — a straight stair's back face, sealed by
// its lower slab plus its step together — and that costs nothing, because a
// stair's render layer is Cutout, so `canOcclude` is false and its mask is zero
// either way. RN-8e, which is where stairs would gain an `occludes` bit, is
// where the union has to arrive with it; the call sites do not change when it
// does, only this function's Boxes arm.
[[nodiscard]] constexpr bool faceOccludesFully(const BlockShape& shape, Face face) {
    switch (shape.kind) {
    case ShapeKind::Empty:
        return false;
    case ShapeKind::Column:
        // A Column always fills its whole 1x1 footprint, so each cap only has to
        // reach its own end of the cell, while a side face needs the full height.
        switch (face) {
        case Face::PositiveY:
            return shape.top >= 1.0F;
        case Face::NegativeY:
            return shape.bottom <= 0.0F;
        default:
            return shape.bottom <= 0.0F && shape.top >= 1.0F;
        }
    case ShapeKind::Boxes:
        for (const ShapeBox& box : shape.boxes) {
            if (detail::boxSealsFace(box, face)) {
                return true;
            }
        }
        return false;
    }
    return false;
}

// The six faces this state seals, as a bit per `Face` (bit i = Face(i)). A block
// that cannot occlude at all masks to zero, so the mesher's per-face test is one
// bit and never has to ask the block registry a second question.
[[nodiscard]] constexpr std::uint8_t faceOcclusionMask(BlockState state) {
    if (!canOcclude(state.block())) {
        return 0U;
    }
    const BlockShape shape = blockShape(state);
    std::uint8_t mask = 0U;
    for (std::size_t f = 0; f < kFaceCount; ++f) {
        if (faceOccludesFully(shape, static_cast<Face>(f))) {
            mask |= static_cast<std::uint8_t>(1U << f);
        }
    }
    return mask;
}

// The sentinel bit `kOcclusionMaskByBlock` sets for a block whose mask the state
// decides (a slab's SlabType, an anvil's FACING). Bit 7, clear of the six face
// bits, so a resolved mask still fits the snapshot's six spare flag bits.
inline constexpr std::uint8_t kOcclusionStateDependent = 0x80U;

namespace detail {

// Whether a model's shape handler reads the state at all. Derived from the
// handler set rather than hand-listed per block, so that when RN-8e gives the
// geometrically-solid Cutout models an `occludes` bit, their sentinel appears
// with it instead of having to be remembered.
[[nodiscard]] constexpr bool shapeVariesWithState(BlockModel model) {
    switch (model) {
    case BlockModel::Slab:
    case BlockModel::Stairs:
    case BlockModel::Door:
    case BlockModel::FenceGate:
    case BlockModel::TrapDoor:
    case BlockModel::PressurePlate:
    case BlockModel::Button:
    case BlockModel::Wall:
    case BlockModel::Torch:
    case BlockModel::Crop:
    case BlockModel::ElementModel:
        return true;
    default:
        return false;
    }
}

} // namespace detail

// RN-8a's main table: one byte per block, resolved at compile time. The
// overwhelming majority of blocks answer the same six-face mask for every state
// (a Cube seals all six, a Cross or a torch seals none), so the mesher's
// snapshot fill reads this and is done; only a state-dependent block spends a
// `chunk->state()` + `blockShape()` to resolve its sentinel, once per cell at
// fill time and never on the per-face path.
//
// At ~350 bytes the whole table is L1-resident, unlike the 92 KB / 272 B-stride
// block registry the old per-face `renderLayer` lookups were randomly indexing.
inline constexpr std::array<std::uint8_t, static_cast<std::size_t>(Block::Count)>
    kOcclusionMaskByBlock = [] {
        std::array<std::uint8_t, static_cast<std::size_t>(Block::Count)> table{};
        for (std::size_t index = 0; index < table.size(); ++index) {
            const auto block = static_cast<Block>(index);
            if (!canOcclude(block)) {
                table[index] = 0U;
                continue;
            }
            if (detail::shapeVariesWithState(blockDefinition(block).model)) {
                table[index] = kOcclusionStateDependent;
                continue;
            }
            table[index] = faceOcclusionMask(BlockState{block});
        }
        return table;
    }();

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

// FenceGateBlock#getCollisionShape empties when the gate is open (entities pass
// straight through) even though its outline / visual shape stays put. Both the
// collision-span fast path and the full collisionShape below consult this so an
// open gate is passable without also making it invisible or unpickable.
[[nodiscard]] constexpr bool isOpenPassableGate(BlockState state) {
    return blockDefinition(state.block()).model == BlockModel::FenceGate && state.open();
}

[[nodiscard]] constexpr BlockCollisionSpan collisionSpan(BlockState state) {
    if (!hasCollision(state.block()) || isOpenPassableGate(state)) {
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
    // An open fence gate collides with nothing (entities pass through) even
    // though its outline / visual shape (blockShape, above) stays the post box
    // so it is still drawn and selectable by the pick ray.
    if (isOpenPassableGate(state)) {
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

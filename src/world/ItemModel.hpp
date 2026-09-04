#pragma once

// RN-14: what a block ITEM is, as geometry.
//
// Vanilla answers this with `assets/minecraft/items/<id>.json`, whose `model`
// field names a model to render: `oak_stairs` points at `block/oak_stairs`, so a
// stair item IS the stair model. Nothing about that is a texture question, which
// is why "the stairs are not registered" was the wrong diagnosis for "a stair in
// my inventory looks exactly like a plank": stairs are in the catalogue, and
// their icon was a full 16x16x16 cube wearing `oak_planks` — the parent block's
// sprite on a shape that is not the parent block's shape.
//
// Before this header the three item surfaces (the HUD icon, the dropped
// ItemEntity, the held item) shared ONE question, `world::rendersAsCubeItem`,
// with a yes/no answer: cube or flat sprite. There was no third answer, so every
// shaped block fell into "cube". This is that third answer — a list of boxes with
// the model json's own per-face UV rects — and it stays a single point for the
// same reason `rendersAsCubeItem` is one: the three surfaces disagreeing is the
// defect RN-10f closed and this one must not reopen.
//
// The boxes are transcribed from the vanilla models the `items/` entries name:
//
//   stairs           block/stairs            2 boxes  (the item IS the world model)
//   wall             block/wall_inventory    2 boxes  (a DEDICATED item model:
//                                                     the world wall has no such
//                                                     "one post + one full bar")
//   button           block/button_inventory  1 box    (dedicated, likewise)
//   pressure plate   block/pressure_plate_up 1 box
//   fence gate       block/template_fence_gate 8 boxes (the closed world model)
//   slab             block/slab              1 box
//   full cube        block/cube              1 box    (the three CubeUvModel
//                                                     variants, unchanged)
//
// A block whose kind is None keeps the flat item sprite: vanilla's door item is
// `item/oak_door`, a 2D sprite, and the diodes' are `item/repeater` and
// `item/comparator`.

#include "world/Block.hpp"
#include "world/CubeUv.hpp"

#include <array>
#include <cstdint>
#include <glm/vec3.hpp>
#include <span>

namespace mc::world {

// Which of a block item's five layers a face samples. The model json says this
// with a `#texture` reference (`#top`, `#side`, `#bottom`); those three are all
// the roster's item models use, and `front`/`back` exist for the DirectionalCube
// item whose north and south faces differ (RN-8c-D).
enum class ItemLayerSlot : std::uint8_t { Top, Bottom, Side, Front, Back };

struct ItemModelFace final {
    bool present = false;
    bake::FaceUv uv{};
    std::uint8_t quadrant = 0;
    ItemLayerSlot slot = ItemLayerSlot::Side;
};

// One box of an item model, in 0..16 model units, with its six declared faces.
// The direct analogue of a model json `elements[]` entry — deliberately the same
// shape as `bake::ModelElement`, minus the world-only fields (cullface, element
// rotation, shade) that an item render has no use for.
struct ItemModelBox final {
    glm::vec3 from16{0.0F, 0.0F, 0.0F};
    glm::vec3 to16{16.0F, 16.0F, 16.0F};
    std::array<ItemModelFace, bake::kFacingCount> face{}; // by bake::Facing
};

// The vanilla `display.gui` yaw a model is drawn at in the inventory, as
// quarter-turns away from the 225 degrees `block/block` gives everything by
// default. This is NOT cosmetic: `stairs.json` overrides gui to
// `rotation [30, 135, 0]`, and at 225 a stair shows its plain back — a 16x16
// square of the side sprite, i.e. exactly the "it looks like a plank" the field
// report describes. The turn is baked into the icon box table below rather than
// sent to the shader, because the icon's projection is fixed.
// The value is the number of quarter turns about +Y, in `turnUnitCorner`'s sense.
// Which count a given vanilla gui yaw corresponds to is not derived here: the
// icon's projection is hand-rolled (hud.vert draws three faces of a box in an
// orthographic isometric, it does not build vanilla's transform), so the mapping
// was established by rendering our icon beside the vanilla item model —
// `python3 tools/block_uv_preview.py oak_stairs --icon --pack <pack>` — and
// reading which way the step faced. A stair at the wrong count shows its plain
// back, which is the "it looks like a plank" pose the field report describes, so
// this is not a detail that can be left to reasoning.
enum class ItemIconTurn : std::uint8_t {
    None = 0,          // block/block: gui rotation [30, 225, 0]
    Quarter = 1,
    Half = 2,          // template_fence_gate.json: [30, 45, 0]
    ThreeQuarter = 3,  // stairs.json / wall_inventory.json: [30, 135, 0]
};

enum class ItemModelKind : std::uint8_t {
    None, // a flat item sprite, not a model
    Cube,
    Slab,
    Stairs,
    Wall,
    FenceGate,
    PressurePlate,
    Button,
    TrapDoor,
    Count,
};

namespace detail {

[[nodiscard]] constexpr bake::FaceUv itemRect(float minU, float minV, float maxU, float maxV) {
    return bake::FaceUv{minU, minV, maxU, maxV, /*absent=*/false};
}

constexpr void putItemFace(ItemModelBox& box, bake::Facing facing, ItemLayerSlot slot,
                           const bake::FaceUv& uv, std::uint8_t quadrant = 0) {
    ItemModelFace& f = box.face[static_cast<std::size_t>(facing)];
    f.present = true;
    f.uv = uv;
    f.quadrant = quadrant;
    f.slot = slot;
}

// A box whose six faces all take the whole sprite unrotated — the plain cube, and
// the shape a `CubeUvModel` variant then edits.
[[nodiscard]] constexpr ItemModelBox wholeCube(CubeUvModel model) {
    ItemModelBox box;
    box.from16 = kCellFrom16;
    box.to16 = kCellTo16;
    const CubeUvModelDesc desc = cubeUvModelDesc(model);
    for (std::uint8_t f = 0; f < bake::kFacingCount; ++f) {
        const auto facing = static_cast<bake::Facing>(f);
        const ItemLayerSlot slot = facing == bake::Facing::Up      ? ItemLayerSlot::Top
                                   : facing == bake::Facing::Down  ? ItemLayerSlot::Bottom
                                   : facing == bake::Facing::North ? ItemLayerSlot::Front
                                   : facing == bake::Facing::South ? ItemLayerSlot::Back
                                                                   : ItemLayerSlot::Side;
        putItemFace(box, facing, slot, desc.rect[f], desc.quadrant[f]);
    }
    return box;
}

// block/slab.json and the lower box of block/stairs.json are the same element:
// (0,0,0)-(16,8,16), #bottom below, #top above, and the side sprite's LOWER half
// on the four sides. That lower-half rect is what the old shader spelled as a
// hard-coded `uv.y = 0.5 + uv.y * 0.5` slab special case.
[[nodiscard]] constexpr ItemModelBox slabBox() {
    ItemModelBox box;
    box.from16 = {0.0F, 0.0F, 0.0F};
    box.to16 = {16.0F, 8.0F, 16.0F};
    putItemFace(box, bake::Facing::Down, ItemLayerSlot::Bottom, itemRect(0, 0, 16, 16));
    putItemFace(box, bake::Facing::Up, ItemLayerSlot::Top, itemRect(0, 0, 16, 16));
    for (const bake::Facing side : {bake::Facing::North, bake::Facing::South,
                                    bake::Facing::West, bake::Facing::East}) {
        putItemFace(box, side, ItemLayerSlot::Side, itemRect(0, 8, 16, 16));
    }
    return box;
}

// block/stairs.json's upper step: (8,8,0)-(16,16,16), and NO down face — it sits
// on the lower box. Its four side rects are each a different eighth of the
// sprite, which is the whole reason a stair icon is recognisable at 16 pixels.
[[nodiscard]] constexpr ItemModelBox stairStepBox() {
    ItemModelBox box;
    box.from16 = {8.0F, 8.0F, 0.0F};
    box.to16 = {16.0F, 16.0F, 16.0F};
    putItemFace(box, bake::Facing::Up, ItemLayerSlot::Top, itemRect(8, 0, 16, 16));
    putItemFace(box, bake::Facing::North, ItemLayerSlot::Side, itemRect(0, 0, 8, 8));
    putItemFace(box, bake::Facing::South, ItemLayerSlot::Side, itemRect(8, 0, 16, 8));
    putItemFace(box, bake::Facing::West, ItemLayerSlot::Side, itemRect(0, 0, 16, 8));
    putItemFace(box, bake::Facing::East, ItemLayerSlot::Side, itemRect(0, 0, 16, 8));
    return box;
}

// block/wall_inventory.json — a model that exists ONLY for the item: a full-height
// centre post plus one straight arm through it. No wall blockstate ever produces
// this combination, which is why the item cannot simply reuse a world variant.
[[nodiscard]] constexpr ItemModelBox wallPostBox() {
    ItemModelBox box;
    box.from16 = {4.0F, 0.0F, 4.0F};
    box.to16 = {12.0F, 16.0F, 12.0F};
    putItemFace(box, bake::Facing::Down, ItemLayerSlot::Side, itemRect(4, 4, 12, 12));
    putItemFace(box, bake::Facing::Up, ItemLayerSlot::Side, itemRect(4, 4, 12, 12));
    for (const bake::Facing side : {bake::Facing::North, bake::Facing::South,
                                    bake::Facing::West, bake::Facing::East}) {
        putItemFace(box, side, ItemLayerSlot::Side, itemRect(4, 0, 12, 16));
    }
    return box;
}

[[nodiscard]] constexpr ItemModelBox wallArmBox() {
    ItemModelBox box;
    box.from16 = {5.0F, 0.0F, 0.0F};
    box.to16 = {11.0F, 13.0F, 16.0F};
    putItemFace(box, bake::Facing::Down, ItemLayerSlot::Side, itemRect(5, 0, 11, 16));
    putItemFace(box, bake::Facing::Up, ItemLayerSlot::Side, itemRect(5, 0, 11, 16));
    putItemFace(box, bake::Facing::North, ItemLayerSlot::Side, itemRect(5, 3, 11, 16));
    putItemFace(box, bake::Facing::South, ItemLayerSlot::Side, itemRect(5, 3, 11, 16));
    putItemFace(box, bake::Facing::West, ItemLayerSlot::Side, itemRect(0, 3, 16, 16));
    putItemFace(box, bake::Facing::East, ItemLayerSlot::Side, itemRect(0, 3, 16, 16));
    return box;
}

// block/pressure_plate_up.json: the unpowered world model, which is what the
// item points at.
[[nodiscard]] constexpr ItemModelBox pressurePlateBox() {
    ItemModelBox box;
    box.from16 = {1.0F, 0.0F, 1.0F};
    box.to16 = {15.0F, 1.0F, 15.0F};
    putItemFace(box, bake::Facing::Down, ItemLayerSlot::Side, itemRect(1, 1, 15, 15));
    putItemFace(box, bake::Facing::Up, ItemLayerSlot::Side, itemRect(1, 1, 15, 15));
    for (const bake::Facing side : {bake::Facing::North, bake::Facing::South,
                                    bake::Facing::West, bake::Facing::East}) {
        putItemFace(box, side, ItemLayerSlot::Side, itemRect(1, 15, 15, 16));
    }
    return box;
}

// block/button_inventory.json — the other item-only model. Note the rects: the up
// face's V runs backwards (`[5,10,11,6]`) and the four sides sample the sprite's
// BOTTOM four rows rather than the box's own projection, because that is where a
// button standing on the ground takes them from. Both are vanilla's, and both are
// invisible on stone or planks — which is exactly why they are transcribed rather
// than derived: the day a button gets a non-uniform sprite, a derived rect would
// be wrong and nobody would know why.
[[nodiscard]] constexpr ItemModelBox buttonBox() {
    ItemModelBox box;
    box.from16 = {5.0F, 6.0F, 6.0F};
    box.to16 = {11.0F, 10.0F, 10.0F};
    putItemFace(box, bake::Facing::Down, ItemLayerSlot::Side, itemRect(5, 6, 11, 10));
    putItemFace(box, bake::Facing::Up, ItemLayerSlot::Side, itemRect(5, 10, 11, 6));
    putItemFace(box, bake::Facing::North, ItemLayerSlot::Side, itemRect(5, 12, 11, 16));
    putItemFace(box, bake::Facing::South, ItemLayerSlot::Side, itemRect(5, 12, 11, 16));
    putItemFace(box, bake::Facing::West, ItemLayerSlot::Side, itemRect(6, 12, 10, 16));
    putItemFace(box, bake::Facing::East, ItemLayerSlot::Side, itemRect(6, 12, 10, 16));
    return box;
}

// block/template_trapdoor_bottom.json, which `items/oak_trapdoor.json` names —
// the CLOSED bottom-half world model, a 3px slab.
//
// RN-14 recorded this as an open question and RN-15 settles it: vanilla's
// trapdoor ITEM is a 3D thin slab, not a flat sprite. Only the door is a sprite
// (`items/oak_door.json` -> `item/oak_door`). The RN-10 mac checklist's "doors
// and trapdoors are still flat sprites, that is vanilla's behaviour" was right
// about the door and wrong about the trapdoor.
//
// Every face takes `#texture`, the block's single declared sprite, so all six
// route to Side; a trapdoor declares the same name in all three texture slots
// anyway. The four side rects run V backwards (`[0,16,16,13]`) — vanilla's own,
// the same audit-R6 rect the world model carries.
[[nodiscard]] constexpr ItemModelBox trapdoorBottomBox() {
    ItemModelBox box;
    box.from16 = {0.0F, 0.0F, 0.0F};
    box.to16 = {16.0F, 3.0F, 16.0F};
    putItemFace(box, bake::Facing::Down, ItemLayerSlot::Side, itemRect(0, 0, 16, 16));
    putItemFace(box, bake::Facing::Up, ItemLayerSlot::Side, itemRect(0, 0, 16, 16));
    for (const bake::Facing side : {bake::Facing::North, bake::Facing::South,
                                    bake::Facing::West, bake::Facing::East}) {
        putItemFace(box, side, ItemLayerSlot::Side, itemRect(0, 16, 16, 13));
    }
    return box;
}

// The closed, not-in-wall fence gate, box for box from template_fence_gate.json.
// It is the same eight elements `bake::fenceGateElements(false, false)` builds for
// the world mesh; `block_item_model_test` asserts the two agree element for
// element rather than trusting this copy, since that function returns a runtime
// vector and this table has to be a compile-time one.
[[nodiscard]] constexpr ItemModelBox fenceGatePostBox(bool right) {
    ItemModelBox box;
    box.from16 = {right ? 14.0F : 0.0F, 5.0F, 7.0F};
    box.to16 = {right ? 16.0F : 2.0F, 16.0F, 9.0F};
    const float u0 = right ? 14.0F : 0.0F;
    const float u1 = right ? 16.0F : 2.0F;
    putItemFace(box, bake::Facing::Down, ItemLayerSlot::Side, itemRect(u0, 7, u1, 9));
    putItemFace(box, bake::Facing::Up, ItemLayerSlot::Side, itemRect(u0, 7, u1, 9));
    putItemFace(box, bake::Facing::North, ItemLayerSlot::Side, itemRect(u0, 0, u1, 11));
    putItemFace(box, bake::Facing::South, ItemLayerSlot::Side, itemRect(u0, 0, u1, 11));
    putItemFace(box, bake::Facing::West, ItemLayerSlot::Side, itemRect(7, 0, 9, 11));
    putItemFace(box, bake::Facing::East, ItemLayerSlot::Side, itemRect(7, 0, 9, 11));
    return box;
}

[[nodiscard]] constexpr ItemModelBox fenceGateInnerPostBox(float x0) {
    ItemModelBox box;
    box.from16 = {x0, 6.0F, 7.0F};
    box.to16 = {x0 + 2.0F, 15.0F, 9.0F};
    putItemFace(box, bake::Facing::Down, ItemLayerSlot::Side, itemRect(x0, 7, x0 + 2, 9));
    putItemFace(box, bake::Facing::Up, ItemLayerSlot::Side, itemRect(x0, 7, x0 + 2, 9));
    putItemFace(box, bake::Facing::North, ItemLayerSlot::Side, itemRect(x0, 1, x0 + 2, 10));
    putItemFace(box, bake::Facing::South, ItemLayerSlot::Side, itemRect(x0, 1, x0 + 2, 10));
    putItemFace(box, bake::Facing::West, ItemLayerSlot::Side, itemRect(7, 1, 9, 10));
    putItemFace(box, bake::Facing::East, ItemLayerSlot::Side, itemRect(7, 1, 9, 10));
    return box;
}

// The four horizontal bars: down/up/north/south only. The two faces vanilla omits
// are buried inside the neighbouring post (audit R7).
[[nodiscard]] constexpr ItemModelBox fenceGateBarBox(float x0, float y0, float x1, float y1,
                                                     const bake::FaceUv& cap,
                                                     const bake::FaceUv& side) {
    ItemModelBox box;
    box.from16 = {x0, y0, 7.0F};
    box.to16 = {x1, y1, 9.0F};
    putItemFace(box, bake::Facing::Down, ItemLayerSlot::Side, cap);
    putItemFace(box, bake::Facing::Up, ItemLayerSlot::Side, cap);
    putItemFace(box, bake::Facing::North, ItemLayerSlot::Side, side);
    putItemFace(box, bake::Facing::South, ItemLayerSlot::Side, side);
    return box;
}

} // namespace detail

// Every item-model box in the roster, flat, in one array. The first three entries
// are the plain cube's three `CubeUvModel` variants and MUST stay first and in
// that order: `cubeItemUvModel(block)` already returns 0/1/2 and both item vertex
// shaders index their UV table with it, so keeping the cubes at 0..2 is what makes
// this an extension of that table rather than a replacement for it.
inline constexpr std::array<ItemModelBox, 19> kItemModelBoxes{{
    detail::wholeCube(CubeUvModel::Default),        // 0
    detail::wholeCube(CubeUvModel::PistonTemplate), // 1
    detail::wholeCube(CubeUvModel::Observer),       // 2
    detail::slabBox(),                              // 3
    detail::slabBox(),                              // 4  stairs, lower box
    detail::stairStepBox(),                         // 5  stairs, upper step
    detail::wallPostBox(),                          // 6
    detail::wallArmBox(),                           // 7
    detail::pressurePlateBox(),                     // 8
    detail::buttonBox(),                            // 9
    detail::fenceGatePostBox(false),                // 10
    detail::fenceGatePostBox(true),                 // 11
    detail::fenceGateInnerPostBox(6.0F),            // 12
    detail::fenceGateInnerPostBox(8.0F),            // 13
    detail::fenceGateBarBox(2, 6, 6, 9, detail::itemRect(2, 7, 6, 9),
                            detail::itemRect(2, 7, 6, 10)),   // 14
    detail::fenceGateBarBox(2, 12, 6, 15, detail::itemRect(2, 7, 6, 9),
                            detail::itemRect(2, 1, 6, 4)),    // 15
    detail::fenceGateBarBox(10, 6, 14, 9, detail::itemRect(10, 7, 14, 9),
                            detail::itemRect(10, 7, 14, 10)), // 16
    detail::fenceGateBarBox(10, 12, 14, 15, detail::itemRect(10, 7, 14, 9),
                            detail::itemRect(10, 1, 14, 4)),  // 17
    detail::trapdoorBottomBox(),                              // 18
}};

// Where each kind's boxes live in the flat array, and how the inventory turns it.
struct ItemModelRange final {
    std::uint8_t first = 0;
    std::uint8_t count = 0;
    ItemIconTurn turn = ItemIconTurn::None;
};

inline constexpr std::array<ItemModelRange, static_cast<std::size_t>(ItemModelKind::Count)>
    kItemModelRanges{{
        {0, 0, ItemIconTurn::None},           // None
        {0, 1, ItemIconTurn::None},           // Cube (first is the block's CubeUvModel)
        {3, 1, ItemIconTurn::None},           // Slab
        {4, 2, ItemIconTurn::ThreeQuarter},   // Stairs
        {6, 2, ItemIconTurn::ThreeQuarter},   // Wall
        {10, 8, ItemIconTurn::Half},          // FenceGate
        {8, 1, ItemIconTurn::None},           // PressurePlate
        {9, 1, ItemIconTurn::None},           // Button
        {18, 1, ItemIconTurn::None},          // TrapDoor
    }};

// The single point. Every item surface asks this and nothing else: `None` means
// the flat sprite, anything else names a box list.
//
// This replaces `rendersAsCubeItem`'s yes/no. That predicate answered "cube" for
// every shaped block that was not a door or trapdoor, which is how a stair, a
// wall, a fence gate, a button and a pressure plate all ended up as full cubes
// wearing their parent block's sprite.
[[nodiscard]] constexpr ItemModelKind itemModelKindOf(Block block) {
    const auto model = blockDefinition(block).model;
    switch (model) {
    case BlockModel::Cube:
    case BlockModel::DirectionalCube:
    case BlockModel::Chest:
        return ItemModelKind::Cube;
    case BlockModel::Slab:
        return ItemModelKind::Slab;
    case BlockModel::Stairs:
        return ItemModelKind::Stairs;
    case BlockModel::Wall:
        return ItemModelKind::Wall;
    case BlockModel::FenceGate:
        return ItemModelKind::FenceGate;
    case BlockModel::PressurePlate:
        return ItemModelKind::PressurePlate;
    case BlockModel::Button:
        return ItemModelKind::Button;
    // RN-15: a trapdoor item is `block/<name>_bottom`, a 3D slab — NOT a flat
    // sprite. The door beside it really is one (`items/oak_door.json` names
    // `item/oak_door`), which is why the two were mistaken for a pair.
    case BlockModel::TrapDoor:
        return ItemModelKind::TrapDoor;
    // A door item is a flat sprite in vanilla's `items/` entry
    // (`item/oak_door`); so are the diodes (`item/repeater`), the lever, the
    // torch, the crops and the wire, all of which vanilla draws from an
    // `item/*.png` and none of which has a useful 3D silhouette at 16 pixels.
    case BlockModel::Door:
    case BlockModel::Cross:
    case BlockModel::Crop:
    case BlockModel::Torch:
    case BlockModel::ElementModel:
    case BlockModel::RedstoneWire:
    case BlockModel::Fire:
        return ItemModelKind::None;
    }
    return ItemModelKind::None;
}

// The block's boxes, as indices into kItemModelBoxes. A cube's single index is
// its CubeUvModel, which is why this is not simply `kItemModelRanges[kind]`.
[[nodiscard]] inline ItemModelRange itemModelRange(Block block) {
    const auto kind = itemModelKindOf(block);
    ItemModelRange range = kItemModelRanges[static_cast<std::size_t>(kind)];
    if (kind == ItemModelKind::Cube) {
        range.first = static_cast<std::uint8_t>(cubeItemUvModel(block));
    }
    return range;
}

// Whether this block's item is drawn from a model at all (the old
// `rendersAsCubeItem`'s question, now "model or sprite" rather than "cube or
// sprite").
[[nodiscard]] constexpr bool rendersAsModelItem(Block block) {
    return itemModelKindOf(block) != ItemModelKind::None;
}

// Which of the five item layers a face of an item box shows.
[[nodiscard]] inline float itemFaceLayer(const CubeItemLayers& layers, ItemLayerSlot slot) {
    switch (slot) {
    case ItemLayerSlot::Top: return layers.top;
    case ItemLayerSlot::Bottom: return layers.bottom;
    case ItemLayerSlot::Front: return layers.front;
    case ItemLayerSlot::Back: return layers.back;
    case ItemLayerSlot::Side: break;
    }
    return layers.side;
}

// --- The inventory icon's projection -----------------------------------------
//
// The icon is an orthographic isometric view, and until RN-14 it was written down
// as eighteen literal SCREEN positions in hud.vert — a unit cube, pre-projected,
// with the slab's half height patched in by nudging six of those y values. A box
// that is not the unit cube cannot be expressed that way at all, which is the
// mechanical reason every shaped block's icon was a full cube.
//
// The eighteen positions are exactly this affine map applied to the three visible
// faces' corners, so naming the map (and asserting it reproduces them) is what
// turns "a table of screen positions" into "a projection you can put any box
// through". Derived, not fitted: see the static_asserts below.
[[nodiscard]] constexpr glm::vec2 iconProject(const glm::vec3& p) {
    return {0.5F + 0.44F * (p.z - p.x),
            0.46F - 0.21F * (p.x + p.z) + 0.48F * (1.0F - p.y)};
}

// Depth along the view axis, normalised to [0,1]. The view direction is
// (+1,-1,+1)/sqrt(3) — the corner (0,1,0) is nearest the eye, (1,0,1) farthest —
// so this is that dot product remapped from [-1,2] onto [0,1]. A multi-box item
// model needs it: a wall's centre post and its arm interpenetrate, and no
// back-to-front ordering of whole boxes composites them correctly.
[[nodiscard]] constexpr float iconDepth(const glm::vec3& p) {
    return (p.x - p.y + p.z + 1.0F) / 3.0F;
}

// The three faces the icon shows, and the corner order the shader emits them in
// (two triangles as v0,v1,v2, v0,v2,v3). Positions are unit-cube corners.
inline constexpr std::array<Face, 3> kIconFaces{Face::PositiveY, Face::NegativeZ,
                                                Face::NegativeX};

inline constexpr std::array<glm::vec3, 18> kIconCubeCorners{{
    // top diamond = the up face
    {1, 1, 1}, {0, 1, 1}, {0, 1, 0}, {1, 1, 1}, {0, 1, 0}, {1, 1, 0},
    // left parallelogram = the model's north face
    {1, 1, 0}, {0, 1, 0}, {0, 0, 0}, {1, 1, 0}, {0, 0, 0}, {1, 0, 0},
    // right parallelogram = its west face
    {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0},
}};

// The literals hud.vert used to carry, reproduced by the map above. If this ever
// fails the projection and the shader have stopped describing the same cube.
static_assert(iconProject(kIconCubeCorners[0]).x == 0.5F);
static_assert(iconProject(kIconCubeCorners[1]).x == 0.94F);
static_assert(iconProject(kIconCubeCorners[2]).x == 0.5F);
static_assert(iconProject(kIconCubeCorners[8]).y == 0.94F);
static_assert(iconProject(kIconCubeCorners[11]).y == 0.73F);

// --- The inventory turn -------------------------------------------------------
//
// A quarter turn about +Y, on a unit-cube corner and about the cube's centre. The
// sense is the one that carries a stair's step onto a face the icon can see:
// vanilla's stairs.json asks for gui yaw 135 where block/block gives 225, and
// under a fixed camera that is the model turned, not the camera.
[[nodiscard]] constexpr glm::vec3 turnUnitCorner(const glm::vec3& p, ItemIconTurn turn) {
    glm::vec3 out = p;
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(turn); ++i) {
        const glm::vec3 centred{out.x - 0.5F, out.y - 0.5F, out.z - 0.5F};
        out = {centred.z + 0.5F, centred.y + 0.5F, -centred.x + 0.5F};
    }
    return out;
}

[[nodiscard]] constexpr bake::Facing turnFacing(bake::Facing facing, ItemIconTurn turn) {
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(turn); ++i) {
        switch (facing) {
        case bake::Facing::North: facing = bake::Facing::West; break;
        case bake::Facing::West: facing = bake::Facing::South; break;
        case bake::Facing::South: facing = bake::Facing::East; break;
        case bake::Facing::East: facing = bake::Facing::North; break;
        default: break;
        }
    }
    return facing;
}
static_assert(turnFacing(bake::Facing::North, ItemIconTurn::Quarter) == bake::Facing::West);
static_assert(turnFacing(bake::Facing::North, ItemIconTurn::ThreeQuarter) == bake::Facing::East);
static_assert(turnFacing(bake::Facing::Up, ItemIconTurn::Half) == bake::Facing::Up);

// The UV a given CORNER POSITION of a box face samples, rather than a given
// corner INDEX. Keyed by position because the icon turn permutes the corners: the
// vertex standing at a rotated position took its texel from wherever that corner
// was before the turn, and matching by position is what says so without a
// hand-written permutation table.
[[nodiscard]] constexpr glm::vec2 faceUvAtCorner(const ItemModelFace& face, bake::Facing facing,
                                                 const glm::vec3& unitCorner) {
    const auto& info = bake::kFaceInfo[static_cast<std::size_t>(facing)];
    constexpr glm::vec3 unitFrom{0.0F, 0.0F, 0.0F};
    constexpr glm::vec3 unitTo{1.0F, 1.0F, 1.0F};
    for (int i = 0; i < 4; ++i) {
        const glm::vec3 got = bake::faceVertex(info[static_cast<std::size_t>(i)], unitFrom, unitTo);
        if (got.x == unitCorner.x && got.y == unitCorner.y && got.z == unitCorner.z) {
            const int index = bake::rotateVertexIndex(i, face.quadrant);
            return {bake::vertexU(face.uv, index) / 16.0F, bake::vertexV(face.uv, index) / 16.0F};
        }
    }
    return {0.0F, 0.0F};
}

// A box as the ICON draws it: turned into the inventory pose, with the three
// visible faces resolved back to the model face each one came from.
struct IconBox final {
    glm::vec3 from{0.0F, 0.0F, 0.0F}; // 0..1, after the turn
    glm::vec3 to{1.0F, 1.0F, 1.0F};
    std::array<bool, 3> present{};       // by kIconFaces
    std::array<bake::FaceUv, 3> uv{};    // the source face's declared rect
    std::array<ItemLayerSlot, 3> slot{}; // and which layer it samples
    // The four corner UVs the shader interpolates, in the order its quad emits
    // them (v0,v1,v2,v3). Resolved here rather than in GLSL because the turn
    // permutes which model corner each icon vertex stands on, and a shader that
    // had to know that would need JE's FaceInfo table and the turn as well.
    std::array<std::array<glm::vec2, 4>, 3> uvCorner{};
};

// How many quarter turns undo `turn` — the icon vertex stands at a turned
// position and took its texel from wherever that corner was before.
[[nodiscard]] constexpr std::uint8_t inverseTurnSteps(ItemIconTurn turn) {
    return static_cast<std::uint8_t>((4U - static_cast<std::uint8_t>(turn)) % 4U);
}

[[nodiscard]] constexpr glm::vec3 unturnUnitCorner(const glm::vec3& p, ItemIconTurn turn) {
    glm::vec3 out = p;
    for (std::uint8_t i = 0; i < inverseTurnSteps(turn); ++i) {
        out = turnUnitCorner(out, ItemIconTurn::Quarter);
    }
    return out;
}

[[nodiscard]] constexpr IconBox iconBoxOf(const ItemModelBox& box, ItemIconTurn turn) {
    IconBox out;
    const glm::vec3 a = turnUnitCorner(box.from16 / 16.0F, turn);
    const glm::vec3 b = turnUnitCorner(box.to16 / 16.0F, turn);
    out.from = {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
    out.to = {a.x < b.x ? b.x : a.x, a.y < b.y ? b.y : a.y, a.z < b.z ? b.z : a.z};
    for (std::size_t i = 0; i < kIconFaces.size(); ++i) {
        const bake::Facing target = bakeFacingOf(kIconFaces[i]);
        for (std::uint8_t f = 0; f < bake::kFacingCount; ++f) {
            const auto source = static_cast<bake::Facing>(f);
            if (turnFacing(source, turn) != target) {
                continue;
            }
            const ItemModelFace& face = box.face[f];
            out.present[i] = face.present;
            out.uv[i] = face.uv;
            out.slot[i] = face.slot;
            // The face's four corners, in the shader's quad order: vertices
            // 0, 1, 2 and 5 of this face's six.
            constexpr std::array<std::size_t, 4> kQuadVertex{0U, 1U, 2U, 5U};
            for (std::size_t c = 0; c < 4; ++c) {
                const glm::vec3 turned = kIconCubeCorners[i * 6U + kQuadVertex[c]];
                out.uvCorner[i][c] = faceUvAtCorner(face, source, unturnUnitCorner(turned, turn));
            }
        }
    }
    return out;
}

// Every box's icon form, baked. The icon draw path indexes this instead of
// calling `iconBoxOf`: it runs per box per face per frame for every visible
// slot, and it searches four corner positions per corner to resolve a UV, which
// is a table lookup's worth of answer for a loop's worth of work.
inline constexpr std::array<IconBox, kItemModelBoxes.size()> kItemIconBoxes = [] {
    std::array<IconBox, kItemModelBoxes.size()> table{};
    for (std::size_t b = 0; b < kItemModelBoxes.size(); ++b) {
        // Which kind owns this box decides the turn. The cubes and the slab are
        // shared by several kinds, but every kind that uses them turns None.
        ItemIconTurn turn = ItemIconTurn::None;
        for (std::size_t k = 0; k < kItemModelRanges.size(); ++k) {
            const ItemModelRange& range = kItemModelRanges[k];
            if (range.count != 0U && b >= range.first &&
                b < static_cast<std::size_t>(range.first) + range.count) {
                turn = range.turn;
            }
        }
        table[b] = iconBoxOf(kItemModelBoxes[b], turn);
    }
    return table;
}();

} // namespace mc::world

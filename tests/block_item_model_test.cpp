// RN-14: a block item is drawn from the block's own model, not from a cube
// wearing the block's texture.
//
// The field report was "the stairs are not registered — you cannot get them",
// which is not what happens: `Block::OakStairs` is in the creative catalogue
// under BuildingBlocks. What a player actually sees is a 16x16x16 cube wearing
// `oak_planks`, which is a plank. Same for the fence gate, the pressure plate and
// the button — every shaped block fell into `rendersAsCubeItem`'s "cube" answer
// because there was no third answer to give.
//
// Everything here is data: which boxes, which uv rects, which layer, which
// inventory turn. What the pixels look like is a Mac question and is not signed
// off here.

#include "world/Block.hpp"
#include "world/ElementModelBaker.hpp"
#include "gameplay/Inventory.hpp"
#include "world/ItemModel.hpp"

#include <array>
#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <vector>

namespace {

using namespace mc::world;

[[nodiscard]] bool near(float a, float b) { return std::fabs(a - b) < 1.0e-4F; }

[[nodiscard]] bool sameVec(const glm::vec3& a, const glm::vec3& b) {
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

[[nodiscard]] bool sameRect(const bake::FaceUv& a, const bake::FaceUv& b) {
    return near(a.minU, b.minU) && near(a.minV, b.minV) && near(a.maxU, b.maxU) &&
           near(a.maxV, b.maxV);
}

[[nodiscard]] const ItemModelBox& boxOf(Block block, std::size_t index) {
    const auto range = itemModelRange(block);
    assert(index < range.count);
    return kItemModelBoxes[static_cast<std::size_t>(range.first) + index];
}

// Whether this block's item is a single full 16x16x16 box — the shape every
// shaped block's icon had before RN-14, and the one thing that must NOT be true
// of a stair, a wall, a fence gate, a plate or a button.
[[nodiscard]] bool isOneFullCube(Block block) {
    const auto range = itemModelRange(block);
    if (range.count != 1U) {
        return false;
    }
    const auto& box = kItemModelBoxes[range.first];
    return sameVec(box.from16, {0.0F, 0.0F, 0.0F}) && sameVec(box.to16, {16.0F, 16.0F, 16.0F});
}

[[nodiscard]] std::size_t countKind(ItemModelKind kind) {
    std::size_t total = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(Block::Count); ++i) {
        if (itemModelKindOf(static_cast<Block>(i)) == kind) {
            ++total;
        }
    }
    return total;
}

} // namespace

int main() {
    // --- The defect, stated as an assertion. Before RN-14 every one of these was
    //     a single full cube wearing the parent block's sprite, which is why a
    //     stair and a plank were indistinguishable in the inventory. ---
    for (const Block block : {Block::OakStairs, Block::CobblestoneWall, Block::OakFenceGate,
                              Block::StonePressurePlate, Block::StoneButton}) {
        assert(!isOneFullCube(block) &&
               "a shaped block's item must not be a full cube wearing its parent's texture");
        assert(rendersAsModelItem(block));
    }
    // The control: a plain block IS one full cube, so the assertion above is
    // about the shape and not about the predicate always answering no.
    assert(isOneFullCube(Block::OakPlanks));
    assert(isOneFullCube(Block::Stone));

    // --- block/stairs.json, box for box. The lower box is block/slab's element;
    //     the upper step has NO down face (it sits on the lower one) and four
    //     different side rects, which is what makes a stair icon readable. ---
    {
        assert(itemModelRange(Block::OakStairs).count == 2U);
        const ItemModelBox& lower = boxOf(Block::OakStairs, 0);
        assert(sameVec(lower.from16, {0.0F, 0.0F, 0.0F}));
        assert(sameVec(lower.to16, {16.0F, 8.0F, 16.0F}));
        assert(sameRect(lower.face[static_cast<std::size_t>(bake::Facing::North)].uv,
                        {0.0F, 8.0F, 16.0F, 16.0F, false}));
        assert(lower.face[static_cast<std::size_t>(bake::Facing::Down)].slot ==
               ItemLayerSlot::Bottom);
        assert(lower.face[static_cast<std::size_t>(bake::Facing::Up)].slot == ItemLayerSlot::Top);

        const ItemModelBox& step = boxOf(Block::OakStairs, 1);
        assert(sameVec(step.from16, {8.0F, 8.0F, 0.0F}));
        assert(sameVec(step.to16, {16.0F, 16.0F, 16.0F}));
        assert(!step.face[static_cast<std::size_t>(bake::Facing::Down)].present &&
               "the step's down face is inside the lower box; drawing it z-fights");
        assert(sameRect(step.face[static_cast<std::size_t>(bake::Facing::North)].uv,
                        {0.0F, 0.0F, 8.0F, 8.0F, false}));
        assert(sameRect(step.face[static_cast<std::size_t>(bake::Facing::South)].uv,
                        {8.0F, 0.0F, 16.0F, 8.0F, false}));
    }

    // --- block/wall_inventory.json and block/button_inventory.json are models
    //     that exist ONLY for the item: no wall blockstate produces "one post
    //     plus one straight arm", and no button state is a nub floating at
    //     y 6..10. An item path that could only reuse world variants could not
    //     draw either. ---
    {
        assert(itemModelRange(Block::CobblestoneWall).count == 2U);
        const ItemModelBox& post = boxOf(Block::CobblestoneWall, 0);
        assert(sameVec(post.from16, {4.0F, 0.0F, 4.0F}));
        assert(sameVec(post.to16, {12.0F, 16.0F, 12.0F}));
        const ItemModelBox& arm = boxOf(Block::CobblestoneWall, 1);
        assert(sameVec(arm.from16, {5.0F, 0.0F, 0.0F}));
        assert(sameVec(arm.to16, {11.0F, 13.0F, 16.0F}));
        assert(sameRect(arm.face[static_cast<std::size_t>(bake::Facing::West)].uv,
                        {0.0F, 3.0F, 16.0F, 16.0F, false}));

        assert(itemModelRange(Block::StoneButton).count == 1U);
        const ItemModelBox& button = boxOf(Block::StoneButton, 0);
        assert(sameVec(button.from16, {5.0F, 6.0F, 6.0F}));
        assert(sameVec(button.to16, {11.0F, 10.0F, 10.0F}));
        // Two vanilla rects that are NOT the box's own projection: the up face's V
        // runs backwards and the sides take the sprite's bottom four rows. Both
        // are invisible on stone, which is exactly why they are transcribed.
        assert(sameRect(button.face[static_cast<std::size_t>(bake::Facing::Up)].uv,
                        {5.0F, 10.0F, 11.0F, 6.0F, false}));
        assert(sameRect(button.face[static_cast<std::size_t>(bake::Facing::North)].uv,
                        {5.0F, 12.0F, 11.0F, 16.0F, false}));

        assert(itemModelRange(Block::StonePressurePlate).count == 1U);
        const ItemModelBox& plate = boxOf(Block::StonePressurePlate, 0);
        assert(sameVec(plate.from16, {1.0F, 0.0F, 1.0F}));
        assert(sameVec(plate.to16, {15.0F, 1.0F, 15.0F}));
    }

    // --- The fence gate item is the CLOSED world model, so it must be the same
    //     eight elements the mesher draws rather than a second transcription that
    //     can drift from it. Asserted against `bake::fenceGateElements` rather
    //     than trusted, because that one returns a runtime vector and the item
    //     table has to be a compile-time one. ---
    {
        const auto world = bake::fenceGateElements(/*open=*/false, /*inWall=*/false);
        const auto range = itemModelRange(Block::OakFenceGate);
        assert(range.count == world.size());
        for (std::size_t i = 0; i < world.size(); ++i) {
            const ItemModelBox& item = kItemModelBoxes[range.first + i];
            assert(sameVec(item.from16, world[i].from16));
            assert(sameVec(item.to16, world[i].to16));
            for (std::size_t f = 0; f < bake::kFacingCount; ++f) {
                assert(item.face[f].present == world[i].faces[f].present);
                if (item.face[f].present) {
                    assert(sameRect(item.face[f].uv, world[i].faces[f].uv));
                }
            }
        }
    }

    // --- The whole roster, not five hand-picked blocks: every stair, wall, gate,
    //     plate and button resolves to its kind, so a newly registered species
    //     cannot be half-registered. The counts come from the roster itself. ---
    {
        std::size_t stairs = 0;
        std::size_t walls = 0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(Block::Count); ++i) {
            const auto block = static_cast<Block>(i);
            const auto model = blockDefinition(block).model;
            const auto kind = itemModelKindOf(block);
            switch (model) {
            case BlockModel::Stairs:
                assert(kind == ItemModelKind::Stairs);
                ++stairs;
                break;
            case BlockModel::Wall:
                assert(kind == ItemModelKind::Wall);
                ++walls;
                break;
            case BlockModel::FenceGate: assert(kind == ItemModelKind::FenceGate); break;
            case BlockModel::PressurePlate: assert(kind == ItemModelKind::PressurePlate); break;
            case BlockModel::Button: assert(kind == ItemModelKind::Button); break;
            case BlockModel::Door: assert(kind == ItemModelKind::None); break;
            case BlockModel::TrapDoor: assert(kind == ItemModelKind::TrapDoor); break;
            default: break;
            }
        }
        // The field report named "23 stairs, 13 walls"; the roster is the source.
        assert(stairs > 0 && walls > 0);
        assert(countKind(ItemModelKind::Stairs) == stairs);
        assert(countKind(ItemModelKind::Wall) == walls);
    }

    // --- The DOOR keeps the flat sprite: vanilla's items/oak_door.json names
    //     `item/oak_door`, a 2D drawing of a whole door. The TRAPDOOR does not —
    //     items/oak_trapdoor.json names `block/oak_trapdoor_bottom`, a 3D slab.
    //     RN-10f registered the two together as "thin leaves"; RN-14 found the
    //     trapdoor half of that wrong and RN-15 corrects it. ---
    for (const Block block : {Block::OakDoor, Block::IronDoor}) {
        assert(itemModelKindOf(block) == ItemModelKind::None);
        assert(!rendersAsModelItem(block));
    }
    for (const Block block : {Block::OakTrapdoor, Block::IronTrapdoor}) {
        assert(itemModelKindOf(block) == ItemModelKind::TrapDoor);
        assert(rendersAsModelItem(block));
        assert(itemModelRange(block).count == 1U);
        const ItemModelBox& leaf = boxOf(block, 0);
        assert(sameVec(leaf.from16, {0.0F, 0.0F, 0.0F}));
        assert(sameVec(leaf.to16, {16.0F, 3.0F, 16.0F}));
        // template_trapdoor_bottom.json's own side rect, V running backwards.
        assert(sameRect(leaf.face[static_cast<std::size_t>(bake::Facing::North)].uv,
                        {0.0F, 16.0F, 16.0F, 13.0F, false}));
    }
    // So do the diodes, the lever and the torch: vanilla draws each from an
    // `item/*.png`, not from its block model.
    for (const Block block : {Block::Repeater, Block::Comparator, Block::Lever,
                              Block::Torch, Block::RedstoneWire}) {
        assert(itemModelKindOf(block) == ItemModelKind::None);
    }

    // --- The inventory turn. `stairs.json` overrides gui to rotation
    //     [30, 135, 0] where block/block gives 225, and the difference is not
    //     decorative: at 225 the icon shows the stair's plain back — a full
    //     16x16 square of the side sprite, i.e. a plank. ---
    {
        assert(kItemModelRanges[static_cast<std::size_t>(ItemModelKind::Stairs)].turn ==
               ItemIconTurn::ThreeQuarter);
        assert(kItemModelRanges[static_cast<std::size_t>(ItemModelKind::Wall)].turn ==
               ItemIconTurn::ThreeQuarter);
        assert(kItemModelRanges[static_cast<std::size_t>(ItemModelKind::FenceGate)].turn ==
               ItemIconTurn::Half);
        assert(kItemModelRanges[static_cast<std::size_t>(ItemModelKind::Cube)].turn ==
               ItemIconTurn::None);

        // After the turn the step must sit at the FRONT of the icon (small z is
        // nearest the eye) and take only half the depth, so the icon shows its
        // profile. Untumed it sits at x 8..16, where the icon would see its
        // plain 16x16 back.
        const IconBox step = iconBoxOf(boxOf(Block::OakStairs, 1), ItemIconTurn::ThreeQuarter);
        assert(sameVec(step.from, {0.0F, 0.5F, 0.5F}));
        assert(sameVec(step.to, {1.0F, 1.0F, 1.0F}));
        // Its three visible faces come from the model's up, west and south faces
        // respectively — the turn permutes which model face lands where.
        assert(step.present[0] && step.present[1] && step.present[2]);
        assert(sameRect(step.uv[0], {8.0F, 0.0F, 16.0F, 16.0F, false}));   // up
        assert(sameRect(step.uv[1], {0.0F, 0.0F, 16.0F, 8.0F, false}));    // north <- west
        assert(sameRect(step.uv[2], {8.0F, 0.0F, 16.0F, 8.0F, false}));    // west <- south

        // A face the model does not declare is not drawn: the step's down face is
        // absent, and after the turn the lower box is what the icon sees below.
        const IconBox lower = iconBoxOf(boxOf(Block::OakStairs, 0), ItemIconTurn::ThreeQuarter);
        assert(sameVec(lower.from, {0.0F, 0.0F, 0.0F}));
        assert(sameVec(lower.to, {1.0F, 0.5F, 1.0F}));
    }

    // --- The baked icon table. The draw path indexes it instead of calling
    //     iconBoxOf per frame, so its turn derivation (which kind owns which box)
    //     has to agree with the ranges. A box that fell through would silently be
    //     drawn unturned — which for a stair is precisely the "it looks like a
    //     plank" pose. ---
    {
        for (std::size_t k = 1; k < kItemModelRanges.size(); ++k) {
            const ItemModelRange& range = kItemModelRanges[k];
            for (std::size_t i = 0; i < range.count; ++i) {
                const std::size_t box = static_cast<std::size_t>(range.first) + i;
                const IconBox expected = iconBoxOf(kItemModelBoxes[box], range.turn);
                const IconBox& baked = kItemIconBoxes[box];
                assert(sameVec(baked.from, expected.from));
                assert(sameVec(baked.to, expected.to));
                for (std::size_t f = 0; f < 3; ++f) {
                    assert(baked.present[f] == expected.present[f]);
                    assert(baked.slot[f] == expected.slot[f]);
                    for (std::size_t c = 0; c < 4; ++c) {
                        assert(near(baked.uvCorner[f][c].x, expected.uvCorner[f][c].x));
                        assert(near(baked.uvCorner[f][c].y, expected.uvCorner[f][c].y));
                    }
                }
            }
        }
        // The stair's boxes really are turned and the cube's really is not, so the
        // loop above is not comparing two copies of "unturned".
        assert(!sameVec(kItemIconBoxes[itemModelRange(Block::OakStairs).first + 1].from,
                        boxOf(Block::OakStairs, 1).from16 / 16.0F));
        assert(sameVec(kItemIconBoxes[0].from, {0.0F, 0.0F, 0.0F}));
        assert(sameVec(kItemIconBoxes[0].to, {1.0F, 1.0F, 1.0F}));
    }

    // --- Every icon vertex's UV must be one the model's rect actually contains,
    //     and the four corners of a face must be four DIFFERENT texels unless the
    //     rect is degenerate. A permutation bug in the turn shows up here as two
    //     corners collapsing onto one. ---
    {
        for (std::size_t b = 0; b < kItemModelBoxes.size(); ++b) {
            const IconBox& icon = kItemIconBoxes[b];
            for (std::size_t f = 0; f < 3; ++f) {
                if (!icon.present[f]) {
                    continue;
                }
                const bake::FaceUv& rect = icon.uv[f];
                const float lowU = std::min(rect.minU, rect.maxU) / 16.0F;
                const float highU = std::max(rect.minU, rect.maxU) / 16.0F;
                const float lowV = std::min(rect.minV, rect.maxV) / 16.0F;
                const float highV = std::max(rect.minV, rect.maxV) / 16.0F;
                int distinct = 0;
                for (std::size_t c = 0; c < 4; ++c) {
                    const glm::vec2 uv = icon.uvCorner[f][c];
                    assert(uv.x >= lowU - 1.0e-4F && uv.x <= highU + 1.0e-4F);
                    assert(uv.y >= lowV - 1.0e-4F && uv.y <= highV + 1.0e-4F);
                    bool seen = false;
                    for (std::size_t d = 0; d < c; ++d) {
                        seen = seen || (near(uv.x, icon.uvCorner[f][d].x) &&
                                        near(uv.y, icon.uvCorner[f][d].y));
                    }
                    distinct += seen ? 0 : 1;
                }
                assert(distinct == 4 && "a face's four corners must be four texels");
            }
        }
    }

    // --- The plain cube is untouched: it is still one box, still indexed by its
    //     CubeUvModel, so the piston and the observer keep the icon UVs RN-8c gave
    //     them. This is the regression guard for the 200-odd ordinary blocks. ---
    {
        assert(itemModelRange(Block::Stone).first ==
               static_cast<std::uint8_t>(CubeUvModel::Default));
        assert(itemModelRange(Block::Observer).first ==
               static_cast<std::uint8_t>(cubeItemUvModel(Block::Observer)));
        for (std::size_t m = 0; m < kCubeUvModelCount; ++m) {
            const auto& box = kItemModelBoxes[m];
            assert(sameVec(box.from16, kCellFrom16));
            assert(sameVec(box.to16, kCellTo16));
            for (std::size_t f = 0; f < bake::kFacingCount; ++f) {
                assert(box.face[f].present);
            }
        }
        // A slab is one half-height box, which is what the shader's hard-coded
        // `portion` hack used to express.
        assert(itemModelRange(Block::OakSlab).count == 1U);
        assert(sameVec(boxOf(Block::OakSlab, 0).to16, {16.0F, 8.0F, 16.0F}));
    }

    // --- audit R18: the two diodes have an item SPRITE of their own. vanilla's
    //     items/repeater.json names `item/repeater`, a side-on drawing of the whole
    //     component; the icon was showing `block/repeater`, its top plate, because
    //     a block stack's layer came from `textureLayers(block).top` and block
    //     items are not in the item icon atlas segment. The two files genuinely
    //     differ. ---
    {
        assert(hasItemSprite(Block::Repeater));
        assert(hasItemSprite(Block::Comparator));
        assert(std::string_view{blockDefinition(Block::Repeater).itemSprite} == "repeater");
        assert(std::string_view{blockDefinition(Block::Comparator).itemSprite} == "comparator");
        // A block item with its own sprite is still a FLAT sprite, not a model —
        // that is what "item/repeater" means.
        assert(itemModelKindOf(Block::Repeater) == ItemModelKind::None);

        // And the routing: a stack of them samples that layer, not the block's.
        setBlockTextureLayers(Block::Repeater, {101.0F, 102.0F, 103.0F});
        setBlockItemSpriteLayer(Block::Repeater, 555.0F);
        const mc::gameplay::ItemStack repeater{Block::Repeater, 1U, nullptr};
        assert(mc::gameplay::itemTextureLayer(repeater) == 555.0F);
        // A registration the atlas builder never filled in falls back to the block
        // texture rather than to whatever sits at atlas layer 0 — item sprites are
        // appended after every block texture, so a real one is never zero. Nothing
        // in ctest links the atlas builder (it needs a resource pack, which this
        // repository deliberately does not ship), so this is the guard that keeps a
        // half-wired registration from becoming a wrong sprite.
        setBlockItemSpriteLayer(Block::Repeater, 0.0F);
        assert(mc::gameplay::itemTextureLayer(repeater) == 101.0F);
        setBlockItemSpriteLayer(Block::Repeater, 555.0F);

        // The control: a block WITHOUT its own item sprite still samples its top
        // texture, so the assertion above is about the sprite and not about the
        // lookup always answering the same thing.
        assert(!hasItemSprite(Block::Stone));
        setBlockTextureLayers(Block::Stone, {201.0F, 202.0F, 203.0F});
        const mc::gameplay::ItemStack stone{Block::Stone, 1U, nullptr};
        assert(mc::gameplay::itemTextureLayer(stone) == 201.0F);
        // Nothing else in the roster claims one yet. Doors and sugar cane are the
        // same defect (items/oak_door.json names `item/oak_door`, a whole-door
        // drawing, and the icon shows `oak_door_top`), registered but not done —
        // see the RN-14 landing record.
        // RN-15 registered the rest of the same defect class: every door
        // (`items/<name>.json` -> `item/<name>`, a drawing of a whole door, where
        // the icon was showing the block's upper-half sprite) and sugar cane.
        // The two waxed copper doors name the UNWAXED sprite, because vanilla has
        // no waxed_*_door.png at all — a rule that has to be transcribed, since
        // deriving the sprite name from the block name would ask for a file the
        // pack does not have and silently render the missing-texture checker.
        assert(std::string_view{blockDefinition(Block::WaxedCopperDoor).itemSprite} ==
               "copper_door");
        assert(std::string_view{blockDefinition(Block::WaxedOxidizedCopperDoor).itemSprite} ==
               "oxidized_copper_door");
        assert(hasItemSprite(Block::SugarCane));
        std::size_t sprites = 0;
        std::size_t doorSprites = 0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(Block::Count); ++i) {
            const auto block = static_cast<Block>(i);
            sprites += hasItemSprite(block) ? 1U : 0U;
            if (blockDefinition(block).model == BlockModel::Door) {
                assert(hasItemSprite(block) && "every door's item is item/<name>.png");
                ++doorSprites;
            }
        }
        // Two diodes + sugar cane + every door in the roster.
        assert(doorSprites > 0U);
        assert(sprites == 3U + doorSprites);
    }

    // --- Layer routing: a face asks for one of the five item layers, and the
    //     three the new models use are top/bottom/side. The stair's step takes
    //     #top above and #side around, exactly as block/stairs.json says. ---
    {
        const ItemModelBox& step = boxOf(Block::OakStairs, 1);
        assert(step.face[static_cast<std::size_t>(bake::Facing::Up)].slot == ItemLayerSlot::Top);
        assert(step.face[static_cast<std::size_t>(bake::Facing::East)].slot == ItemLayerSlot::Side);
        const CubeItemLayers layers{1.0F, 2.0F, 3.0F, 4.0F, 5.0F};
        assert(itemFaceLayer(layers, ItemLayerSlot::Top) == 1.0F);
        assert(itemFaceLayer(layers, ItemLayerSlot::Bottom) == 2.0F);
        assert(itemFaceLayer(layers, ItemLayerSlot::Side) == 3.0F);
        assert(itemFaceLayer(layers, ItemLayerSlot::Front) == 4.0F);
        assert(itemFaceLayer(layers, ItemLayerSlot::Back) == 5.0F);
    }

    return 0;
}

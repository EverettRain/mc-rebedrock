// STRUCT-2 (engine): placement math, rotation, and the template placer, ending in
// the STORAGE->PLACEMENT->LOOT slice — a template placed into a chunk with its
// chest emitting a loot placement whose table then rolls into items.
//
// What is pinned: random_spread picks exactly one origin chunk per region and is
// seed-deterministic with continuous negative-coordinate regions; the rotation is
// a rigid transform (four 90° turns return to the start, coords stay in the
// rotated box, a horizontal facing turns N->E->S->W while a non-oriented block is
// untouched); the placer writes in-chunk cells, hands border cells to the
// TreeBorderBlock stream, and emits chest loot placements; and a placed chest's
// table rolls deterministically into items.

#include "core/Json.hpp"
#include "data/ChestLootFile.hpp"
#include "gameplay/ChestLootTable.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/Random.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/StructurePlacer.hpp"
#include "world/StructureRotation.hpp"
#include "world/StructureTemplate.hpp"
#include "world/gen/StructurePlacement.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using mc::world::Block;
using mc::world::BlockOrientation;
using mc::world::BlockState;
using mc::world::LocalPos;
using mc::world::rotateLocal;
using mc::world::rotateOrientation;
using mc::world::rotateState;
using mc::world::StructureRotation;
using mc::world::gen::StructureChunk;
using mc::world::gen::StructurePlacement;

void testPlacement() {
    // Villages: spacing 34, separation 8, salt 10387312.
    const StructurePlacement villages{34, 8, 10387312, mc::world::gen::SpreadType::Linear};
    const std::uint64_t seed = 0x1234ABCDULL;

    // Deterministic: same region + seed -> same origin.
    assert(villages.originChunk(0, 0, seed) == villages.originChunk(0, 0, seed));
    // A different salt (a different structure set) moves the origin.
    const StructurePlacement other{34, 8, 12345, mc::world::gen::SpreadType::Linear};
    // (not guaranteed different for every region, but for this one it is)
    assert(!(villages.originChunk(0, 0, seed) == other.originChunk(0, 0, seed)));

    // Exactly one chunk per region is the origin, and it *is* a structure chunk.
    for (int gridX = -2; gridX <= 2; ++gridX) {
        for (int gridZ = -2; gridZ <= 2; ++gridZ) {
            const StructureChunk origin = villages.originChunk(gridX, gridZ, seed);
            // origin lands inside its region's [grid*spacing, grid*spacing+range).
            assert(origin.x >= gridX * 34 && origin.x < gridX * 34 + (34 - 8));
            assert(origin.z >= gridZ * 34 && origin.z < gridZ * 34 + (34 - 8));
            assert(villages.isStructureChunk(origin.x, origin.z, seed));
            // a neighbour of the origin is not a structure chunk.
            assert(!villages.isStructureChunk(origin.x + 1, origin.z, seed) ||
                   origin.x + 1 >= gridX * 34 + 34);
        }
    }

    // floorDiv keeps regions continuous across zero.
    assert(StructurePlacement::floorDiv(-1, 34) == -1);
    assert(StructurePlacement::floorDiv(-34, 34) == -1);
    assert(StructurePlacement::floorDiv(-35, 34) == -2);
    assert(StructurePlacement::floorDiv(33, 34) == 0);
}

void testRotation() {
    // Four clockwise quarter-turns of a coordinate return to the start (2x3 box).
    const LocalPos start{1, 5, 0};
    LocalPos p = start;
    int sizeX = 2;
    int sizeZ = 3;
    for (int turn = 0; turn < 4; ++turn) {
        p = rotateLocal(p, sizeX, sizeZ, StructureRotation::Clockwise90);
        std::swap(sizeX, sizeZ); // the footprint swaps on a quarter turn
    }
    assert(p == start);

    // Every rotated coordinate stays inside the rotated box.
    for (int x = 0; x < 4; ++x) {
        for (int z = 0; z < 6; ++z) {
            const LocalPos r = rotateLocal({x, 0, z}, 4, 6, StructureRotation::Clockwise90);
            assert(r.x >= 0 && r.x < 6 && r.z >= 0 && r.z < 4);
        }
    }

    // Horizontal facing turns clockwise; vertical is untouched.
    assert(rotateOrientation(BlockOrientation::North, StructureRotation::Clockwise90) ==
           BlockOrientation::East);
    assert(rotateOrientation(BlockOrientation::West, StructureRotation::Clockwise90) ==
           BlockOrientation::North);
    assert(rotateOrientation(BlockOrientation::North, StructureRotation::Clockwise180) ==
           BlockOrientation::South);
    assert(rotateOrientation(BlockOrientation::Up, StructureRotation::Clockwise90) ==
           BlockOrientation::Up);

    // A furnace's facing turns; stone (no facing) is unchanged.
    assert(rotateState(BlockState{Block::Furnace}, StructureRotation::Clockwise90).orientation() ==
           BlockOrientation::East);
    assert(rotateState(BlockState{Block::Stone}, StructureRotation::Clockwise90) ==
           BlockState{Block::Stone});
}

// A 2x1x2 template: stone, a north-facing furnace, and a chest with a loot table.
mc::world::StructureTemplateDef makeTemplate() {
    using namespace mc::world;
    StructureTemplateDef tmpl;
    tmpl.sizeX = 2;
    tmpl.sizeY = 1;
    tmpl.sizeZ = 2;
    tmpl.palette.push_back({Block::Stone, BlockState{Block::Stone}.rawId(), true});
    tmpl.palette.push_back({Block::Furnace, BlockState{Block::Furnace}.rawId(), true});
    tmpl.palette.push_back({Block::Chest, BlockState{Block::Chest}.rawId(), true});
    tmpl.blockEntities.push_back({"minecraft:chest", "minecraft:chests/test", ""});
    tmpl.blocks.push_back({0, 0, 0, 0, kNoBlockEntity});       // stone
    tmpl.blocks.push_back({1, 0, 0, 1, kNoBlockEntity});       // furnace
    tmpl.blocks.push_back({0, 0, 1, 2, 0});                    // chest -> loot
    return tmpl;
}

void testPlacer() {
    using namespace mc::world;
    const StructureTemplateDef tmpl = makeTemplate();

    // Placed wholly inside chunk 0 at origin (5,64,5), rotation None.
    {
        Chunk chunk;
        std::vector<gen::TreeBorderBlock> border;
        std::vector<StructureLootPlacement> loot;
        placeStructure(chunk, 0, 0, tmpl, 5, 64, 5, StructureRotation::None, border, loot);
        assert(border.empty());
        assert(chunk.state(5, 64, 5).block() == Block::Stone);
        assert(chunk.state(6, 64, 5).block() == Block::Furnace);
        assert(chunk.state(5, 64, 6).block() == Block::Chest);
        assert(loot.size() == 1);
        assert(loot[0].worldX == 5 && loot[0].worldY == 64 && loot[0].worldZ == 6);
        assert(loot[0].lootTable == "minecraft:chests/test");
    }

    // Placed straddling the chunk-1 border: cells at worldX>=16 go to `border`.
    {
        Chunk chunk;
        std::vector<gen::TreeBorderBlock> border;
        std::vector<StructureLootPlacement> loot;
        placeStructure(chunk, 0, 0, tmpl, 15, 64, 5, StructureRotation::None, border, loot);
        // stone at x=15 stays; furnace at x=16 crosses to chunk 1.
        assert(chunk.state(15, 64, 5).block() == Block::Stone);
        bool furnaceInBorder = false;
        for (const auto& block : border) {
            if (block.worldX == 16 && block.state.block() == Block::Furnace) {
                furnaceInBorder = true;
            }
        }
        assert(furnaceInBorder);
    }

    // Rotation moves cells the way rotateLocal says.
    {
        Chunk chunk;
        std::vector<gen::TreeBorderBlock> border;
        std::vector<StructureLootPlacement> loot;
        placeStructure(chunk, 0, 0, tmpl, 5, 64, 5, StructureRotation::Clockwise90, border, loot);
        // furnace local (1,0,0) -> rotateLocal in 2x2 -> (sizeZ-1-0,0,1)=(1,0,1) -> world (6,64,6)
        const LocalPos r = rotateLocal({1, 0, 0}, tmpl.sizeX, tmpl.sizeZ, StructureRotation::Clockwise90);
        assert(chunk.state(5 + r.x, 64, 5 + r.z).block() == Block::Furnace);
        // its facing turned east.
        assert(chunk.state(5 + r.x, 64, 5 + r.z).orientation() == BlockOrientation::East);
    }
}

// STORAGE -> PLACEMENT -> LOOT: place the template, take the chest's emitted loot
// placement, and roll its table into items — the milestone's three layers wired
// end to end (headless; the live ChunkStreamer wiring is the follow-on step).
void testEndToEndLoot() {
    using mc::world::Chunk;
    using mc::world::StructureLootPlacement;
    using mc::world::StructureRotation;
    const auto tmpl = makeTemplate();

    Chunk chunk;
    std::vector<mc::world::gen::TreeBorderBlock> border;
    std::vector<StructureLootPlacement> loot;
    placeStructure(chunk, 0, 0, tmpl, 5, 64, 5, StructureRotation::None, border, loot);
    assert(loot.size() == 1);

    // The chest's table (as a datapack would supply it), keyed by the placement's id.
    constexpr std::string_view kJson = R"({
      "type": "minecraft:chest",
      "pools": [ { "rolls": { "type": "minecraft:uniform", "min": 3.0, "max": 5.0 },
        "entries": [
          { "type": "minecraft:item", "name": "minecraft:apple", "weight": 10,
            "functions": [ { "function": "minecraft:set_count",
              "count": { "type": "minecraft:uniform", "min": 1.0, "max": 3.0 } } ] },
          { "type": "minecraft:item", "name": "minecraft:coal", "weight": 10 } ] } ]
    })";
    const auto json = mc::core::Json::parse(kJson);
    const auto def = mc::data::jeChestLoot(json, loot[0].lootTable);
    assert(def.has_value() && def->identifier == "minecraft:chests/test");

    mc::gameplay::ChestLootTable table;
    std::uint64_t a = mc::rng::seedFromValue(0x51EED2ULL);
    std::uint64_t b = mc::rng::seedFromValue(0x51EED2ULL);
    const auto first = table.roll(*def, a);
    const auto second = table.roll(*def, b);
    assert(!first.empty());
    assert(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        assert(first[i].item == second[i].item && first[i].count == second[i].count);
    }
}

// The placer carves its footprint above the floor, so terrain that rises into a
// structure on uneven ground does not poke through the cells the template leaves
// as void. A hollow 2×3×2 template (floor only) placed into solid-filled terrain
// clears everything above the floor in its footprint.
void testFootprintClear() {
    using namespace mc::world;
    StructureTemplateDef hollow;
    hollow.sizeX = 2;
    hollow.sizeY = 3;
    hollow.sizeZ = 2;
    hollow.palette.push_back({Block::Stone, BlockState{Block::Stone}.rawId(), true});
    // Only a floor at y=0 (the four footprint cells); y=1,2 are structure void.
    for (int x = 0; x < 2; ++x)
        for (int z = 0; z < 2; ++z)
            hollow.blocks.push_back({static_cast<std::int8_t>(x), 0, static_cast<std::int8_t>(z), 0,
                                     kNoBlockEntity});

    Chunk chunk;
    // Solid terrain everywhere up to y=66.
    for (int x = 0; x < 8; ++x)
        for (int z = 0; z < 8; ++z)
            for (int y = 60; y <= 66; ++y)
                chunk.setState(x, y, z, BlockState{Block::Dirt});

    std::vector<mc::world::gen::TreeBorderBlock> border;
    std::vector<StructureLootPlacement> loot;
    placeStructure(chunk, 0, 0, hollow, 3, 64, 3, StructureRotation::None, border, loot);

    // Floor stamped at y=64; the void cells above (y=65,66) are carved to air even
    // though the template never listed them, so no terrain pokes into the shell.
    for (int x = 3; x < 5; ++x) {
        for (int z = 3; z < 5; ++z) {
            assert(chunk.state(x, 64, z).block() == Block::Stone); // floor
            assert(chunk.block(x, 65, z) == Block::Air);           // carved (was Dirt)
            assert(chunk.block(x, 66, z) == Block::Air);           // carved (was Dirt)
        }
    }
    // Below the floor is untouched (structure rests on ground, no hole beneath).
    assert(chunk.block(3, 63, 3) == Block::Dirt);
    // Outside the footprint is untouched.
    assert(chunk.block(0, 65, 0) == Block::Dirt);
}

// A jigsaw marker (unresolved, skipped as a block) is replaced by its final_state.
void testJigsawFinalState() {
    using namespace mc::world;
    StructureTemplateDef t;
    t.sizeX = 1;
    t.sizeY = 1;
    t.sizeZ = 1;
    // A single jigsaw block whose final_state is stone. No palette/blocks, so the
    // only thing placed is the final_state at the jigsaw position.
    t.jigsaws.push_back(
        {0, 0, 0, BlockOrientation::North, "n", "tg", "p", "minecraft:stone", false});
    Chunk chunk;
    std::vector<mc::world::gen::TreeBorderBlock> border;
    std::vector<StructureLootPlacement> loot;
    placeStructure(chunk, 0, 0, t, 5, 64, 5, StructureRotation::None, border, loot);
    assert(chunk.state(5, 64, 5).block() == Block::Stone);
}

} // namespace

int main() {
    testPlacement();
    testRotation();
    testPlacer();
    testEndToEndLoot();
    testFootprintClear();
    testJigsawFinalState();
    return 0;
}

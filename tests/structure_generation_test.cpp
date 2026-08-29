// STRUCT-2 (wiring, second half): the generation step placeStructures.
//
// What is pinned: with no registered sets the step is a no-op (the chunk is
// untouched — the guarantee that a build with no structure content generates as
// before); with a set whose placement selects the chunk and whose biome gate
// passes, the template is stamped onto the chunk's surface at a deterministic
// rotation; a biome gate that fails suppresses it; and the placement/seed decide
// which chunk gets the structure.

#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/StructureManager.hpp"
#include "world/StructureTemplate.hpp"
#include "world/WorldConstants.hpp"
#include "world/gen/Biome.hpp"
#include "world/gen/StructureGenerator.hpp"
#include "world/gen/StructurePlacement.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using mc::world::Block;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::StructureManager;
using mc::world::StructureSet;
using mc::world::StructureTemplateDef;
using mc::world::gen::Biome;
using mc::world::gen::placeStructures;
using mc::world::gen::SpreadType;
using mc::world::gen::StructurePlacement;
using mc::world::gen::TreeBorderBlock;

constexpr int kGround = 64;

// A chunk with a flat stone floor at kGround, so surfaceHeight is kGround.
Chunk flatChunk() {
    Chunk chunk;
    for (int x = 0; x < mc::world::kChunkWidth; ++x) {
        for (int z = 0; z < mc::world::kChunkDepth; ++z) {
            chunk.setState(x, kGround, z, BlockState{Block::Stone});
        }
    }
    return chunk;
}

// A 1x1x1 template of a single distinctive block (a furnace), so its placement is
// unambiguous against the stone floor.
StructureTemplateDef furnaceMarker() {
    StructureTemplateDef t;
    t.sizeX = 1;
    t.sizeY = 1;
    t.sizeZ = 1;
    t.palette.push_back({Block::Furnace, BlockState{Block::Furnace}.rawId(), true});
    t.blocks.push_back({0, 0, 0, 0, mc::world::kNoBlockEntity});
    return t;
}

// spacing 1 / separation 0 makes every chunk a structure chunk — a clean way to
// force placement in a unit test.
StructureSet everyChunkSetFor(std::string templateId, std::vector<Biome> biomes) {
    StructureSet set;
    set.placement = StructurePlacement{1, 0, 0, SpreadType::Linear};
    set.templateId = std::move(templateId);
    set.biomes = std::move(biomes);
    return set;
}
StructureSet everyChunkSet(std::vector<Biome> biomes) {
    return everyChunkSetFor("minecraft:test/marker", std::move(biomes));
}

void testNoOpWhenEmpty() {
    Chunk chunk = flatChunk();
    StructureManager empty;
    std::vector<TreeBorderBlock> border;
    placeStructures(chunk, 0, 0, 12345, empty, [](int, int) { return Biome::Plains; }, [](int, int) { return kGround; }, border);
    // untouched: still a bare stone floor, air above it.
    assert(chunk.state(0, kGround, 0).block() == Block::Stone);
    assert(chunk.state(0, kGround + 1, 0).block() == Block::Air);
    assert(border.empty());
}

void testStamped() {
    Chunk chunk = flatChunk();
    StructureManager manager;
    manager.add("minecraft:test/marker", furnaceMarker());
    manager.addSet(everyChunkSet({})); // any biome
    std::vector<TreeBorderBlock> border;
    placeStructures(chunk, 0, 0, 999, manager, [](int, int) { return Biome::Plains; }, [](int, int) { return kGround; }, border);
    // origin (0,0) at ground: the furnace replaced the surface cell.
    assert(chunk.state(0, kGround, 0).block() == Block::Furnace);
}

void testBiomeGate() {
    Chunk chunk = flatChunk();
    StructureManager manager;
    manager.add("minecraft:test/marker", furnaceMarker());
    manager.addSet(everyChunkSet({Biome::SnowyTundra})); // only snowy
    std::vector<TreeBorderBlock> border;
    // A plains origin biome fails the gate -> not stamped.
    placeStructures(chunk, 0, 0, 999, manager, [](int, int) { return Biome::Plains; }, [](int, int) { return kGround; }, border);
    assert(chunk.state(0, kGround, 0).block() == Block::Stone);
    // The allowed biome stamps it.
    placeStructures(chunk, 0, 0, 999, manager, [](int, int) { return Biome::SnowyTundra; }, [](int, int) { return kGround; }, border);
    assert(chunk.state(0, kGround, 0).block() == Block::Furnace);
}

void testPlacementSelectsChunk() {
    // Villages-like spacing: not every chunk is a structure chunk.
    StructureManager manager;
    manager.add("minecraft:test/marker", furnaceMarker());
    // Villages-like spacing (not every chunk is a structure chunk).
    StructureSet spaced;
    spaced.placement = StructurePlacement{16, 4, 777, SpreadType::Linear};
    spaced.templateId = "minecraft:test/marker";
    manager.addSet(spaced);
    const std::uint64_t seed = 0xBEEFULL;
    const auto& set = manager.sets()[0];

    int stamped = 0;
    for (int cx = 0; cx < 16; ++cx) {
        for (int cz = 0; cz < 16; ++cz) {
            Chunk chunk = flatChunk();
            std::vector<TreeBorderBlock> border;
            placeStructures(chunk, cx, cz, seed, manager, [](int, int) { return Biome::Plains; },
                            [](int, int) { return kGround; }, border);
            const bool here = chunk.state(0, kGround, 0).block() == Block::Furnace;
            assert(here == set.placement.isStructureChunk(cx, cz, seed));
            stamped += here ? 1 : 0;
        }
    }
    // A 16x16 chunk area is one 16-spacing region: roughly one structure.
    assert(stamped >= 1);
}

// A template carrying a chest with a loot table, for the chest-replay path.
StructureTemplateDef chestTemplate() {
    using namespace mc::world;
    StructureTemplateDef t;
    t.sizeX = 1;
    t.sizeY = 1;
    t.sizeZ = 1;
    t.palette.push_back({Block::Chest, BlockState{Block::Chest}.rawId(), true});
    t.blockEntities.push_back({"minecraft:chest", "minecraft:chests/test", ""});
    t.blocks.push_back({0, 0, 0, 0, 0});
    return t;
}

// structureChestsForChunk replays placement to locate chests + loot tables, at the
// same rotation/position the block pass used — for the gameplay chest binding.
void testChestReplay() {
    using mc::world::gen::structureChestsForChunk;
    StructureManager manager;
    manager.add("minecraft:test/vault", chestTemplate());
    manager.addSet(everyChunkSetFor("minecraft:test/vault", {}));

    const auto chests = structureChestsForChunk(
        0, 0, 4242, manager, kGround, [](int, int) { return Biome::Plains; });
    assert(chests.size() == 1);
    assert(chests[0].lootTable == "minecraft:chests/test");
    // origin (0,0) at kGround, 1x1x1 chest at local (0,0,0).
    assert(chests[0].worldX == 0 && chests[0].worldY == kGround && chests[0].worldZ == 0);

    // No sets / wrong biome -> no chests.
    StructureManager empty;
    assert(structureChestsForChunk(0, 0, 4242, empty, kGround,
                                   [](int, int) { return Biome::Plains; })
               .empty());
    StructureManager gated;
    gated.add("minecraft:test/vault", chestTemplate());
    gated.addSet(everyChunkSetFor("minecraft:test/vault", {Biome::SnowyTundra}));
    assert(structureChestsForChunk(0, 0, 4242, gated, kGround,
                                   [](int, int) { return Biome::Plains; })
               .empty());
}

// A structure rests on the terrain under a tree, not on the treetop, and clears
// the tree canopy that would poke through it — surfaceHeight steps over logs/leaves.
void testTreeIsSteppedOver() {
    Chunk chunk = flatChunk(); // stone floor at kGround
    // A tree on the origin column: trunk + a leaf above the ground.
    chunk.setState(0, kGround + 1, 0, BlockState{Block::OakLog});
    chunk.setState(0, kGround + 2, 0, BlockState{Block::OakLeaves});
    StructureManager manager;
    manager.add("minecraft:test/marker", furnaceMarker());
    manager.addSet(everyChunkSet({}));
    std::vector<TreeBorderBlock> border;
    placeStructures(chunk, 0, 0, 4321, manager, [](int, int) { return Biome::Plains; }, [](int, int) { return kGround; }, border);
    // Placed on the ground (kGround), not on the log a cell higher.
    assert(chunk.state(0, kGround, 0).block() == Block::Furnace);
    // The tree above the structure was cleared (canopy pass), no log/leaf poking up.
    assert(chunk.block(0, kGround + 1, 0) == Block::Air);
    assert(chunk.block(0, kGround + 2, 0) == Block::Air);
}

// A submerged column (water over the ground) is skipped — a land structure does
// not build on the sea floor.
void testSubmergedColumnSkipped() {
    Chunk chunk = flatChunk();
    chunk.setState(0, kGround + 1, 0, BlockState{Block::Water}); // ocean over the origin
    StructureManager manager;
    manager.add("minecraft:test/marker", furnaceMarker());
    manager.addSet(everyChunkSet({}));
    std::vector<TreeBorderBlock> border;
    placeStructures(chunk, 0, 0, 4321, manager, [](int, int) { return Biome::Plains; }, [](int, int) { return kGround; }, border);
    // Not stamped: the origin ground is under water.
    assert(chunk.state(0, kGround, 0).block() == Block::Stone);
}

// A jigsaw-kind set expands and stamps its pieces through the generation step.
void testJigsawSetGenerates() {
    using namespace mc::world;
    StructureManager manager;
    // A one-block start template (a furnace, so it is distinguishable from the
    // stone floor), reachable from a start pool; no jigsaws, so it does not expand.
    StructureTemplateDef start;
    start.sizeX = 1;
    start.sizeY = 1;
    start.sizeZ = 1;
    start.palette.push_back({Block::Furnace, BlockState{Block::Furnace}.rawId(), true});
    start.blocks.push_back({0, 0, 0, 0, kNoBlockEntity});
    manager.add("minecraft:start_t", start);

    mc::data::StructurePoolDef p;
    p.id = "minecraft:start";
    p.fallback = "minecraft:empty";
    mc::data::StructurePoolElement e;
    e.location = "minecraft:start_t";
    e.weight = 1;
    p.elements.push_back(e);
    manager.addPool(p);

    StructureSet set;
    set.placement = StructurePlacement{1, 0, 0, SpreadType::Linear}; // every chunk
    set.kind = StructureKind::Jigsaw;
    set.startPool = "minecraft:start";
    set.size = 1;
    set.maxDistance = 80;
    manager.addSet(set);

    Chunk chunk = flatChunk(); // stone floor at kGround
    std::vector<TreeBorderBlock> border;
    placeStructures(chunk, 0, 0, 123, manager, [](int, int) { return Biome::Plains; }, [](int, int) { return kGround; }, border);
    // The start piece stamped its furnace at the origin ground.
    assert(chunk.state(0, kGround, 0).block() == Block::Furnace);
}

} // namespace

int main() {
    testNoOpWhenEmpty();
    testStamped();
    testBiomeGate();
    testPlacementSelectsChunk();
    testChestReplay();
    testTreeIsSteppedOver();
    testSubmergedColumnSkipped();
    testJigsawSetGenerates();
    return 0;
}

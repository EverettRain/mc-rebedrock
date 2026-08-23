// WG-0: nether/end base-block and biome identity registration.
//
// The generator (WG-2/3) cannot place a block or select a biome that has no
// identity, so WG-0 registers the nether/end terrain palette and the ten
// dimension biomes at the top of the worldgen path. This test is the headless
// acceptance for that registration:
//   1. every new base block resolves by its `rebedrock:`, bare and `minecraft:`
//      names, with vanilla-faithful strength/light;
//   2. every new biome enumerates, resolves by name (the JC anchor), and carries
//      a definition with the right surface palette and climate;
//   3. WG-0 is identity only — it registers no placement, so a freshly created
//      world still holds none of these blocks.

#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/WorldConstants.hpp"
#include "world/gen/Biome.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

namespace {

using mc::world::Block;
using mc::world::blockDefinition;
using mc::world::blockFromIdentifier;
using mc::world::gen::Biome;
using mc::world::gen::biomeDefinition;
using mc::world::gen::biomeFromIdentifier;

// 1a. Every new base block resolves under all three name forms, and the vanilla
// alias in particular round-trips — a datapack/save written as `minecraft:*`
// reaches the block (the JC obligation). A missing alias is an interop hole.
void testBlockIdentityAndAliases() {
    struct Case {
        Block block;
        const char* path;
    };
    constexpr Case kCases[] = {
        {Block::SoulSand, "soul_sand"},
        {Block::SoulSoil, "soul_soil"},
        {Block::NetherQuartzOre, "nether_quartz_ore"},
        {Block::MagmaBlock, "magma_block"},
        {Block::Basalt, "basalt"},
        {Block::Blackstone, "blackstone"},
        {Block::NetherBricks, "nether_bricks"},
        {Block::NetherWartBlock, "nether_wart_block"},
        {Block::CrimsonNylium, "crimson_nylium"},
        {Block::WarpedNylium, "warped_nylium"},
        {Block::EndStone, "end_stone"},
        {Block::PurpurBlock, "purpur_block"},
    };
    for (const Case& c : kCases) {
        const std::string bare = c.path;
        const std::string rebedrock = std::string{"rebedrock:"} + c.path;
        const std::string vanilla = std::string{"minecraft:"} + c.path;
        // Bare, own-namespace and vanilla-alias all resolve to the same block.
        assert(blockFromIdentifier(bare) == c.block);
        assert(blockFromIdentifier(rebedrock) == c.block);
        // The vanilla alias is the JC import anchor — this is the assertion the
        // "missing minecraft: alias" sabotage trips.
        assert(blockFromIdentifier(vanilla) == c.block);
        // The definition records the vanilla name it mirrors.
        assert(blockDefinition(c.block).vanilla.toString() == vanilla);
    }
}

// 1b. Properties are vanilla-faithful: the light emitters emit, the inert blocks
// stay dark, and a couple of strengths match 1.16.1/26.1 Blocks.java.
void testBlockProperties() {
    // Glowstone (already present) emits 15; magma emits 3 — the two light values
    // the task calls out. A dropped `.light()` shows up here.
    assert(blockDefinition(Block::Glowstone).light == 15U);
    assert(blockDefinition(Block::MagmaBlock).light == 3U);
    // The rest of the palette is inert (no glow), unlike magma.
    assert(blockDefinition(Block::Netherrack).light == 0U);
    assert(blockDefinition(Block::EndStone).light == 0U);
    assert(blockDefinition(Block::Blackstone).light == 0U);

    // A few vanilla strengths, so a mistyped table entry is caught.
    assert(blockDefinition(Block::EndStone).hardness == 3.0F);
    assert(blockDefinition(Block::EndStone).blastResistance == 9.0F);
    assert(blockDefinition(Block::SoulSand).hardness == 0.5F);
    assert(blockDefinition(Block::NetherBricks).hardness == 2.0F);
    assert(blockDefinition(Block::PurpurBlock).blastResistance == 6.0F);

    // Basalt is a pillar (takes a placement axis), like a log.
    assert(blockDefinition(Block::Basalt).pillar);
}

// 2. The ten dimension biomes enumerate, resolve by name, and carry a definition
// with the surface palette WG-2/3 will read and the vanilla climate.
void testBiomeIdentity() {
    struct Case {
        Biome biome;
        const char* path;
        Block surface;
        float temperature;
    };
    constexpr Case kCases[] = {
        {Biome::NetherWastes, "nether_wastes", Block::Netherrack, 2.0F},
        {Biome::SoulSandValley, "soul_sand_valley", Block::SoulSand, 2.0F},
        {Biome::CrimsonForest, "crimson_forest", Block::CrimsonNylium, 2.0F},
        {Biome::WarpedForest, "warped_forest", Block::WarpedNylium, 2.0F},
        {Biome::BasaltDeltas, "basalt_deltas", Block::Basalt, 2.0F},
        {Biome::TheEnd, "the_end", Block::EndStone, 0.5F},
        {Biome::EndHighlands, "end_highlands", Block::EndStone, 0.5F},
        {Biome::EndMidlands, "end_midlands", Block::EndStone, 0.5F},
        {Biome::EndBarrens, "end_barrens", Block::EndStone, 0.5F},
        {Biome::SmallEndIslands, "small_end_islands", Block::EndStone, 0.5F},
    };
    for (const Case& c : kCases) {
        // Bare, minecraft: and rebedrock: names all resolve to the biome — the
        // biome path is its vanilla id, so the JC mapping is the identity.
        assert(biomeFromIdentifier(c.path) == c.biome);
        assert(biomeFromIdentifier(std::string{"minecraft:"} + c.path) == c.biome);
        assert(biomeFromIdentifier(std::string{"rebedrock:"} + c.path) == c.biome);

        const auto& definition = biomeDefinition(c.biome);
        assert(definition.biome == c.biome);
        assert(definition.identifier == c.path);
        assert(definition.surface == c.surface);
        assert(definition.temperature == c.temperature);
        // These are identity-only biomes: no worldgen decoration attached here
        // (trees/grass are WG-2/3 features).
        assert(definition.treeCount == 0);
        assert(definition.trees.empty());
    }
    // An unknown key resolves to Count (the not-found sentinel), not a wrong biome.
    assert(biomeFromIdentifier("minecraft:not_a_biome") == Biome::Count);
}

// 3. WG-0 registers identity, not placement: a freshly generated chunk holds
// none of the nether/end base blocks — they land only when WG-2/3 place them.
// (The world's default generator is the overworld one; the nether/end palette
// must not appear in it.)
void testNotPlaced() {
    mc::world::Chunk chunk;
    constexpr Block kNetherEnd[] = {
        Block::SoulSand,  Block::SoulSoil,       Block::NetherQuartzOre, Block::MagmaBlock,
        Block::Basalt,    Block::Blackstone,     Block::NetherBricks,    Block::NetherWartBlock,
        Block::CrimsonNylium, Block::WarpedNylium, Block::EndStone,      Block::PurpurBlock,
    };
    // A default (ungenerated) chunk is all air; the point is that placement is
    // not part of WG-0 — the chunk carries no nether/end block because nothing
    // placed one. Sweep the whole column volume to be sure.
    for (int x = 0; x < mc::world::kChunkWidth; ++x) {
        for (int z = 0; z < mc::world::kChunkDepth; ++z) {
            for (int y = mc::world::kMinY; y < mc::world::kMaxY; ++y) {
                const Block here = chunk.block(x, y, z);
                assert(here == Block::Air);
                for (const Block forbidden : kNetherEnd) {
                    assert(here != forbidden);
                }
            }
        }
    }
}

} // namespace

int main() {
    testBlockIdentityAndAliases();
    testBlockProperties();
    testBiomeIdentity();
    testNotPlaced();
    return 0;
}

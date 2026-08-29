#pragma once

#include "world/Block.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace mc::world::gen {

// The overworld biomes this build generates. Java has sixty-odd; these are the
// ones that differ from each other in the two things the generator currently
// reads off a biome — the terrain shape it asks for and the trees that grow on
// it — so adding the rest is a matter of extending the table below.
enum class Biome : std::uint8_t {
    Ocean,
    Beach,
    Plains,
    Forest,
    BirchForest,
    Taiga,
    SnowyTundra,
    Desert,
    Savanna,
    Jungle,
    DarkForest,
    Swamp,
    Mountains,
    River,
    DeepOcean,
    // WG-0 nether biomes (vanilla's five). Identity only: the BiomeSource that
    // selects between them by multi-noise is WG-2. Their surface palette and
    // colours are recorded here so the generator has a complete definition to
    // read; placement stays downstream.
    NetherWastes,
    SoulSandValley,
    CrimsonForest,
    WarpedForest,
    BasaltDeltas,
    // WG-0 end biomes. The TheEndBiomeSource that selects by distance to centre
    // is WG-3.
    TheEnd,
    EndHighlands,
    EndMidlands,
    EndBarrens,
    SmallEndIslands,
    Count,
};

// The tree shapes ported from vanilla's ConfiguredFeatures. Each names a
// trunk/foliage placer pair rather than a block palette, because the palette is
// carried alongside it in the biome's tree list.
enum class TreeKind : std::uint8_t {
    // StraightTrunkPlacer(4, 2, 0) + BlobFoliagePlacer(2, 0, 3).
    Oak,
    // LargeOakTrunkPlacer(3, 11, 0) + LargeOakFoliagePlacer(2, 0, 4).
    FancyOak,
    // StraightTrunkPlacer(5, 2, 0) + BlobFoliagePlacer(2, 0, 3).
    Birch,
    // StraightTrunkPlacer(5, 2, 1) + SpruceFoliagePlacer(2, 1, 1..2).
    Spruce,
    // StraightTrunkPlacer(6, 4, 0) + PineFoliagePlacer(1, 0, 1, 3).
    Pine,
    // StraightTrunkPlacer(4, 8, 0) + BlobFoliagePlacer(2, 0, 3), with vines.
    JungleTree,
    // ForkingTrunkPlacer(5, 2, 2) + AcaciaFoliagePlacer(2, 0).
    Acacia,
    // DarkOakTrunkPlacer(6, 2, 1) + DarkOakFoliagePlacer(0, 0), a 2x2 trunk.
    DarkOak,
    // StraightTrunkPlacer(5, 3, 0) + BlobFoliagePlacer(3, 0, 3), sits in water.
    SwampOak,
};

struct TreeChoice final {
    TreeKind kind = TreeKind::Oak;
    Block log = Block::OakLog;
    Block leaves = Block::OakLeaves;
    // Relative weight inside the biome's RandomFeature selector.
    float weight = 1.0F;
    // How deep a standing-water column this tree tolerates (TreeFeatureConfig
    // maxWaterDepth). Swamp trees grow in one block of shallow water; dry-land
    // trees accept none.
    int maxWaterDepth = 0;
};

// Java's Biome plus the two ConfiguredFeature slots the generator reads.
struct BiomeDefinition final {
    Biome biome = Biome::Plains;
    // The registry path, matching the vanilla biome id.
    std::string_view identifier;
    // Biome.Builder#depth / #scale, which drive the noise column's shape.
    float depth = 0.125F;
    float scale = 0.05F;
    // Only used to pick between the snow, sand and grass surface families.
    float temperature = 0.8F;
    // Biome.Builder#downfall, which together with the temperature
    // indexes the vanilla grass/foliage colour maps.
    float downfall = 0.4F;
    // SurfaceConfig: what the top block and the few below it become.
    Block surface = Block::Grass;
    Block filler = Block::Dirt;
    Block underwaterSurface = Block::Gravel;
    // Decorator.COUNT_EXTRA: `count` trees per chunk, plus one more with
    // probability `extraChance`.
    int treeCount = 0;
    float extraTreeChance = 0.0F;
    int extraTreeCount = 1;
    std::span<const TreeChoice> trees;
    // DefaultBiomeFeatures grass/flower rates, expressed per chunk.
    int grassCount = 2;
    int flowerCount = 1;
};

[[nodiscard]] const BiomeDefinition& biomeDefinition(Biome biome);

// Resolves a biome registry key to its Biome. A biome's `identifier` path is its
// vanilla id, so this accepts the bare name (`nether_wastes`), the `minecraft:`
// alias (`minecraft:nether_wastes`) and the `rebedrock:` key alike — the JC
// import anchor, mirroring blockFromIdentifier. Returns Count for an unknown key.
[[nodiscard]] Biome biomeFromIdentifier(std::string_view text);

// The grass-family atlas layers (top / side / plant) tinted with this biome's
// vanilla grass colour (BiomeColors.getGrassColor through the grass colour map,
// plus the swamp/dark-forest overrides). The renderer fills the table at atlas
// build time; the mesher reads it for grass blocks. Swamp returns the lighter
// tone; swampDarkGrassLayers() carries the darker per-block noise tone.
[[nodiscard]] const world::BlockTextureLayers& biomeGrassLayers(Biome biome);
void setBiomeGrassLayers(Biome biome, world::BlockTextureLayers layers);
[[nodiscard]] const world::BlockTextureLayers& swampDarkGrassLayers();
void setSwampDarkGrassLayers(world::BlockTextureLayers layers);

// The baked per-biome foliage atlas layer for a leaf block: the untinted leaf
// texture tinted with the biome's foliage colour at build time, so the terrain
// colour never depends on per-vertex data reaching the fragment shader. Spruce
// and birch keep fixed tones via terrainLeafLayer.
[[nodiscard]] float biomeFoliageLayer(Biome biome, world::Block leaves);
void setBiomeFoliageLayer(Biome biome, world::Block leaves, float layer);
// The untinted terrain grass/leaf atlas layers, kept as fallbacks.
[[nodiscard]] float terrainGrassTopLayer();
[[nodiscard]] float terrainGrassPlantLayer();
[[nodiscard]] float terrainLeafLayer(world::Block leaves);
void setTerrainGrassLayers(float top, float plant);
void setTerrainLeafLayer(world::Block leaves, float layer);

// The log/leaves pair a sapling of this block would grow, so the loot tables and
// the tree features agree on what belongs to which wood set.
[[nodiscard]] constexpr Block leavesForLog(Block log) {
    switch (log) {
    case Block::SpruceLog: return Block::SpruceLeaves;
    case Block::BirchLog: return Block::BirchLeaves;
    case Block::JungleLog: return Block::JungleLeaves;
    case Block::AcaciaLog: return Block::AcaciaLeaves;
    case Block::DarkOakLog: return Block::DarkOakLeaves;
    default: return Block::OakLeaves;
    }
}

} // namespace mc::world::gen

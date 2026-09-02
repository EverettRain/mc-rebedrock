#pragma once

#include "world/Block.hpp"
#include "world/attribute/EnvironmentAttribute.hpp"

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
    SnowyPlains,
    Desert,
    Savanna,
    Jungle,
    DarkForest,
    Swamp,
    WindsweptHills,
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

// Biome.ClimateSettings#temperatureModifier (26.1). FROZEN carves warm patches
// out of a frozen ocean through three noise fields; the only biomes that use it
// are the frozen oceans, which this build does not have yet, so it is registered
// and left unimplemented rather than guessed at.
enum class TemperatureModifier : std::uint8_t {
    None,
    Frozen,
};

// Biome.Precipitation: what falls on a position, if anything.
enum class Precipitation : std::uint8_t {
    None,
    Rain,
    Snow,
};

// BiomeSpecialEffects.GrassColorModifier (26.1): a per-position adjustment on top
// of whatever the grass colour map gave. Applied where the colour is resolved —
// SWAMP reads a noise field at the block position, so it cannot be folded into
// the biome's own colour.
enum class GrassColorModifier : std::uint8_t {
    None,
    // ARGB.opaque((base & 0xFEFEFE) + 0x28340A >> 1)
    DarkForest,
    // Two fixed tones chosen by BIOME_INFO_NOISE at (x * 0.0225, z * 0.0225).
    Swamp,
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
    // The registry path: the biome's **26.1** vanilla id, which is the one
    // authoritative name for it. Two biomes were renamed after 1.16
    // (snowy_tundra -> snowy_plains, mountains -> windswept_hills); the old ids
    // live on only as aliases inside biomeFromIdentifier, never here.
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

    // BiomeSpecialEffects (26.1). The whole record is five colour fields now —
    // fog, sky, cloud, ambient particles, music and the ambient sounds all moved
    // to EnvironmentAttribute in 26.x, and land in BM-3 rather than here.
    //
    // An override of 0 means "no override": resolve the colour through the
    // grass/foliage colour map by (temperature, downfall), the way vanilla does
    // for a biome that names none. No vanilla biome overrides to pure black, so
    // zero is free to mean absent.
    std::uint32_t waterColor = 0x3F76E4U;  // 4159204, BiomeSpecialEffects' default
    std::uint32_t foliageColorOverride = 0U;
    std::uint32_t dryFoliageColorOverride = 0U;
    std::uint32_t grassColorOverride = 0U;
    GrassColorModifier grassColorModifier = GrassColorModifier::None;

    // Biome.ClimateSettings (26.1). `temperature` and `downfall` are above, with
    // the colour-map inputs they double as.
    //
    // A desert, a savanna, the nether and the end all set hasPrecipitation
    // false: no rain reaches them however hard it is raining elsewhere. Rain
    // used to fall on every biome in the game, because nothing asked.
    bool hasPrecipitation = true;
    TemperatureModifier temperatureModifier = TemperatureModifier::None;
};

[[nodiscard]] const BiomeDefinition& biomeDefinition(Biome biome);

// Resolves a biome registry key to its Biome. A biome's `identifier` path is its
// 26.1 vanilla id, so this accepts the bare name (`nether_wastes`), the
// `minecraft:` alias (`minecraft:nether_wastes`) and the `rebedrock:` key alike —
// the JC import anchor, mirroring blockFromIdentifier. The two 1.16 ids vanilla
// has since renamed (`snowy_tundra`, `mountains`) resolve too, so an older save or
// pack is read rather than dropped. Returns Count for an unknown key.
[[nodiscard]] Biome biomeFromIdentifier(std::string_view text);

// A biome's four resolved surface colours, 0xRRGGBB — the four ColorResolvers
// vanilla's BiomeColors declares. `grass` and `foliage` are the colour-map
// lookups (or the biome's override); `grassColorModifier` is NOT applied here,
// because SWAMP depends on the block position and so belongs where the mesher
// resolves a column.
//
// This replaces a table of per-biome *atlas layers*: grass and leaf textures used
// to be tinted at atlas build time and given a layer of their own per biome. That
// cost one atlas layer per (biome x tintable texture) — which does not survive
// 26.1's 66 biomes, let alone a data pack adding more — and being a discrete
// layer it could neither blend across a biome border nor tint water at all.
struct BiomeSurfaceColors final {
    std::uint32_t grass = 0xFFFFFFU;
    std::uint32_t foliage = 0xFFFFFFU;
    std::uint32_t dryFoliage = 0xFFFFFFU;
    std::uint32_t water = 0xFFFFFFU;
};

// White until the renderer resolves the colour maps, which is also what a
// headless build sees: a white tint multiplies to the raw texture, so terrain
// without a resource pack renders untinted rather than black.
[[nodiscard]] const BiomeSurfaceColors& biomeSurfaceColors(Biome biome);
void setBiomeSurfaceColors(Biome biome, BiomeSurfaceColors colors);

// BiomeSpecialEffects.GrassColorModifier#modifyColor, applied at a block
// position on top of the biome's own grass colour.
[[nodiscard]] std::uint32_t applyGrassColorModifier(GrassColorModifier modifier,
                                                    std::uint32_t baseColor, int x, int z);

// The biome's attribute layer (26.1's `Biome#attributes`), which overrides the
// dimension's. The overworld biomes carry the sky colour their temperature
// derives (OverworldBiomes.baseBiome); the nether biomes each carry their own
// fog. Attributes carrying a reference (music, ambient sounds, ambient
// particles) are absent until the audio wiring gives them a side table.
[[nodiscard]] const attribute::EnvAttrLayer& biomeAttributes(Biome biome);

// Biome#getHeightAdjustedTemperature: the biome's base temperature, cooled with
// height above the snow line (sea level + 17) by a noise-perturbed lapse rate.
// This is what puts snow on a mountain that stands in a temperate biome — the
// same biome is rainy at its foot and snowy at its peak.
[[nodiscard]] float heightAdjustedTemperature(Biome biome, int x, int y, int z, int seaLevel);

// Biome#warmEnoughToRain / #coldEnoughToSnow: the 0.15 threshold on the
// height-adjusted temperature.
[[nodiscard]] bool warmEnoughToRain(Biome biome, int x, int y, int z, int seaLevel);
[[nodiscard]] bool coldEnoughToSnow(Biome biome, int x, int y, int z, int seaLevel);

// Biome#getPrecipitationAt: NONE where the biome has no precipitation at all,
// otherwise SNOW or RAIN by temperature at this position.
[[nodiscard]] Precipitation precipitationAt(Biome biome, int x, int y, int z, int seaLevel);

// The UNTINTED terrain atlas layers for the two texture families the biome
// colours multiply into. The atlas also holds tinted copies of these for items
// and the GUI, where there is no biome to ask; terrain reads these and takes its
// colour from the vertex tint instead.
//
// grassBlockSide is the plain dirt-and-grey-edge texture and grassBlockOverlay
// the grass-shaped mask drawn over it — vanilla's grass_block model draws the
// sides twice for exactly this reason, and the mesher does the same, because a
// single pre-composited texture cannot be tinted without turning the dirt green.
//
// Spruce and birch leaves keep a fixed tinted layer: their colour is a constant
// in every biome (FoliageColor's evergreen and birch tones), so tinting them per
// vertex would only spend bandwidth to reach the same pixel.
[[nodiscard]] float terrainGrassTopLayer();
[[nodiscard]] float terrainGrassPlantLayer();
[[nodiscard]] float terrainGrassSideLayer();
[[nodiscard]] float terrainGrassOverlayLayer();
[[nodiscard]] float terrainLeafLayer(world::Block leaves);
void setTerrainGrassLayers(float top, float plant, float side, float overlay);
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

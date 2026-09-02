#include "world/gen/Biome.hpp"

#include "world/gen/JavaRandom.hpp"
#include "world/gen/NoiseSampler.hpp"

namespace mc::world::gen {
namespace {

// The tree lists are the RandomFeature selectors from vanilla's
// DefaultBiomeFeatures, flattened to weights. Where vanilla nests a selector
// inside another (forests pick birch, then fancy oak, then plain oak) the
// nested probabilities are multiplied out into one flat list.
constexpr std::array<TreeChoice, 2> kPlainsTrees{{
    {TreeKind::FancyOak, Block::OakLog, Block::OakLeaves, 0.33F},
    {TreeKind::Oak, Block::OakLog, Block::OakLeaves, 0.67F},
}};

constexpr std::array<TreeChoice, 3> kForestTrees{{
    {TreeKind::Birch, Block::BirchLog, Block::BirchLeaves, 0.2F},
    {TreeKind::FancyOak, Block::OakLog, Block::OakLeaves, 0.1F},
    {TreeKind::Oak, Block::OakLog, Block::OakLeaves, 0.7F},
}};

constexpr std::array<TreeChoice, 1> kBirchForestTrees{{
    {TreeKind::Birch, Block::BirchLog, Block::BirchLeaves, 1.0F},
}};

constexpr std::array<TreeChoice, 2> kTaigaTrees{{
    {TreeKind::Spruce, Block::SpruceLog, Block::SpruceLeaves, 0.33F},
    {TreeKind::Pine, Block::SpruceLog, Block::SpruceLeaves, 0.67F},
}};

constexpr std::array<TreeChoice, 1> kSnowyTrees{{
    {TreeKind::Spruce, Block::SpruceLog, Block::SpruceLeaves, 1.0F},
}};

constexpr std::array<TreeChoice, 2> kMountainTrees{{
    {TreeKind::Spruce, Block::SpruceLog, Block::SpruceLeaves, 0.66F},
    {TreeKind::Oak, Block::OakLog, Block::OakLeaves, 0.34F},
}};

constexpr std::array<TreeChoice, 2> kSavannaTrees{{
    {TreeKind::Acacia, Block::AcaciaLog, Block::AcaciaLeaves, 0.8F},
    {TreeKind::Oak, Block::OakLog, Block::OakLeaves, 0.2F},
}};

constexpr std::array<TreeChoice, 2> kJungleTrees{{
    {TreeKind::JungleTree, Block::JungleLog, Block::JungleLeaves, 0.9F},
    {TreeKind::Oak, Block::OakLog, Block::OakLeaves, 0.1F},
}};

constexpr std::array<TreeChoice, 2> kDarkForestTrees{{
    {TreeKind::DarkOak, Block::DarkOakLog, Block::DarkOakLeaves, 0.8F},
    {TreeKind::Birch, Block::BirchLog, Block::BirchLeaves, 0.2F},
}};

constexpr std::array<TreeChoice, 1> kSwampTrees{{
    // Vanilla's SWAMP_TREE has maxWaterDepth(1): the oak grows through a single
    // block of standing water, which is what lets the flooded swamp carry trees.
    {TreeKind::SwampOak, Block::OakLog, Block::OakLeaves, 1.0F, 1},
}};

// depth/scale are Biome.Builder's values for the vanilla biome of the same name;
// the tree counts are its Decorator.COUNT_EXTRA arguments. temperature and
// downfall are the vanilla Biome.Builder values, which index the grass/foliage
// colour maps (the surface-family pick only reads the temperature).
const std::array<BiomeDefinition, static_cast<std::size_t>(Biome::Count)> kBiomeRegistry{{
    {Biome::Ocean, "ocean", -1.0F, 0.1F, 0.5F, 0.5F, Block::Gravel, Block::Gravel, Block::Gravel,
     0, 0.0F, 1, {}, 0, 0},
    {Biome::Beach, "beach", 0.0F, 0.025F, 0.8F, 0.4F, Block::Sand, Block::Sand, Block::Sand,
     0, 0.0F, 1, {}, 0, 0},
    {Biome::Plains, "plains", 0.125F, 0.05F, 0.8F, 0.4F, Block::Grass, Block::Dirt, Block::Gravel,
     0, 0.05F, 1, kPlainsTrees, 6, 4},
    {Biome::Forest, "forest", 0.1F, 0.2F, 0.7F, 0.8F, Block::Grass, Block::Dirt, Block::Gravel,
     10, 0.1F, 1, kForestTrees, 2, 2},
    {Biome::BirchForest, "birch_forest", 0.1F, 0.2F, 0.6F, 0.6F, Block::Grass, Block::Dirt,
     Block::Gravel, 10, 0.1F, 1, kBirchForestTrees, 2, 1},
    {Biome::Taiga, "taiga", 0.2F, 0.2F, 0.25F, 0.8F, Block::Grass, Block::Dirt, Block::Gravel,
     10, 0.1F, 1, kTaigaTrees, 1, 1},
    {Biome::SnowyPlains, "snowy_plains", 0.125F, 0.05F, 0.0F, 0.5F, Block::SnowBlock, Block::Dirt,
     Block::Gravel, 0, 0.1F, 1, kSnowyTrees, 0, 0},
    {Biome::Desert, "desert", 0.125F, 0.05F, 2.0F, 0.0F, Block::Sand, Block::Sand, Block::Gravel,
     0, 0.0F, 1, {}, 0, 0, 0x3F76E4U, 0U, 0U, 0U, GrassColorModifier::None, false},
    {Biome::Savanna, "savanna", 0.125F, 0.05F, 2.0F, 0.0F, Block::Grass, Block::Dirt, Block::Gravel,
     1, 0.1F, 1, kSavannaTrees, 8, 2, 0x3F76E4U, 0U, 0U, 0U, GrassColorModifier::None, false},
    {Biome::Jungle, "jungle", 0.1F, 0.2F, 0.95F, 0.9F, Block::Grass, Block::Dirt, Block::Gravel,
     50, 0.1F, 1, kJungleTrees, 12, 4},
    // 26.1's dark forest keeps the default water and darkens its grass through a
    // modifier rather than an override, so the colour still follows the colour map.
    {Biome::DarkForest, "dark_forest", 0.1F, 0.2F, 0.7F, 0.8F, Block::Grass, Block::Dirt,
     Block::Gravel, 10, 0.1F, 1, kDarkForestTrees, 2, 2,
     0x3F76E4U, 0U, 0x7B5334U, 0U, GrassColorModifier::DarkForest},
    // Vanilla's swamp is a flat wetland that sits at or just below sea level so
    // standing water covers most of it; the depth keeps it flooded while the
    // low scale keeps it flat.
    // Vanilla's swamp floor stays dirt under the standing water, so its trees
    // can root in the shallows as well as on the dry patches.
    // The swamp is the one overworld biome that overrides water and foliage
    // outright (murky green), and its grass is the two-tone noise modifier.
    {Biome::Swamp, "swamp", -0.25F, 0.1F, 0.8F, 0.9F, Block::Grass, Block::Dirt, Block::Dirt,
     2, 0.1F, 1, kSwampTrees, 5, 1,
     0x617B64U, 0x6A7039U, 0x7B5334U, 0U, GrassColorModifier::Swamp},
    {Biome::WindsweptHills, "windswept_hills", 1.0F, 0.5F, 0.2F, 0.3F, Block::Grass, Block::Dirt, Block::Gravel,
     0, 0.1F, 1, kMountainTrees, 2, 1},
    // River: a shallow water channel a couple of blocks below sea level.
    {Biome::River, "river", -0.5F, 0.0F, 0.5F, 0.5F, Block::Sand, Block::Gravel, Block::Gravel,
     0, 0.0F, 1, {}, 0, 0},
    // Deep ocean: the basins far from shore, whose floor sits well below the
    // shallow-ocean floor.
    {Biome::DeepOcean, "deep_ocean", -1.8F, 0.1F, 0.5F, 0.5F, Block::Gravel, Block::Gravel, Block::Gravel,
     0, 0.0F, 1, {}, 0, 0},
    // WG-0 nether biomes (vanilla's five). depth/scale are the vanilla
    // Biome.Builder values (0.1/0.2), temperature 2.0 / downfall 0.0 as every
    // nether biome carries. The surface palette records each biome's floor
    // block so WG-2 has a complete definition to read; no trees/grass (those
    // are nether vegetal features, WG-2/5). Underwater surface is the floor
    // block itself since the nether has no seas.
    {Biome::NetherWastes, "nether_wastes", 0.1F, 0.2F, 2.0F, 0.0F, Block::Netherrack, Block::Netherrack,
     Block::Netherrack, 0, 0.0F, 1, {}, 0, 0, 0x3F76E4U, 0U, 0U, 0U, GrassColorModifier::None, false},
    {Biome::SoulSandValley, "soul_sand_valley", 0.1F, 0.2F, 2.0F, 0.0F, Block::SoulSand, Block::SoulSoil,
     Block::SoulSand, 0, 0.0F, 1, {}, 0, 0, 0x3F76E4U, 0U, 0U, 0U, GrassColorModifier::None, false},
    {Biome::CrimsonForest, "crimson_forest", 0.1F, 0.2F, 2.0F, 0.0F, Block::CrimsonNylium, Block::Netherrack,
     Block::CrimsonNylium, 0, 0.0F, 1, {}, 0, 0, 0x3F76E4U, 0U, 0U, 0U, GrassColorModifier::None, false},
    {Biome::WarpedForest, "warped_forest", 0.1F, 0.2F, 2.0F, 0.0F, Block::WarpedNylium, Block::Netherrack,
     Block::WarpedNylium, 0, 0.0F, 1, {}, 0, 0, 0x3F76E4U, 0U, 0U, 0U, GrassColorModifier::None, false},
    {Biome::BasaltDeltas, "basalt_deltas", 0.1F, 0.2F, 2.0F, 0.0F, Block::Basalt, Block::Blackstone,
     Block::Basalt, 0, 0.0F, 1, {}, 0, 0, 0x3F76E4U, 0U, 0U, 0U, GrassColorModifier::None, false},
    // WG-0 end biomes. All end_stone surfaced; temperature 0.5 / downfall 0.5 as
    // the vanilla end biomes carry. Placement (centre island vs outer ring vs
    // void) is WG-3's TheEndBiomeSource, driven by distance to origin.
    {Biome::TheEnd, "the_end", 0.1F, 0.2F, 0.5F, 0.5F, Block::EndStone, Block::EndStone, Block::EndStone,
     0, 0.0F, 1, {}, 0, 0, 0x3F76E4U, 0U, 0U, 0U, GrassColorModifier::None, false},
    {Biome::EndHighlands, "end_highlands", 0.1F, 0.2F, 0.5F, 0.5F, Block::EndStone, Block::EndStone,
     Block::EndStone, 0, 0.0F, 1, {}, 0, 0, 0x3F76E4U, 0U, 0U, 0U, GrassColorModifier::None, false},
    {Biome::EndMidlands, "end_midlands", 0.1F, 0.2F, 0.5F, 0.5F, Block::EndStone, Block::EndStone,
     Block::EndStone, 0, 0.0F, 1, {}, 0, 0, 0x3F76E4U, 0U, 0U, 0U, GrassColorModifier::None, false},
    {Biome::EndBarrens, "end_barrens", 0.1F, 0.2F, 0.5F, 0.5F, Block::EndStone, Block::EndStone,
     Block::EndStone, 0, 0.0F, 1, {}, 0, 0, 0x3F76E4U, 0U, 0U, 0U, GrassColorModifier::None, false},
    {Biome::SmallEndIslands, "small_end_islands", 0.1F, 0.2F, 0.5F, 0.5F, Block::EndStone, Block::EndStone,
     Block::EndStone, 0, 0.0F, 1, {}, 0, 0, 0x3F76E4U, 0U, 0U, 0U, GrassColorModifier::None, false},
}};

// The 1.16 ids two of these biomes carried before vanilla renamed them. The 26.1
// id is the authoritative one and the only thing `identifier` holds; these keep a
// save, a data pack or a command written against the old name resolving instead of
// silently reading as "unknown biome".
struct LegacyAlias final {
    std::string_view identifier;
    Biome biome;
};

constexpr std::array<LegacyAlias, 2> kLegacyAliases{{
    {"snowy_tundra", Biome::SnowyPlains},
    {"mountains", Biome::WindsweptHills},
}};

} // namespace

const BiomeDefinition& biomeDefinition(Biome biome) {
    const auto index = static_cast<std::size_t>(biome);
    return kBiomeRegistry[index < kBiomeRegistry.size() ? index : 0U];
}

Biome biomeFromIdentifier(std::string_view text) {
    // Strip the namespace: a biome's path is its vanilla id, so `minecraft:` and
    // `rebedrock:` (and the bare name) all resolve to the same biome.
    const auto colon = text.find(':');
    const std::string_view path = colon == std::string_view::npos ? text : text.substr(colon + 1U);
    for (const auto& definition : kBiomeRegistry) {
        if (definition.identifier == path) {
            return definition.biome;
        }
    }
    for (const auto& alias : kLegacyAliases) {
        if (alias.identifier == path) {
            return alias.biome;
        }
    }
    return Biome::Count;
}

namespace {
// Resolved per-biome surface colours. White until the renderer reads the colour
// maps and fills them in — a white tint multiplies to the raw texture, so a
// headless build (and the frames before the atlas is ready) renders untinted
// rather than black.
std::array<BiomeSurfaceColors, static_cast<std::size_t>(Biome::Count)> kBiomeColors{};

// SwampBiome's grass mottle, seeded exactly like vanilla's BIOME_INFO_NOISE.
[[nodiscard]] const SimplexNoiseSampler& swampGrassNoise() {
    static const SimplexNoiseSampler sampler = [] {
        JavaRandom random{2345ULL};
        return SimplexNoiseSampler{random};
    }();
    return sampler;
}
} // namespace

const BiomeSurfaceColors& biomeSurfaceColors(Biome biome) {
    const auto index = static_cast<std::size_t>(biome);
    return kBiomeColors[index < kBiomeColors.size() ? index : 0U];
}

void setBiomeSurfaceColors(Biome biome, BiomeSurfaceColors colors) {
    const auto index = static_cast<std::size_t>(biome);
    if (index < kBiomeColors.size()) {
        kBiomeColors[index] = colors;
    }
}

namespace {
float kTerrainGrassTop = 0.0F;
float kTerrainGrassPlant = 0.0F;
float kTerrainGrassSide = 0.0F;
float kTerrainGrassOverlay = 0.0F;
std::array<float, static_cast<std::size_t>(world::Block::Count)> kTerrainLeafLayers{};
} // namespace

float terrainGrassTopLayer() { return kTerrainGrassTop; }
float terrainGrassPlantLayer() { return kTerrainGrassPlant; }
float terrainGrassSideLayer() { return kTerrainGrassSide; }
float terrainGrassOverlayLayer() { return kTerrainGrassOverlay; }

void setTerrainGrassLayers(float top, float plant, float side, float overlay) {
    kTerrainGrassTop = top;
    kTerrainGrassPlant = plant;
    kTerrainGrassSide = side;
    kTerrainGrassOverlay = overlay;
}

float terrainLeafLayer(world::Block leaves) {
    const auto index = static_cast<std::size_t>(leaves);
    return index < kTerrainLeafLayers.size() ? kTerrainLeafLayers[index] : 0.0F;
}

void setTerrainLeafLayer(world::Block leaves, float layer) {
    const auto index = static_cast<std::size_t>(leaves);
    if (index < kTerrainLeafLayers.size()) {
        kTerrainLeafLayers[index] = layer;
    }
}

namespace {
// Biome.TEMPERATURE_NOISE: PerlinSimplexNoise(WorldgenRandom(LegacyRandomSource(1234)),
// octaves {0}) — one simplex layer, the same shape the swamp mottle uses.
[[nodiscard]] const SimplexNoiseSampler& temperatureNoise() {
    static const SimplexNoiseSampler sampler = [] {
        JavaRandom random{1234ULL};
        return SimplexNoiseSampler{random};
    }();
    return sampler;
}
} // namespace

float heightAdjustedTemperature(Biome biome, int x, int y, int z, int seaLevel) {
    // TemperatureModifier.FROZEN is registered but not implemented: it belongs to
    // the frozen oceans, which this build has no biome for. Reaching it with a
    // biome that claims it would silently return the unmodified temperature, so
    // the definition table is the place that must not claim it yet.
    const float base = biomeDefinition(biome).temperature;
    const int snowLevel = seaLevel + 17;
    if (y <= snowLevel) {
        return base;
    }
    // Java: adjusted - (noise * 8 + y - snowLevel) * 0.05F / 40.0F, with the
    // noise sampled at (x / 8, z / 8).
    const auto variation = static_cast<float>(
        temperatureNoise().sample(static_cast<double>(x) / 8.0, static_cast<double>(z) / 8.0) *
        8.0);
    return base - (variation + static_cast<float>(y - snowLevel)) * 0.05F / 40.0F;
}

bool warmEnoughToRain(Biome biome, int x, int y, int z, int seaLevel) {
    return heightAdjustedTemperature(biome, x, y, z, seaLevel) >= 0.15F;
}

bool coldEnoughToSnow(Biome biome, int x, int y, int z, int seaLevel) {
    return !warmEnoughToRain(biome, x, y, z, seaLevel);
}

Precipitation precipitationAt(Biome biome, int x, int y, int z, int seaLevel) {
    // The biome-level gate comes first and short-circuits: a desert never pays
    // for a noise sample to be told it is dry.
    if (!biomeDefinition(biome).hasPrecipitation) {
        return Precipitation::None;
    }
    return coldEnoughToSnow(biome, x, y, z, seaLevel) ? Precipitation::Snow : Precipitation::Rain;
}

std::uint32_t applyGrassColorModifier(GrassColorModifier modifier, std::uint32_t baseColor,
                                      int x, int z) {
    switch (modifier) {
    case GrassColorModifier::None:
        break;
    case GrassColorModifier::DarkForest:
        // ARGB.opaque((base & 0xFEFEFE) + 0x28340A >> 1) — vanilla's own
        // arithmetic, including the shift binding looser than the addition.
        return (((baseColor & 0xFEFEFEU) + 0x28340AU) >> 1U) & 0xFFFFFFU;
    case GrassColorModifier::Swamp:
        // Two fixed tones, not a tint of the colour map: -11766212 / -9801671.
        return swampGrassNoise().sample(static_cast<double>(x) * 0.0225,
                                        static_cast<double>(z) * 0.0225) < -0.1
            ? 0x4C763CU
            : 0x6A7039U;
    }
    return baseColor;
}

} // namespace mc::world::gen

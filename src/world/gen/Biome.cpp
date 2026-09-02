#include "world/gen/Biome.hpp"

#include <unordered_map>

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
     0, 0.0F, 1, {}, 0, 0},
    {Biome::Savanna, "savanna", 0.125F, 0.05F, 1.2F, 0.0F, Block::Grass, Block::Dirt, Block::Gravel,
     1, 0.1F, 1, kSavannaTrees, 8, 2},
    {Biome::Jungle, "jungle", 0.1F, 0.2F, 0.95F, 0.9F, Block::Grass, Block::Dirt, Block::Gravel,
     50, 0.1F, 1, kJungleTrees, 12, 4},
    {Biome::DarkForest, "dark_forest", 0.1F, 0.2F, 0.7F, 0.8F, Block::Grass, Block::Dirt,
     Block::Gravel, 10, 0.1F, 1, kDarkForestTrees, 2, 2},
    // Vanilla's swamp is a flat wetland that sits at or just below sea level so
    // standing water covers most of it; the depth keeps it flooded while the
    // low scale keeps it flat.
    // Vanilla's swamp floor stays dirt under the standing water, so its trees
    // can root in the shallows as well as on the dry patches.
    {Biome::Swamp, "swamp", -0.25F, 0.1F, 0.8F, 0.9F, Block::Grass, Block::Dirt, Block::Dirt,
     2, 0.1F, 1, kSwampTrees, 5, 1},
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
     Block::Netherrack, 0, 0.0F, 1, {}, 0, 0},
    {Biome::SoulSandValley, "soul_sand_valley", 0.1F, 0.2F, 2.0F, 0.0F, Block::SoulSand, Block::SoulSoil,
     Block::SoulSand, 0, 0.0F, 1, {}, 0, 0},
    {Biome::CrimsonForest, "crimson_forest", 0.1F, 0.2F, 2.0F, 0.0F, Block::CrimsonNylium, Block::Netherrack,
     Block::CrimsonNylium, 0, 0.0F, 1, {}, 0, 0},
    {Biome::WarpedForest, "warped_forest", 0.1F, 0.2F, 2.0F, 0.0F, Block::WarpedNylium, Block::Netherrack,
     Block::WarpedNylium, 0, 0.0F, 1, {}, 0, 0},
    {Biome::BasaltDeltas, "basalt_deltas", 0.1F, 0.2F, 2.0F, 0.0F, Block::Basalt, Block::Blackstone,
     Block::Basalt, 0, 0.0F, 1, {}, 0, 0},
    // WG-0 end biomes. All end_stone surfaced; temperature 0.5 / downfall 0.5 as
    // the vanilla end biomes carry. Placement (centre island vs outer ring vs
    // void) is WG-3's TheEndBiomeSource, driven by distance to origin.
    {Biome::TheEnd, "the_end", 0.1F, 0.2F, 0.5F, 0.5F, Block::EndStone, Block::EndStone, Block::EndStone,
     0, 0.0F, 1, {}, 0, 0},
    {Biome::EndHighlands, "end_highlands", 0.1F, 0.2F, 0.5F, 0.5F, Block::EndStone, Block::EndStone,
     Block::EndStone, 0, 0.0F, 1, {}, 0, 0},
    {Biome::EndMidlands, "end_midlands", 0.1F, 0.2F, 0.5F, 0.5F, Block::EndStone, Block::EndStone,
     Block::EndStone, 0, 0.0F, 1, {}, 0, 0},
    {Biome::EndBarrens, "end_barrens", 0.1F, 0.2F, 0.5F, 0.5F, Block::EndStone, Block::EndStone,
     Block::EndStone, 0, 0.0F, 1, {}, 0, 0},
    {Biome::SmallEndIslands, "small_end_islands", 0.1F, 0.2F, 0.5F, 0.5F, Block::EndStone, Block::EndStone,
     Block::EndStone, 0, 0.0F, 1, {}, 0, 0},
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
// Per-biome grass atlas layers, filled at atlas build time. Defaults are empty;
// the mesher only uses them once the renderer has tinted and registered them.
std::array<world::BlockTextureLayers, static_cast<std::size_t>(Biome::Count)>
    kBiomeGrassLayers{};
world::BlockTextureLayers kSwampDarkGrassLayers{};
std::unordered_map<std::uint64_t, float> kBiomeFoliageLayers{};
float kTerrainGrassTopLayer = 0.0F;
float kTerrainGrassPlantLayer = 0.0F;
std::unordered_map<world::Block, float> kTerrainLeafLayers{};
} // namespace

const world::BlockTextureLayers& biomeGrassLayers(Biome biome) {
    const auto index = static_cast<std::size_t>(biome);
    return kBiomeGrassLayers[index < kBiomeGrassLayers.size() ? index : 0U];
}

void setBiomeGrassLayers(Biome biome, world::BlockTextureLayers layers) {
    kBiomeGrassLayers[static_cast<std::size_t>(biome)] = layers;
}

const world::BlockTextureLayers& swampDarkGrassLayers() {
    return kSwampDarkGrassLayers;
}

void setSwampDarkGrassLayers(world::BlockTextureLayers layers) {
    kSwampDarkGrassLayers = layers;
}

float biomeFoliageLayer(Biome biome, world::Block leaves) {
    const auto biomeIndex = static_cast<std::size_t>(biome);
    const auto found = kBiomeFoliageLayers.find(
        biomeIndex * 64U + static_cast<std::uint32_t>(leaves));
    return found != kBiomeFoliageLayers.end() ? found->second : 0.0F;
}

void setBiomeFoliageLayer(Biome biome, world::Block leaves, float layer) {
    kBiomeFoliageLayers[static_cast<std::size_t>(biome) * 64U +
                        static_cast<std::uint32_t>(leaves)] = layer;
}

float terrainGrassTopLayer() {
    return kTerrainGrassTopLayer;
}

float terrainGrassPlantLayer() {
    return kTerrainGrassPlantLayer;
}

float terrainLeafLayer(world::Block leaves) {
    const auto found = kTerrainLeafLayers.find(leaves);
    return found != kTerrainLeafLayers.end() ? found->second : 0.0F;
}

void setTerrainGrassLayers(float top, float plant) {
    kTerrainGrassTopLayer = top;
    kTerrainGrassPlantLayer = plant;
}

void setTerrainLeafLayer(world::Block leaves, float layer) {
    kTerrainLeafLayers[leaves] = layer;
}

} // namespace mc::world::gen

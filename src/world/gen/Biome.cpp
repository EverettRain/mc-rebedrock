#include "world/gen/Biome.hpp"

namespace mc::world::gen {
namespace {

// The tree lists are the RandomFeature selectors from 1.16.1's
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
    {TreeKind::SwampOak, Block::OakLog, Block::OakLeaves, 1.0F},
}};

// depth/scale are Biome.Builder's values for the vanilla biome of the same name;
// the tree counts are its Decorator.COUNT_EXTRA arguments.
const std::array<BiomeDefinition, static_cast<std::size_t>(Biome::Count)> kBiomeRegistry{{
    {Biome::Ocean, "ocean", -1.0F, 0.1F, 0.5F, Block::Gravel, Block::Gravel, Block::Gravel,
     0, 0.0F, 1, {}, 0, 0},
    {Biome::Beach, "beach", 0.0F, 0.025F, 0.8F, Block::Sand, Block::Sand, Block::Sand,
     0, 0.0F, 1, {}, 0, 0},
    {Biome::Plains, "plains", 0.125F, 0.05F, 0.8F, Block::Grass, Block::Dirt, Block::Gravel,
     0, 0.05F, 1, kPlainsTrees, 6, 4},
    {Biome::Forest, "forest", 0.1F, 0.2F, 0.7F, Block::Grass, Block::Dirt, Block::Gravel,
     10, 0.1F, 1, kForestTrees, 2, 2},
    {Biome::BirchForest, "birch_forest", 0.1F, 0.2F, 0.6F, Block::Grass, Block::Dirt,
     Block::Gravel, 10, 0.1F, 1, kBirchForestTrees, 2, 1},
    {Biome::Taiga, "taiga", 0.2F, 0.2F, 0.25F, Block::Grass, Block::Dirt, Block::Gravel,
     10, 0.1F, 1, kTaigaTrees, 1, 1},
    {Biome::SnowyTundra, "snowy_tundra", 0.125F, 0.05F, 0.0F, Block::SnowBlock, Block::Dirt,
     Block::Gravel, 0, 0.1F, 1, kSnowyTrees, 0, 0},
    {Biome::Desert, "desert", 0.125F, 0.05F, 2.0F, Block::Sand, Block::Sand, Block::Gravel,
     0, 0.0F, 1, {}, 0, 0},
    {Biome::Savanna, "savanna", 0.125F, 0.05F, 1.2F, Block::Grass, Block::Dirt, Block::Gravel,
     1, 0.1F, 1, kSavannaTrees, 8, 2},
    {Biome::Jungle, "jungle", 0.1F, 0.2F, 0.95F, Block::Grass, Block::Dirt, Block::Gravel,
     50, 0.1F, 1, kJungleTrees, 12, 4},
    {Biome::DarkForest, "dark_forest", 0.1F, 0.2F, 0.7F, Block::Grass, Block::Dirt,
     Block::Gravel, 10, 0.1F, 1, kDarkForestTrees, 2, 2},
    {Biome::Swamp, "swamp", -0.2F, 0.1F, 0.8F, Block::Grass, Block::Dirt, Block::Gravel,
     2, 0.1F, 1, kSwampTrees, 5, 1},
    {Biome::Mountains, "mountains", 1.0F, 0.5F, 0.2F, Block::Grass, Block::Dirt, Block::Gravel,
     0, 0.1F, 1, kMountainTrees, 2, 1},
    // River: a shallow water channel a couple of blocks below sea level.
    {Biome::River, "river", -0.5F, 0.0F, 0.5F, Block::Sand, Block::Gravel, Block::Gravel,
     0, 0.0F, 1, {}, 0, 0},
    // Deep ocean: the basins far from shore, whose floor sits well below the
    // shallow-ocean floor.
    {Biome::DeepOcean, "deep_ocean", -1.8F, 0.1F, 0.5F, Block::Gravel, Block::Gravel, Block::Gravel,
     0, 0.0F, 1, {}, 0, 0},
}};

} // namespace

const BiomeDefinition& biomeDefinition(Biome biome) {
    const auto index = static_cast<std::size_t>(biome);
    return kBiomeRegistry[index < kBiomeRegistry.size() ? index : 0U];
}

} // namespace mc::world::gen

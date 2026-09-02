#pragma once

// Per-biome spawn tables, the shape of 26.1's MobSpawnSettings
// (`world/level/biome/MobSpawnSettings.java`): a weighted list of spawner
// entries per mob category.
//
// This replaces a table the spawner built from the entity registry — every
// naturally-spawning species, in every biome, at weight 1 with a group of 1-4.
// Two things that hid: a desert had the same pig density as a plain, and every
// species was equally likely, so adding one silently rebalanced the rest.
//
// Loading follows the same rule BlockTags established, and for the same reason:
// **built-in defaults first, data pack over the top**. A player's *resource*
// pack carries only `assets/`, so a table that could only come from
// `data/minecraft/worldgen/biome/…` would be empty in most installs — which
// for spawning means an empty world.

#include "assets/ResourceProvider.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "world/gen/Biome.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mc::gameplay {

// MobSpawnSettings.SpawnerData: which species, how likely against its
// neighbours in the same category, and how many arrive together.
struct SpawnerData final {
    const entities::EntityType* type = nullptr;
    // Vanilla's weights are integers (pig 10, cow 8, zombie 95) and only ever
    // compared against each other's sum.
    int weight = 1;
    int minGroup = 1;
    int maxGroup = 1;
};

inline constexpr std::size_t kMobCategoryCount = 5U;

// The categories a natural spawn can belong to. Ambient/WaterCreature have no
// species in this game yet, but the slots exist so a bat does not need the
// table reshaped.
struct MobSpawnSettings final {
    std::array<std::vector<SpawnerData>, kMobCategoryCount> byCategory;

    [[nodiscard]] const std::vector<SpawnerData>& forCategory(
        entities::MobCategory category) const {
        return byCategory[static_cast<std::size_t>(category)];
    }

    [[nodiscard]] bool empty(entities::MobCategory category) const {
        return forCategory(category).empty();
    }
};

class BiomeSpawnTables final {
  public:
    // 26.1's own numbers, compiled in: `BiomeDefaultFeatures.farmAnimals`
    // (pig 10, cow 8, groups of 4) and `monsters` (zombie 95, groups of 4),
    // applied to the biomes vanilla applies them to. This is the floor a load()
    // starts from and the whole table for a headless caller.
    void loadBuiltinDefaults();

    // Overlays `data/minecraft/worldgen/biome/<biome>.json`'s `spawners` block
    // where a pack supplies one. A biome the pack does not mention keeps its
    // built-in table rather than going empty.
    void load(const assets::ResourceProvider& resources);

    [[nodiscard]] const MobSpawnSettings& settings(world::gen::Biome biome) const {
        return settings_[static_cast<std::size_t>(biome)];
    }

    // Whether any biome at all lists a species in this category. The spawner
    // skips a category that has none instead of sampling positions for it —
    // AMBIENT and WATER_CREATURE are empty in this game, and a sample that can
    // only ever read an empty list still pays for the biome query ahead of it.
    [[nodiscard]] bool anySpecies(entities::MobCategory category) const;

    // Whether a pack supplied this biome's table. Diagnostics only: both
    // sources are equally authoritative once loaded.
    [[nodiscard]] bool dataDriven(world::gen::Biome biome) const {
        return dataDriven_[static_cast<std::size_t>(biome)];
    }

    // For tests and for building a table by hand.
    void set(world::gen::Biome biome, entities::MobCategory category,
             std::vector<SpawnerData> entries);

  private:
    std::array<MobSpawnSettings, static_cast<std::size_t>(world::gen::Biome::Count)> settings_{};
    std::array<bool, static_cast<std::size_t>(world::gen::Biome::Count)> dataDriven_{};
};

// The pack-relative path a biome's data lives at, e.g.
// "worldgen/biome/plains.json". Derived from the biome's own 26.1 id, so this is
// not a second place where biome ids are written down. Exposed so the loader and
// its test agree.
[[nodiscard]] std::string biomeDataPath(world::gen::Biome biome);

// The process-wide tables, the same shape blockTags() has and for the same
// reason: the pack stack is read once at startup, and everything that spawns
// reads the result. A spawner copies from here when a world loads, so a
// headless test can still hand one its own tables.
[[nodiscard]] BiomeSpawnTables& biomeSpawnTables();

} // namespace mc::gameplay

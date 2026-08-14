#include "gameplay/MobSpawnSettings.hpp"

#include "assets/ResourceProvider.hpp"
#include "gameplay/entities/EntityRegistry.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

// The biome spawn tables. What is pinned here is the loading contract that the
// deployment constraint forces: **built-in defaults are the floor**, because a
// player's resource pack carries only `assets/` and a table that could come
// only from `data/minecraft/worldgen/biome/…` would leave most installs with an
// empty world.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"mob_spawn_settings_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using mc::gameplay::BiomeSpawnTables;
using mc::gameplay::entities::MobCategory;
using mc::world::gen::Biome;

void writeBiome(const std::filesystem::path& packRoot, std::string_view name,
                std::string_view json) {
    const auto path = packRoot / "data" / "minecraft" / "worldgen" / "biome" / name;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary};
    REQUIRE(static_cast<bool>(out));
    out << json;
}

[[nodiscard]] int weightOf(const BiomeSpawnTables& tables, Biome biome, MobCategory category,
                           std::string_view species) {
    for (const auto& entry : tables.settings(biome).forCategory(category)) {
        if (entry.type != nullptr && entry.type->id().matches(species)) {
            return entry.weight;
        }
    }
    return 0;
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    using namespace mc;

    gameplay::entities::registerBuiltinEntities();

    // --- The built-in table carries 26.1's own numbers, and carries them
    // per biome rather than "every species everywhere at weight 1". ---
    {
        BiomeSpawnTables tables;
        tables.loadBuiltinDefaults();
        // BiomeDefaultFeatures.farmAnimals: pig 10, cow 8.
        REQUIRE(weightOf(tables, Biome::Plains, MobCategory::Creature, "pig") == 10);
        REQUIRE(weightOf(tables, Biome::Plains, MobCategory::Creature, "cow") == 8);
        // BiomeDefaultFeatures.monsters: zombie 95.
        REQUIRE(weightOf(tables, Biome::Plains, MobCategory::Monster, "zombie") == 95);
        // A desert gets no farm animals in vanilla; the old table gave it the
        // same passive density as a plain.
        REQUIRE(tables.settings(Biome::Desert).empty(MobCategory::Creature));
        REQUIRE(tables.settings(Biome::Ocean).empty(MobCategory::Creature));
        // Hostiles reach every biome, water included — vanilla's ocean tables
        // call commonSpawns too.
        REQUIRE(!tables.settings(Biome::Ocean).empty(MobCategory::Monster));
        REQUIRE(!tables.settings(Biome::Desert).empty(MobCategory::Monster));
        // Groups of four, as vanilla's SpawnerData says.
        for (const auto& entry : tables.settings(Biome::Plains).forCategory(
                 MobCategory::Creature)) {
            REQUIRE(entry.minGroup == 4 && entry.maxGroup == 4);
        }
        // Nothing is data-driven yet.
        REQUIRE(!tables.dataDriven(Biome::Plains));
    }

    const fs::path root = fs::temp_directory_path() / "mob_spawn_settings_test";
    fs::remove_all(root);
    const fs::path packRoot = root / "pack";
    fs::create_directories(packRoot);

    // --- A pack with no `data/` at all leaves the built-in table standing.
    // This is the case that matters: it is what every ordinary resource pack
    // looks like, and getting it wrong empties the world of mobs. ---
    {
        BiomeSpawnTables tables;
        assets::StandardPackResourceProvider pack{packRoot};
        tables.load(pack);
        REQUIRE(weightOf(tables, Biome::Plains, MobCategory::Creature, "pig") == 10);
        REQUIRE(!tables.settings(Biome::Plains).empty(MobCategory::Monster));
        REQUIRE(!tables.dataDriven(Biome::Plains));
    }

    // --- A pack that does supply a biome replaces that biome's table
    // wholesale, so removing a species actually removes it — and leaves every
    // other biome on its default. ---
    {
        writeBiome(packRoot, "plains.json", R"({
            "spawners": {
                "creature": [
                    {"type": "minecraft:cow", "weight": 42, "minCount": 2, "maxCount": 5}
                ],
                "monster": []
            }
        })");
        BiomeSpawnTables tables;
        assets::StandardPackResourceProvider pack{packRoot};
        tables.load(pack);
        REQUIRE(tables.dataDriven(Biome::Plains));
        REQUIRE(weightOf(tables, Biome::Plains, MobCategory::Creature, "cow") == 42);
        // The pig the built-in table had is gone, not merged in.
        REQUIRE(weightOf(tables, Biome::Plains, MobCategory::Creature, "pig") == 0);
        // An empty list is a real answer: no monsters here.
        REQUIRE(tables.settings(Biome::Plains).empty(MobCategory::Monster));
        for (const auto& entry : tables.settings(Biome::Plains).forCategory(
                 MobCategory::Creature)) {
            REQUIRE(entry.minGroup == 2 && entry.maxGroup == 5);
        }
        // A biome the pack said nothing about keeps its built-in table.
        REQUIRE(weightOf(tables, Biome::Forest, MobCategory::Creature, "pig") == 10);
        REQUIRE(!tables.dataDriven(Biome::Forest));
    }

    // --- A vanilla biome names dozens of species this build has never heard
    // of. Skipping them is expected; refusing the file is not. ---
    {
        writeBiome(packRoot, "forest.json", R"({
            "spawners": {
                "creature": [
                    {"type": "minecraft:sheep", "weight": 12, "minCount": 4, "maxCount": 4},
                    {"type": "minecraft:pig", "weight": 7, "minCount": 1, "maxCount": 2},
                    {"type": "minecraft:chicken", "weight": 10, "minCount": 4, "maxCount": 4}
                ]
            }
        })");
        BiomeSpawnTables tables;
        assets::StandardPackResourceProvider pack{packRoot};
        tables.load(pack);
        REQUIRE(tables.settings(Biome::Forest).forCategory(MobCategory::Creature).size() == 1U);
        REQUIRE(weightOf(tables, Biome::Forest, MobCategory::Creature, "pig") == 7);
        // The pack said nothing about monsters in this file, so that category
        // keeps its default rather than going silent.
        REQUIRE(!tables.settings(Biome::Forest).empty(MobCategory::Monster));
    }

    // --- A malformed file must not empty the world. ---
    {
        writeBiome(packRoot, "taiga.json", "{ this is not json");
        BiomeSpawnTables tables;
        assets::StandardPackResourceProvider pack{packRoot};
        tables.load(pack);
        REQUIRE(weightOf(tables, Biome::Taiga, MobCategory::Creature, "pig") == 10);
        REQUIRE(!tables.dataDriven(Biome::Taiga));
    }

    // --- Nonsense entries are dropped one by one, not taken on faith: a zero
    // weight would never be picked and an inverted group would spawn nothing. ---
    {
        writeBiome(packRoot, "swamp.json", R"({
            "spawners": {
                "creature": [
                    {"type": "minecraft:pig", "weight": 0, "minCount": 4, "maxCount": 4},
                    {"type": "minecraft:cow", "weight": 5, "minCount": 9, "maxCount": 2}
                ]
            }
        })");
        BiomeSpawnTables tables;
        assets::StandardPackResourceProvider pack{packRoot};
        tables.load(pack);
        REQUIRE(tables.settings(Biome::Swamp).empty(MobCategory::Creature));
    }

    fs::remove_all(root);
    return 0;
}

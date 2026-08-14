#include "gameplay/MobSpawnSettings.hpp"

#include "core/Json.hpp"
#include "gameplay/entities/EntityRegistry.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <string>

namespace mc::gameplay {
namespace {

using world::gen::Biome;

constexpr std::size_t kBiomeCount = static_cast<std::size_t>(Biome::Count);

// The biome ids vanilla names these by. Two of ours have no exact vanilla
// counterpart under the same name (`mountains` is `windswept_hills` since 1.18);
// the id here is what a 26.1 pack would ship, so an override actually matches.
constexpr std::array<std::string_view, kBiomeCount> kBiomeIds{
    "worldgen/biome/ocean.json",
    "worldgen/biome/beach.json",
    "worldgen/biome/plains.json",
    "worldgen/biome/forest.json",
    "worldgen/biome/birch_forest.json",
    "worldgen/biome/taiga.json",
    "worldgen/biome/snowy_plains.json",
    "worldgen/biome/desert.json",
    "worldgen/biome/savanna.json",
    "worldgen/biome/jungle.json",
    "worldgen/biome/dark_forest.json",
    "worldgen/biome/swamp.json",
    "worldgen/biome/windswept_hills.json",
    "worldgen/biome/river.json",
    "worldgen/biome/deep_ocean.json",
};

// The category names vanilla writes inside `spawners`.
[[nodiscard]] std::string_view categoryId(entities::MobCategory category) {
    switch (category) {
    case entities::MobCategory::Creature:
        return "creature";
    case entities::MobCategory::Monster:
        return "monster";
    case entities::MobCategory::Ambient:
        return "ambient";
    case entities::MobCategory::WaterCreature:
        return "water_creature";
    case entities::MobCategory::Misc:
        break;
    }
    return "misc";
}

// BiomeDefaultFeatures.farmAnimals: pig 10, cow 8, in groups of four. Sheep and
// chicken carry the other two weights vanilla lists; they do not exist here, so
// the two that do keep their real numbers rather than being renormalised — a
// pig stays 10/8 more likely than a cow, which is the ratio that matters.
[[nodiscard]] bool hasFarmAnimals(Biome biome) {
    switch (biome) {
    case Biome::Plains:
    case Biome::Forest:
    case Biome::BirchForest:
    case Biome::Taiga:
    case Biome::Savanna:
    case Biome::DarkForest:
    case Biome::Swamp:
    case Biome::Mountains:
        return true;
    // Vanilla gives none of these farm animals: a desert has no grass to graze,
    // a jungle has its own list, the snowy plains only get polar bears and
    // rabbits, and water is water.
    case Biome::Ocean:
    case Biome::DeepOcean:
    case Biome::River:
    case Biome::Beach:
    case Biome::Desert:
    case Biome::Jungle:
    case Biome::SnowyTundra:
    case Biome::Count:
        break;
    }
    return false;
}

// BiomeDefaultFeatures.monsters: hostiles spawn everywhere, including over
// water — vanilla's ocean tables call commonSpawns too.
[[nodiscard]] bool hasMonsters(Biome biome) {
    return biome != Biome::Count;
}

// A biome file writes vanilla ids (`minecraft:cow`), and this game's species
// are registered under its own namespace with the vanilla name kept alongside
// — which is exactly what `vanillaId` is for. Matching only the native id would
// resolve nothing at all in a real pack, and the table would silently keep its
// built-in contents while reporting itself data-driven.
[[nodiscard]] const entities::EntityType* speciesById(std::string_view id) {
    for (const auto* type : entities::entityTypeRegistry().all()) {
        if (type != nullptr && (type->id().matches(id) || type->vanillaId().matches(id))) {
            return type;
        }
    }
    return nullptr;
}

} // namespace

std::string_view biomeDataPath(Biome biome) {
    const auto index = static_cast<std::size_t>(biome);
    return index < kBiomeIds.size() ? kBiomeIds[index] : std::string_view{};
}

bool BiomeSpawnTables::anySpecies(entities::MobCategory category) const {
    return std::any_of(settings_.begin(), settings_.end(),
                       [category](const MobSpawnSettings& settings) {
                           return !settings.empty(category);
                       });
}

void BiomeSpawnTables::set(
    Biome biome,
    entities::MobCategory category,
    std::vector<SpawnerData> entries) {
    settings_[static_cast<std::size_t>(biome)]
        .byCategory[static_cast<std::size_t>(category)] = std::move(entries);
}

void BiomeSpawnTables::loadBuiltinDefaults() {
    settings_ = {};
    dataDriven_ = {};
    const auto* pig = speciesById("pig");
    const auto* cow = speciesById("cow");
    const auto* zombie = speciesById("zombie");
    // Resolving none of them means the entity registry was still empty when
    // this ran, and the result is a world that silently never spawns anything.
    // It has to be loud: the tables look fine, every call succeeds, and the only
    // symptom is an empty world hours later.
    if (pig == nullptr && cow == nullptr && zombie == nullptr) {
        std::cerr << "[spawn-tables] built the biome spawn tables before any species was "
                     "registered; no mob will ever spawn\n";
    }
    for (std::size_t index = 0; index < kBiomeCount; ++index) {
        const auto biome = static_cast<Biome>(index);
        if (hasFarmAnimals(biome)) {
            std::vector<SpawnerData> creatures;
            if (pig != nullptr) {
                creatures.push_back({pig, 10, 4, 4});
            }
            if (cow != nullptr) {
                creatures.push_back({cow, 8, 4, 4});
            }
            set(biome, entities::MobCategory::Creature, std::move(creatures));
        }
        if (hasMonsters(biome) && zombie != nullptr) {
            set(biome, entities::MobCategory::Monster, {{zombie, 95, 4, 4}});
        }
    }
}

void BiomeSpawnTables::load(const assets::ResourceProvider& resources) {
    // Start from the built-ins, so an installation whose pack carries only
    // `assets/` — which is every ordinary resource pack — still spawns mobs.
    loadBuiltinDefaults();
    for (std::size_t index = 0; index < kBiomeCount; ++index) {
        const auto biome = static_cast<Biome>(index);
        const auto location = assets::data(std::string{biomeDataPath(biome)});
        if (!resources.exists(location)) {
            continue; // no pack supplies this biome: keep the built-in table
        }
        core::Json document;
        try {
            const auto bytes = resources.readBytes(location);
            document = core::Json::parse(
                std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        } catch (const std::exception&) {
            continue; // a malformed biome file must not empty the world
        }
        const auto& spawners = document["spawners"];
        if (!spawners.isObject()) {
            continue;
        }
        // A supplied biome replaces its built-in table wholesale, the way a data
        // pack's biome replaces vanilla's, so a pack that removes zombies from
        // a biome actually removes them.
        bool supplied = false;
        for (const auto category :
             {entities::MobCategory::Creature, entities::MobCategory::Monster,
              entities::MobCategory::Ambient, entities::MobCategory::WaterCreature}) {
            const auto& list = spawners[categoryId(category)];
            if (!list.isArray()) {
                continue;
            }
            supplied = true;
            std::vector<SpawnerData> entries;
            entries.reserve(list.size());
            for (std::size_t entry = 0; entry < list.size(); ++entry) {
                const auto& record = list[entry];
                const auto* type = speciesById(record["type"].asString());
                if (type == nullptr) {
                    // A vanilla biome names dozens of species this build has
                    // never heard of. That is expected, not an error.
                    continue;
                }
                SpawnerData data;
                data.type = type;
                data.weight = static_cast<int>(record["weight"].asNumber(1.0));
                data.minGroup = static_cast<int>(record["minCount"].asNumber(1.0));
                data.maxGroup = static_cast<int>(record["maxCount"].asNumber(
                    static_cast<double>(data.minGroup)));
                if (data.weight <= 0 || data.minGroup <= 0 || data.maxGroup < data.minGroup) {
                    continue;
                }
                entries.push_back(data);
            }
            set(biome, category, std::move(entries));
        }
        if (supplied) {
            dataDriven_[index] = true;
        }
    }
}

BiomeSpawnTables& biomeSpawnTables() {
    // The built-ins name species through the entity registry, so the first
    // touch has to come after registerBuiltinEntities(). Application's load()
    // is that first touch; a spawner only ever copies from here.
    static BiomeSpawnTables tables = [] {
        BiomeSpawnTables defaults;
        defaults.loadBuiltinDefaults();
        return defaults;
    }();
    return tables;
}

} // namespace mc::gameplay

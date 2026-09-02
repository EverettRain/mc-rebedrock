#include "gameplay/MobSpawnSettings.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/gen/Biome.hpp"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

// BM-0: biome identity is written down once, in 26.1's own vocabulary, and every
// consumer derives from it.
//
// Two ids used to disagree with themselves: the registry said `snowy_tundra` and
// `mountains` (1.16 names vanilla renamed in 1.18) while a second, hand-copied
// list inside MobSpawnSettings said `snowy_plains` and `windswept_hills`. A data
// pack could match one and not the other, and `biomeFromIdentifier` — which had
// no production caller at all — answered "unknown biome" for the id a 26.1 pack
// actually ships.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"biome_identity_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using mc::world::gen::Biome;
using mc::world::gen::biomeDefinition;
using mc::world::gen::biomeFromIdentifier;

constexpr std::size_t kBiomeCount = static_cast<std::size_t>(Biome::Count);

// The 26.1 registry id of every biome this build carries, in enum order. Each one
// was checked against `Biomes.java` in the 26.1 source tree; this array is the
// contract, so a rename that lands here without landing there fails the build's
// own idea of what vanilla calls things.
constexpr std::array<std::string_view, kBiomeCount> kAuthoritativeIds{
    "ocean",         "beach",           "plains",          "forest",
    "birch_forest",  "taiga",           "snowy_plains",    "desert",
    "savanna",       "jungle",          "dark_forest",     "swamp",
    "windswept_hills", "river",         "deep_ocean",      "nether_wastes",
    "soul_sand_valley", "crimson_forest", "warped_forest", "basalt_deltas",
    "the_end",       "end_highlands",   "end_midlands",    "end_barrens",
    "small_end_islands",
};

// Enum ordinals the biome map depends on. Renaming a member must not move it:
// LayeredBiomeSource maps its own 1.16 numeric ids onto these, so a reordering
// silently repaints the world while every test that only checks names still
// passes.
void checkOrdinalsUnmoved() {
    REQUIRE(static_cast<int>(Biome::Plains) == 2);
    REQUIRE(static_cast<int>(Biome::SnowyPlains) == 6);
    REQUIRE(static_cast<int>(Biome::WindsweptHills) == 12);
    REQUIRE(static_cast<int>(Biome::DeepOcean) == 14);
    REQUIRE(static_cast<int>(Biome::NetherWastes) == 15);
    REQUIRE(static_cast<int>(Biome::TheEnd) == 20);
    REQUIRE(kBiomeCount == 25U);
}

void checkAuthoritativeIds() {
    for (std::size_t index = 0; index < kBiomeCount; ++index) {
        const auto biome = static_cast<Biome>(index);
        const auto& definition = biomeDefinition(biome);
        REQUIRE(definition.biome == biome);
        REQUIRE(definition.identifier == kAuthoritativeIds[index]);

        // Bare, `minecraft:` and `rebedrock:` all resolve to the same biome —
        // the JC import anchor, mirroring blockFromIdentifier.
        const std::string bare{definition.identifier};
        REQUIRE(biomeFromIdentifier(bare) == biome);
        REQUIRE(biomeFromIdentifier("minecraft:" + bare) == biome);
        REQUIRE(biomeFromIdentifier("rebedrock:" + bare) == biome);
    }
}

// The two 1.16 ids vanilla has since renamed still resolve, so a save or pack
// written against the old name is read rather than dropped.
void checkLegacyAliases() {
    REQUIRE(biomeFromIdentifier("snowy_tundra") == Biome::SnowyPlains);
    REQUIRE(biomeFromIdentifier("minecraft:snowy_tundra") == Biome::SnowyPlains);
    REQUIRE(biomeFromIdentifier("mountains") == Biome::WindsweptHills);
    REQUIRE(biomeFromIdentifier("minecraft:mountains") == Biome::WindsweptHills);
    // An alias is not an identity: the biome still calls itself by its 26.1 id.
    REQUIRE(biomeDefinition(Biome::SnowyPlains).identifier == "snowy_plains");
    REQUIRE(biomeDefinition(Biome::WindsweptHills).identifier == "windswept_hills");

    REQUIRE(biomeFromIdentifier("minecraft:not_a_biome") == Biome::Count);
    REQUIRE(biomeFromIdentifier("") == Biome::Count);
}

// One table: the pack path is derived from the biome's id, not written down a
// second time. The nether and end biomes get a real path too — the list that
// used to sit in MobSpawnSettings only covered the fifteen overworld ones, so a
// pack could never override a nether biome's spawns at all.
void checkPackPathIsDerived() {
    for (std::size_t index = 0; index < kBiomeCount; ++index) {
        const auto biome = static_cast<Biome>(index);
        const auto expected =
            "worldgen/biome/" + std::string{biomeDefinition(biome).identifier} + ".json";
        REQUIRE(mc::gameplay::biomeDataPath(biome) == expected);
    }
    REQUIRE(mc::gameplay::biomeDataPath(Biome::SnowyPlains) ==
            "worldgen/biome/snowy_plains.json");
    REQUIRE(mc::gameplay::biomeDataPath(Biome::NetherWastes) ==
            "worldgen/biome/nether_wastes.json");
}

// The nether and the end are not overworld biomes wearing different names. Their
// monster tables are empty because none of their species exist in this build yet
// — an empty list is the honest answer; a plain overworld zombie in the nether
// was the wrong one, and that is what `biome != Count` used to produce.
void checkNetherAndEndSpawnTables() {
    mc::gameplay::entities::registerBuiltinEntities();
    mc::gameplay::BiomeSpawnTables tables;
    tables.loadBuiltinDefaults();

    using mc::gameplay::entities::MobCategory;
    REQUIRE(!tables.settings(Biome::Plains).empty(MobCategory::Monster));
    REQUIRE(!tables.settings(Biome::Ocean).empty(MobCategory::Monster));

    constexpr std::array<Biome, 10> kNonOverworld{
        Biome::NetherWastes, Biome::SoulSandValley, Biome::CrimsonForest,
        Biome::WarpedForest, Biome::BasaltDeltas,   Biome::TheEnd,
        Biome::EndHighlands, Biome::EndMidlands,    Biome::EndBarrens,
        Biome::SmallEndIslands,
    };
    for (const auto biome : kNonOverworld) {
        REQUIRE(tables.settings(biome).empty(MobCategory::Monster));
        REQUIRE(tables.settings(biome).empty(MobCategory::Creature));
    }
}

} // namespace

int main() {
    checkOrdinalsUnmoved();
    checkAuthoritativeIds();
    checkLegacyAliases();
    checkPackPathIsDerived();
    checkNetherAndEndSpawnTables();
    return 0;
}

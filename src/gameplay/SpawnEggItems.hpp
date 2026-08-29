#pragma once

#include "gameplay/ContentRegistry.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/entities/BuiltinSpecies.hpp"
#include "gameplay/entities/EntityType.hpp"

#include <cstddef>
#include <string_view>

namespace mc::gameplay::entities {

// Every species is a manifest row (BuiltinSpecies.cpp), and a row's EntityType
// lives in the shared storage inside registerBuiltinSpeciesManifest(), addressed
// only through the registry — there is no per-species `::type()` accessor to
// take the address of. A SpawnEggItem::EntitySupplier is a plain
// `const EntityType& (*)()`, so the supplier is this template instantiated on the
// species name: one link-time function per egg, no hand-written accessor per
// species, and adding an egg stays a single line below.
//
// The lookup is deferred — it runs when an egg is actually used to spawn or
// drawn to render, never at static-init time — and builtinSpecies() registers
// the manifest first, so it cannot be reached too early. Its abort on an unknown
// name is deliberate and loud: a mistyped species would otherwise spawn nothing
// and say nothing.
template <std::size_t N>
struct SpeciesName final {
    char value[N]{};

    // NOLINTNEXTLINE(google-explicit-constructor) — a template parameter is
    // written as a plain string literal, so this must convert implicitly.
    constexpr SpeciesName(const char (&literal)[N]) {
        for (std::size_t index = 0; index < N; ++index) {
            value[index] = literal[index];
        }
    }

    [[nodiscard]] constexpr std::string_view view() const { return {value, N - 1U}; }
};

template <SpeciesName name>
[[nodiscard]] const EntityType& speciesForSpawnEgg() {
    return builtinSpecies(name.view());
}

} // namespace mc::gameplay::entities

namespace mc::gameplay::items {

// Spawn-egg instances. Each is a SpawnEggItem that knows which entity it spawns;
// the constructor stores the EntityType supplier so the renderer can tint the
// icon and the interaction system can spawn the right creature.
inline constexpr SpawnEggItem PigSpawnEgg{
    "pig_spawn_egg", &entities::speciesForSpawnEgg<"pig">};

inline constexpr SpawnEggItem ZombieSpawnEgg{
    "zombie_spawn_egg", &entities::speciesForSpawnEgg<"zombie">};

inline constexpr SpawnEggItem CowSpawnEgg{
    "cow_spawn_egg", &entities::speciesForSpawnEgg<"cow">};

inline constexpr SpawnEggItem SheepSpawnEgg{
    "sheep_spawn_egg", &entities::speciesForSpawnEgg<"sheep">};

inline constexpr SpawnEggItem ChickenSpawnEgg{
    "chicken_spawn_egg", &entities::speciesForSpawnEgg<"chicken">};

inline constexpr SpawnEggItem HuskSpawnEgg{
    "husk_spawn_egg", &entities::speciesForSpawnEgg<"husk">};

} // namespace mc::gameplay::items

namespace mc::gameplay {

// Spawn-egg registry: one entry per spawn egg, appended after kItemRegistry when
// building the creative catalog or the texture atlas. Defined here because the
// spawn-egg constructors need entity headers that cannot be included from
// Item.hpp (circular dependency through Inventory.hpp → Item.hpp).
inline constexpr std::array<const Item*, 6> kSpawnEggItems{
    &items::PigSpawnEgg,
    &items::ZombieSpawnEgg,
    &items::CowSpawnEgg,
    &items::SheepSpawnEgg,
    &items::ChickenSpawnEgg,
    &items::HuskSpawnEgg,
};

// Push spawn eggs into the runtime-extensible lookup (for itemFromIdentifier,
// /give, and save/load) AND into the creative inventory (for ContentRegistry).
// The lambda fires once on first use of any symbol in this translation unit.
// Cannot be done inside contentRegistry()'s own singleton lambda because the
// spawn-egg constructors need entity headers that would drag entity sources
// into every test linking ContentRegistry.cpp.
inline const bool kSpawnEggItemsRegistered = [] {
    auto& extra = extraItemRegistry();
    for (const Item* item : kSpawnEggItems) {
        extra.push_back(item);
    }
    // Register into the creative inventory. contentRegistry() returns a const ref
    // but this is init time — the cast is safe.
    auto& registry = const_cast<ContentRegistry&>(contentRegistry());
    for (const Item* item : kSpawnEggItems) {
        registry.registerItem(item, item->creativeCategory);
    }
    return true;
}();

} // namespace mc::gameplay

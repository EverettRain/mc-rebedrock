#pragma once

#include "gameplay/ContentRegistry.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/entities/CowEntity.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "gameplay/entities/PigEntity.hpp"
#include "gameplay/entities/ZombieEntity.hpp"

#include <cassert>

namespace mc::gameplay::entities {

// Sheep and chicken are E3 manifest species (BuiltinSpecies.cpp), not
// hand-written classes with their own `::type()` accessor — a manifest row's
// EntityType lives in the shared `storage` deque inside
// registerBuiltinSpeciesManifest(), addressed only through the registry. A
// SpawnEggItem::EntitySupplier is a plain `const EntityType& (*)()`, so these
// two functions give the manifest species that same call shape: a deferred
// byId() lookup, resolved lazily (never at static-init time, only when a
// spawn egg is actually used to spawn or drawn to render), by which point
// registerBuiltinEntities() has already run registerBuiltinSpeciesManifest().
// The abort on a missing id is deliberate and loud, the same posture
// BiomeSpawnTables::loadBuiltinDefaults takes for a build with no species
// registered yet: silently returning a placeholder would let a miscabled
// manifest row (or a call before registration) spawn nothing and say nothing.
[[nodiscard]] inline const EntityType& sheepTypeForSpawnEgg() {
    const EntityType* type = entityTypeRegistry().byId("sheep");
    assert(type != nullptr && "sheep spawn egg used before the species manifest registered");
    return *type;
}

[[nodiscard]] inline const EntityType& chickenTypeForSpawnEgg() {
    const EntityType* type = entityTypeRegistry().byId("chicken");
    assert(type != nullptr && "chicken spawn egg used before the species manifest registered");
    return *type;
}

} // namespace mc::gameplay::entities

namespace mc::gameplay::items {

// Spawn-egg instances. Each is a SpawnEggItem that knows which entity it spawns;
// the constructor stores the EntityType supplier so the renderer can tint the
// icon and the interaction system can spawn the right creature.
inline constexpr SpawnEggItem PigSpawnEgg{
    "pig_spawn_egg", &entities::PigEntity::type};

inline constexpr SpawnEggItem ZombieSpawnEgg{
    "zombie_spawn_egg", &entities::ZombieEntity::type};

inline constexpr SpawnEggItem CowSpawnEgg{
    "cow_spawn_egg", &entities::CowEntity::type};

// AR-A1: sheep and chicken are manifest species (BuiltinSpecies.cpp), so their
// supplier is the deferred byId() lookup above rather than a `::type()`
// static-storage accessor.
inline constexpr SpawnEggItem SheepSpawnEgg{
    "sheep_spawn_egg", &entities::sheepTypeForSpawnEgg};

inline constexpr SpawnEggItem ChickenSpawnEgg{
    "chicken_spawn_egg", &entities::chickenTypeForSpawnEgg};

} // namespace mc::gameplay::items

namespace mc::gameplay {

// Spawn-egg registry: one entry per spawn egg, appended after kItemRegistry when
// building the creative catalog or the texture atlas. Defined here because the
// spawn-egg constructors need entity headers that cannot be included from
// Item.hpp (circular dependency through Inventory.hpp → Item.hpp).
inline constexpr std::array<const Item*, 5> kSpawnEggItems{
    &items::PigSpawnEgg,
    &items::ZombieSpawnEgg,
    &items::CowSpawnEgg,
    &items::SheepSpawnEgg,
    &items::ChickenSpawnEgg,
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

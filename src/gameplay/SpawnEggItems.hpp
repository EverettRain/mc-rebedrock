#pragma once

#include "gameplay/ContentRegistry.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/entities/CowEntity.hpp"
#include "gameplay/entities/PigEntity.hpp"
#include "gameplay/entities/ZombieEntity.hpp"

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

} // namespace mc::gameplay::items

namespace mc::gameplay {

// Spawn-egg registry: one entry per spawn egg, appended after kItemRegistry when
// building the creative catalog or the texture atlas. Defined here because the
// spawn-egg constructors need entity headers that cannot be included from
// Item.hpp (circular dependency through Inventory.hpp → Item.hpp).
inline constexpr std::array<const Item*, 3> kSpawnEggItems{
    &items::PigSpawnEgg,
    &items::ZombieSpawnEgg,
    &items::CowSpawnEgg,
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

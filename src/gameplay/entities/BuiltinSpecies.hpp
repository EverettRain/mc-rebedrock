#pragma once

// The batch-import path for species (E3): one table row per creature instead of
// one C++ class plus a hand-written registration line. Once E1 gave every
// species a registry identity and E2 moved its numbers into data, "add an
// animal" collapses to "add a manifest row" — a value carrying the numbers, an
// AI reference (a shared behaviour instance, or a new EntityAi only when the
// creature does something genuinely new), a render descriptor and an optional
// loot roll. Registration walks the table, so nothing dispatches by species.
//
// The three original creatures (Pig/Cow/Zombie) keep their hand-written classes
// — their type() accessors are referenced widely — and register the same way
// they always have; the manifest is how *new* species are added. Adding one
// touches this table and nothing else: no switch to extend, no accessor to
// declare.

#include "audio/MobSoundProfile.hpp"
#include "gameplay/entities/EntityType.hpp"

#include <span>
#include <string_view>

namespace mc::gameplay::entities {

// One creature, entirely as data + references. Everything EntityType::Builder
// needs, so registration is a mechanical translation with no per-species code.
struct SpeciesDef final {
    std::string_view path;        // this project's id path, e.g. "chicken"
    std::string_view vanillaName; // the minecraft: alias path, e.g. "chicken"
    MobCategory category;
    SpawnPlacement placement = SpawnPlacement::OnGround;
    EntityDimensions dimensions;
    EntityAttributes attributes;
    bool hasSpawnEgg = false;
    SpawnEggColors spawnEgg;
    EntityRenderDescriptor render;
    audio::MobSoundProfile sounds;
    // The shared behaviour this species runs. A passive animal points at an
    // AnimalAi, a melee hostile at a MonsterAi variant; a creature with new
    // behaviour points at a new EntityAi written for it.
    const EntityAi* ai = nullptr;
    // The death-drop roll, or null for a creature whose drop needs an item this
    // build does not have yet (the zombie's rotten flesh, say).
    LootRoll loot = nullptr;
};

// The built-in new-species table. Dropping a row removes that species from the
// build entirely — byId stops resolving it — which is exactly the guarantee the
// manifest is meant to give.
[[nodiscard]] std::span<const SpeciesDef> builtinSpeciesManifest();

// Builds every manifest row through the Builder and registers it (Bootstrap
// phase), keeping the built EntityTypes at stable addresses for the run. Called
// once by registerBuiltinEntities() after the hand-written species.
void registerBuiltinSpeciesManifest();

} // namespace mc::gameplay::entities

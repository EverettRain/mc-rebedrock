#pragma once

// The ONE way a species is defined (E3): one table row per creature. Once E1
// gave every species a registry identity and E2 moved its numbers into data,
// "add an animal" collapsed to "add a manifest row" — a value carrying the
// numbers, an AI reference (a shared behaviour instance, or a new EntityAi only
// when the creature does something genuinely new), a render descriptor and an
// optional loot roll. Registration walks the table, so nothing dispatches by
// species.
//
// The three original creatures (Pig/Cow/Zombie) used to be hand-written classes
// with their own `::type()` accessors, kept alive purely because callers held
// those accessors. They are manifest rows like everything else now, in their
// original registration order so the dense Bootstrap ids are unchanged. There is
// no second way to define a species: a caller that needs one by name asks
// builtinSpecies() below.

#include "audio/MobSoundProfile.hpp"
#include "gameplay/entities/EntityType.hpp"

#include <cstdint>
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
    // AR-A2: EM-3's breeding parameters (tempt item + baby scale), stated as
    // data exactly like every other manifest row field. Default (breedable ==
    // false) costs a non-ageable species nothing — the same "content states
    // parameters, EM-3 owns the mechanism" rule BreedingProfile documents.
    BreedingProfile breeding{};
    // AR-A4: the behaviour bit set (EntityBehavior), OR'd together — e.g. a
    // chicken's fallImmune, a husk's Undead ENCH-1 target-category marker.
    // Default (0) subjects the species to every mechanic. The hand-written
    // classes set these directly on the Builder instead.
    std::uint16_t behaviorFlags = 0U;
    // AR-A4: egg-laying parameters, stated as data exactly like breeding.
    // Default (laysEggs == false) costs a non-laying species nothing.
    EggLayProfile eggLay{};
    // XP-2: the kill experience reward as an inclusive [min, max] range —
    // AnimalEntity's 1..3, or a flat monster reward (min == max, e.g. husk 5).
    // Default (max == 0) means the species never drops experience.
    std::int32_t xpRewardMin = 0;
    std::int32_t xpRewardMax = 0;
};

// The built-in new-species table. Dropping a row removes that species from the
// build entirely — byId stops resolving it — which is exactly the guarantee the
// manifest is meant to give.
[[nodiscard]] std::span<const SpeciesDef> builtinSpeciesManifest();

// Builds every manifest row through the Builder and registers it (Bootstrap
// phase), keeping the built EntityTypes at stable addresses for the run. Called
// once by registerBuiltinEntities(); idempotent, so calling it again is free.
void registerBuiltinSpeciesManifest();

// The registered EntityType for a built-in species path ("pig", "husk", …).
//
// This is how a caller names a species. A manifest row's EntityType lives in the
// shared storage inside registerBuiltinSpeciesManifest() and is addressed only
// through the registry, so there is no per-species accessor to hold — and adding
// a species stays a one-row edit. Registration runs first (idempotent), so this
// is safe to call before anything else has touched the registry.
//
// Aborts loudly on a path no manifest row declares: silently handing back a
// placeholder would let a mistyped id spawn nothing and say nothing. Use
// entityTypeRegistry().byId() instead when a miss is a legitimate outcome (a
// save or peer naming a species this build does not have).
[[nodiscard]] const EntityType& builtinSpecies(std::string_view path);

} // namespace mc::gameplay::entities

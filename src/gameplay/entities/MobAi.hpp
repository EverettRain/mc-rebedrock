#pragma once

#include "gameplay/entities/EntityType.hpp"

namespace mc::gameplay::entities {

// Two full turns in radians: the shared wander-heading scale every category AI
// below rolls a fresh direction on. One definition here instead of one per
// species.
inline constexpr float kTwoPi = 6.28318530718F;

// AnimalAi — the shared AI base for MobCategory::Creature species (vanilla
// Animal). A passive land animal grazes: after a longish pause it either keeps
// standing in place (one time in four) or ambles off on a fresh random heading,
// the WanderAroundFarGoal cadence the pig already uses. Species subclass this
// for anything extra (breeding, fleeing); wandering is already the animal's
// default. Category-neutral behaviour like anger lives on NeutralAi instead.
class AnimalAi : public EntityAi {
  public:
    // MobEntity#initGoals's animal grazing cadence, shared by every CREATURE.
    void chooseWanderIntent(SimpleEntity& self, std::uint32_t& rng) const override;
};

// MonsterAi — the shared AI base for MobCategory::Monster species (vanilla
// Monster). A hostile mob shambles almost constantly, pausing far less than a
// grazing animal. Target/attack goals wait on player-targeting AI; until then a
// monster is honestly just a persistent wanderer. Species subclass this for
// their own combat goals once targeting exists.
class MonsterAi : public EntityAi {
  public:
    // The persistent hostile shambling shared by every MONSTER.
    void chooseWanderIntent(SimpleEntity& self, std::uint32_t& rng) const override;
};

// AmbientAi — placeholder for MobCategory::Ambient (vanilla Ambient: bats).
// Nothing is registered yet, so it stays an abstract base: a future bat species
// subclasses it and supplies the flutter. Exists to keep category -> AI 1:1.
class AmbientAi : public EntityAi {
};

// WaterCreatureAi — placeholder for MobCategory::WaterCreature (vanilla
// WaterCreature: squid, dolphins). No species is registered yet; a future one
// subclasses it once water movement exists.
class WaterCreatureAi : public EntityAi {
};

// MiscAi — placeholder for MobCategory::Misc. Vanilla Misc entities
// (projectiles, item frames) are not mobs and have no AI at all; the class
// exists only so every MobCategory has an AI base to point at.
class MiscAi : public EntityAi {
};

} // namespace mc::gameplay::entities

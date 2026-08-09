#pragma once

#include "gameplay/entities/EntityType.hpp"

namespace mc::gameplay::entities {

// AnimalAi — the shared AI base for MobCategory::Creature species (vanilla
// Animal). Each spawn receives independent Swim, EscapeDanger,
// WanderAroundFar, LookAtPlayer and LookAround goal instances. Species subclass
// this profile to insert mating/temptation/follow-parent goals later without
// changing the shared selector or navigation runtime.
class AnimalAi : public EntityAi {
  public:
    explicit AnimalAi(float escapeSpeedMultiplier = 1.25F, float wanderSpeedMultiplier = 1.0F,
                      int passiveGoalPriorityOffset = 0)
        : escapeSpeedMultiplier_(escapeSpeedMultiplier),
          wanderSpeedMultiplier_(wanderSpeedMultiplier),
          passiveGoalPriorityOffset_(passiveGoalPriorityOffset) {}

    void configureBrain(MobBrain& brain) const override;

  private:
    float escapeSpeedMultiplier_ = 1.25F;
    float wanderSpeedMultiplier_ = 1.0F;
    int passiveGoalPriorityOffset_ = 0;
};

// MonsterAi — the shared idle AI base for MobCategory::Monster species
// (vanilla Monster). It supplies the low-priority wander/look fallback; concrete
// monsters subclass it and install their own target and combat goals, as the
// zombie does for player acquisition and melee.
class MonsterAi : public EntityAi {
  public:
    void configureBrain(MobBrain& brain) const override;
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

#pragma once

#include "gameplay/entities/EntityType.hpp"

#include <cstdint>

namespace mc::gameplay::entities {

// Angerable (1.16.1): the framework for neutral mobs — species that are calm
// until provoked, then turn hostile toward whoever hit them for a random spell
// (wolves, endermen, zombified piglins). Neutrality is a *behaviour*, orthogonal
// to MobCategory: a neutral wolf is still a CREATURE, an angry zombified piglin
// still a MONSTER. So it lives on the AI, not the category.
//
// This is the framework only — no built-in creature is neutral yet. A concrete
// neutral species would subclass NeutralAi, keep chooseWanderIntent for its calm
// roaming, and (once target/attack goals exist) act on the anger timer the base
// maintains. The anger state itself is the `angerTicks` field on SimpleEntity,
// which EntitySystem already counts down each tick.
class NeutralAi : public EntityAi {
  public:
    // Angerable#chooseRandomAngerTime: a provoked mob stays angry 20–39 seconds
    // (400–799 ticks). Overridable so a species can widen or narrow the window.
    [[nodiscard]] virtual int chooseAngerTime(std::uint32_t& rng) const;

    // Angerable#setTarget via LivingEntity#damage: being hit starts (or refreshes)
    // the anger timer. A concrete neutral mob's target/pursuit goals would read
    // SimpleEntity::angry() to switch from wandering to chasing.
    void onAttacked(SimpleEntity& self, std::uint32_t& rng) const override;

    // chooseWanderIntent stays pure virtual: a neutral species still has to say
    // how it roams while calm, exactly like any other creature.
};

} // namespace mc::gameplay::entities

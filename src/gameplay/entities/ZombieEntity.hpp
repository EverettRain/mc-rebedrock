#pragma once

#include "gameplay/entities/EntityType.hpp"

namespace mc::gameplay::entities {

// ZombieEntity (1.16.1): a MONSTER-category hostile mob, ported to exercise the
// registration path a hostile creature takes — MobCategory::Monster, the taller
// 0.6 x 1.95 box, the zombie attribute block and its spawn-egg tint. Like every
// species its whole definition lives in ZombieEntity.cpp; registering it changes
// nothing in the shared simulation, which reacts to its category on its own
// (e.g. it despawns in Peaceful because MONSTER is disallowed there).
struct ZombieEntity final {
    [[nodiscard]] static const EntityType& type();
};

} // namespace mc::gameplay::entities

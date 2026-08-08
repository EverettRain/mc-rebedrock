#pragma once

#include "gameplay/entities/EntityType.hpp"

namespace mc::gameplay::entities {

// CowEntity (1.16.1): a passive Creature that wanders and drops raw beef and
// leather. Its EntityType is a stable singleton — the analogue of
// `EntityType<CowEntity> COW` — so any caller can hold `&CowEntity::type()` and
// compare it by address. The whole species (hitbox, attributes, spawn-egg tint,
// AI, loot, render assets) is defined in CowEntity.cpp; nothing about a cow
// leaks into the shared simulation or renderer.
struct CowEntity final {
    // Builds and registers the type on first call, then returns the same object
    // every time. Registration is idempotent, so this doubles as the handle.
    [[nodiscard]] static const EntityType& type();
};

} // namespace mc::gameplay::entities

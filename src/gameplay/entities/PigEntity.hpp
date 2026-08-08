#pragma once

#include "gameplay/entities/EntityType.hpp"

namespace mc::gameplay::entities {

// PigEntity (1.16.1): a passive Creature that wanders and drops porkchops. Its
// EntityType is a stable singleton — the analogue of `EntityType<PigEntity> PIG`
// — so any caller can hold `&PigEntity::type()` and compare it by address. The
// whole species (hitbox, attributes, spawn-egg tint, AI, loot, render assets)
// is defined in PigEntity.cpp; nothing about a pig leaks into the shared
// simulation or renderer.
struct PigEntity final {
    // Builds and registers the type on first call, then returns the same object
    // every time. Registration is idempotent, so this doubles as the handle.
    [[nodiscard]] static const EntityType& type();
};

} // namespace mc::gameplay::entities

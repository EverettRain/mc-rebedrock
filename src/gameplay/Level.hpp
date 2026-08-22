#pragma once

#include "gameplay/EntitySystem.hpp"
#include "gameplay/ItemEntitySystem.hpp"
#include "gameplay/NaturalSpawner.hpp"
#include "gameplay/WeatherSystem.hpp"
#include "world/Dimension.hpp"

#include <cstdint>

namespace mc::world {
class World;
} // namespace mc::world

namespace mc::gameplay {

// One dimension's per-world simulation bundle, mirroring 26.1's ServerLevel: the
// creatures, the dropped items, the weather and the natural spawner that all
// belong to a single dimension. A value-member struct, not a heap object graph —
// no vtable, no per-dimension `unique_ptr` tree — so a table of these is a flat
// array the tick walks by subscript (DIM DESIGN §3). This is the "singularity
// hoist" of DIM-1: what GameSession used to own as one exclusive set of
// singletons now lives here, one Level per DimensionId, indexed by that id.
//
// The World itself stays owned by GameRuntime for now (its lifetime is braided
// through the world lock, the chunk streamer and the background unload-persist
// worker); a Level therefore *references* its world rather than containing it.
// bindWorld wires that reference at construction of the level table; every
// per-dimension system reaches its blocks through world(). Moving the World
// value into Level is a later slice — DIM-1's mandate is structural and
// behaviour-equivalent, and relocating World's entangled ownership would put the
// equivalence guarantee at risk for no DIM-1 benefit (single dimension active).
//
// The clock is deliberately *not* a Level member: ClockManager is already
// multi-clock (one ClockId per dimension), so it stays world-level in
// GameSession and a Level names its clock through clockOf(id) — DIM DESIGN §4.2.
struct Level final {
    world::DimensionId id = world::DimensionId::Overworld;

    // The per-dimension simulation systems, previously GameSession's exclusive
    // singletons. Value members: default-constructed, moved wholesale into the
    // level table, never separately heap-allocated.
    EntitySystem entities;
    ItemEntitySystem items;
    WeatherSystem weather;
    NaturalSpawner spawner{0U};

    // Points the level at the World that GameRuntime owns for this dimension.
    // Called once when the level table is set up; a level whose world is never
    // bound is a programming error the world() accessor asserts against.
    void bindWorld(world::World& world) { world_ = &world; }

    [[nodiscard]] world::World& world() {
        return *world_;  // bound at level-table setup; null here is a wiring bug
    }
    [[nodiscard]] const world::World& world() const { return *world_; }
    [[nodiscard]] bool hasWorld() const { return world_ != nullptr; }

    // This dimension's clock id (index-aligned with DimensionId), so a
    // per-dimension tick reaches its clock through the world-level ClockManager
    // by a subscript rather than a lookup.
    [[nodiscard]] world::ClockId clockId() const { return world::clockOf(id); }

  private:
    world::World* world_ = nullptr;
};

} // namespace mc::gameplay

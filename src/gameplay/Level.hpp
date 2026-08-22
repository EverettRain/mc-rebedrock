#pragma once

#include "gameplay/Difficulty.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/ItemEntitySystem.hpp"
#include "gameplay/NaturalSpawner.hpp"
#include "gameplay/WeatherSystem.hpp"
#include "world/Dimension.hpp"
#include "world/DimensionGenerator.hpp"
#include "world/World.hpp"

#include <cstddef>
#include <cstdint>

namespace mc::gameplay {

// What one secondary-Level tick did, for metering and the DIM-2 skip assertions.
// A dimension with no loaded chunks costs `skippedEmpty == true` and touches
// nothing — the "empty dimension is free" invariant made observable.
struct LevelTickReport final {
    bool skippedEmpty = false;      // no bound world or no loaded chunks -> did nothing
    std::size_t chunksResident = 0; // chunks the level held this tick (0 when skipped)
    std::size_t creaturesTicked = 0;
};

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

    // DIM-3 per-dimension streaming/generation seam ---------------------------
    //
    // Whether the player is present in this dimension. Idle dimensions (no player
    // and no forced load) do not stream — the streaming complement of DIM-2's
    // "empty dimension is free": a dimension nobody is in generates nothing.
    bool hasPlayer = false;

    // This dimension's derived terrain seed. Set at bind time from the world seed
    // and the DimensionId, so the Nether's noise is never the Overworld's.
    std::uint64_t generationSeed = 0U;

    // The generation config (bounds + generator seam) for this dimension, read
    // from the DimensionType. A generator reads height/ceiling from here, never a
    // hardcoded 256.
    [[nodiscard]] world::DimensionGeneratorConfig generatorConfig() const {
        return world::dimensionGeneratorConfig(id);
    }

    // Whether this dimension should stream chunks this tick: it needs a bound
    // world, a real terrain generator (the Nether/End seam is not yet filled by
    // worldgen), and a reason to be active (a player is in it). An idle or
    // generator-less dimension requests nothing.
    [[nodiscard]] bool shouldStream() const {
        return world_ != nullptr && hasPlayer && generatorConfig().hasTerrainGenerator;
    }

    // True when this dimension is dormant: no world bound, or a world with no
    // loaded chunks. This is the "empty dimension is free" test — DIM DESIGN §2:
    // a dimension nobody is in has an empty chunk map, and that emptiness is one
    // hash-size check, no ticket-system object graph. JE's DistanceManager exists
    // to reach the same answer; here it is `chunks_.empty()`.
    [[nodiscard]] bool isDormant() const {
        return world_ == nullptr || world_->chunkCount() == 0;
    }

    // Advances this dimension's passive simulation one 20 TPS tick — the parts
    // that do not need the player: its weather and its creatures against its own
    // world. The primary level (the one the player is in) is NOT ticked through
    // here; GameSession ticks it in full with all the player-coupled systems.
    // This is the per-ServerLevel body of DIM-2's cross-dimension loop, run only
    // for the secondary (playerless) dimensions.
    //
    // A dormant dimension returns immediately having touched nothing: no chunk is
    // read, no entity vector is walked beyond its empty size. That is the whole
    // point — a Nether nobody has entered costs a branch, not a tick.
    LevelTickReport tickPassive(bool doWeatherCycle, Difficulty difficulty) {
        LevelTickReport report;
        if (isDormant()) {
            report.skippedEmpty = true;
            return report;
        }
        report.chunksResident = world_->chunkCount();
        // Non-natural dimensions (Nether/End) have no weather cycle; the
        // WeatherSystem still exists but its auto-cycle is gated off.
        weather.tick(doWeatherCycle && world::dimensionType(id).natural);
        // Playerless: the default pusher sits far below the world, so no creature
        // is shoved by an absent player, and simulationRadius 0 means every
        // creature ticks (there is no player to measure distance from). The
        // difficulty still drives the peaceful-despawn pass. Items and the natural
        // spawner both need a player, so a secondary level does not tick them yet
        // (DIM-5 gives non-primary dimensions a player when one transfers in).
        const auto entityTick = entities.tick(
            *world_, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F, difficulty,
            /*playerAlive=*/false, /*playerCreative=*/false, /*simulationRadius=*/0.0F,
            weather.isRaining());
        static_cast<void>(entityTick);
        report.creaturesTicked = entities.entities().size();
        return report;
    }

  private:
    world::World* world_ = nullptr;
};

} // namespace mc::gameplay

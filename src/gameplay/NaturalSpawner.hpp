#pragma once

#include "gameplay/Difficulty.hpp"
#include "gameplay/EnvironmentSnapshot.hpp"
#include "gameplay/MobSpawnSettings.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "world/gen/Biome.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace mc::world {
class World;
}

namespace mc::gameplay {

class EntitySystem;

// NaturalSpawner (26.1's `world/level/NaturalSpawner`): every spawn interval it
// runs the spawn rule chain once per naturally-spawning category, over columns
// picked at random inside the simulation radius.
//
// The position sample is vanilla's `getRandomPosWithin`: a random column, then
// a random *height* inside it, which the placement type then judges. It used to
// scan down to the first standing surface, so the only cell a column could ever
// offer was its roof — nothing spawned in a cave or under water, at all.
// A column whose chunk has not streamed in yet is skipped rather than spawned
// into.
class NaturalSpawner final {
  public:
    NaturalSpawner();

    // Advances one 20 TPS tick, sampling a few columns per category the way
    // vanilla's spawner runs every tick. It used to bank a second's worth of
    // attempts into one tick; the samples are cheap individually but the biome
    // query that opens the chain is not, so a batch is a hitch.
    //
    // The environment carries the tick's ambient darkness, which is what turns
    // the stored static full-sun sky light into the reading vanilla's spawn
    // check makes — open ground goes dark at night and MONSTERs settle on the
    // surface. It used to be a float day/night factor multiplied into the sky
    // light, a render curve standing in for a gameplay one.
    void tick(const world::World& world, EntitySystem& entities, glm::vec3 center,
              float radius, Difficulty difficulty,
              const EnvironmentSnapshot& environment = EnvironmentSnapshot{});

    // Picks up the process-wide spawn tables (a new save or /reload). The
    // spawner used to own a BiomeSource of its own, rebuilt here from the world
    // seed; it reads the biome off the world now, so a seed is no longer any of
    // its business — see BM-DESIGN 判断 2, single source of truth.
    void refreshTables();

    // NaturalSpawner.spawnMobsForChunkGeneration (26.1): the world-generation-
    // time population pass, run once when a chunk is first generated rather
    // than waited out over the persistent per-tick cycle above. Only the
    // CREATURE category spawns here (vanilla's chunk-generation pass never
    // seeds MONSTER/AMBIENT/WATER_CREATURE — those still arrive solely through
    // tick()); a biome with no creature entries in its table is a no-op.
    //
    // Deterministic in (worldSeed, chunkX, chunkZ) alone: the position rolls,
    // the species pick, the group size and the retry jitter all come from one
    // JavaRandom stream seeded by setPopulationSeed(worldSeed, chunkX*16,
    // chunkZ*16) — the same derivation Features::generateVegetation already
    // uses for tree placement, so a chunk's generation-time herd is exactly as
    // reproducible as its trees. No wall-clock, no global RNG: replaying the
    // same seed and chunk position always lands the same individuals in the
    // same spots. Every spawned individual gets a `spawn(...)` seed itself
    // drawn from that same stream, so its wander/yaw is reproducible too.
    void spawnForChunkGeneration(const world::World& world, EntitySystem& entities,
                                 std::uint64_t worldSeed, int chunkX, int chunkZ) const;

    // Overlays the biome tables from a pack's `data/minecraft/worldgen/biome/…`.
    // Without this the built-in 26.1 numbers stand, which is the usual case:
    // an ordinary resource pack carries no `data/` at all.
    void loadSpawnData(const assets::ResourceProvider& resources) { tables_.load(resources); }

    [[nodiscard]] const MobSpawnSettings& table(world::gen::Biome biome) const {
        return tables_.settings(biome);
    }

    // The tables themselves, for a caller that installs its own — a test that
    // wants a known species in every biome, or a future /reload.
    [[nodiscard]] BiomeSpawnTables& spawnTables() { return tables_; }

  private:
    // One pass of the rule chain for one category, against one freshly sampled
    // column. Vanilla's `spawnCategoryForChunk` is the same unit of work: a
    // category and a random position, then every rule in order.
    void spawnOnce(const world::World& world, EntitySystem& entities, glm::vec3 center,
                   float radius, Difficulty difficulty,
                   const EnvironmentSnapshot& environment,
                   entities::MobCategory category);
    [[nodiscard]] const SpawnerData& pickWeighted(const std::vector<SpawnerData>& entries);

    BiomeSpawnTables tables_;
    // The 48-bit mc::rng state (Java LegacyRandomSource core) the spawn batch
    // advances. Session-only (rebuilt each run), so a fixed raw internal state.
    std::uint64_t randomState_ = 0x0000'9E3779B9ULL;
};

} // namespace mc::gameplay

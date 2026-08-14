#pragma once

#include "gameplay/Difficulty.hpp"
#include "gameplay/EnvironmentSnapshot.hpp"
#include "gameplay/MobSpawnSettings.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "world/gen/Biome.hpp"
#include "world/gen/BiomeSource.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <memory>
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
    explicit NaturalSpawner(std::uint64_t seed);

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

    // Rebuilds the biome source for a new world seed (a new save or /reload).
    void setSeed(std::uint64_t seed);

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

    std::unique_ptr<world::gen::BiomeSource> biomes_;
    BiomeSpawnTables tables_;
    std::uint32_t randomState_ = 0x9E3779B9U;
};

} // namespace mc::gameplay

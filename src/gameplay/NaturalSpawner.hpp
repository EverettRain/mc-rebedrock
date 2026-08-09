#pragma once

#include "gameplay/Difficulty.hpp"
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

// One species in a biome's spawn table: the creature, its weight in the
// biome's weighted pick, and the group size the spawner samples. Lives in
// gameplay (not world::gen) so it can name entities::EntityType without
// inverting the world → gameplay dependency; biomeDefinition stays data-only.
struct SpawnEntry final {
    const entities::EntityType& type;
    float weight = 1.0F;
    int minGroup = 1;
    int maxGroup = 1;
};

// The per-biome spawn settings a NaturalSpawner reads: the passive (CREATURE)
// and hostile (MONSTER) tables, mirroring 1.16.1's SpawnSettings.
struct BiomeSpawnTable final {
    std::vector<SpawnEntry> creatures;
    std::vector<SpawnEntry> monsters;
};

// NaturalSpawner (1.16.1's NaturalSpawner): every spawn interval it picks
// positions inside the simulation radius, checks the ground and the light rule
// (MONSTER in darkness, CREATURE in daylight), samples the biome's table by
// weight and spawns a group — stopping at each category's spawnCap. Unstreamed
// terrain reads as Air on both sides of the surface check, so it is skipped
// rather than spawning creatures into chunks that have not arrived yet.
class NaturalSpawner final {
  public:
    explicit NaturalSpawner(std::uint64_t seed);

    // Advances one 20 TPS tick. Every kIntervalTicks a batch of attempts runs.
    // `skyBrightness` is the day/night factor (DayNightCycle::state: 1 = day,
    // ~0 = night): the stored sky light is static full sun, so it is scaled by
    // this so open ground actually goes dark at night and MONSTERs spawn there.
    void tick(const world::World& world, EntitySystem& entities, glm::vec3 center,
              float radius, Difficulty difficulty, float skyBrightness = 1.0F);

    // Rebuilds the biome source for a new world seed (a new save or /reload).
    void setSeed(std::uint64_t seed);

    [[nodiscard]] const BiomeSpawnTable& table(world::gen::Biome biome) const {
        return tables_[static_cast<std::size_t>(biome)];
    }

  private:
    void buildSpawnTables();
    void spawnOnce(const world::World& world, EntitySystem& entities, glm::vec3 center,
                   float radius, Difficulty difficulty, float skyBrightness);
    [[nodiscard]] const SpawnEntry& pickWeighted(const std::vector<SpawnEntry>& entries);

    std::unique_ptr<world::gen::BiomeSource> biomes_;
    std::array<BiomeSpawnTable, static_cast<std::size_t>(world::gen::Biome::Count)> tables_;
    std::uint32_t randomState_ = 0x9E3779B9U;
    unsigned int tickCounter_ = 0U;
};

} // namespace mc::gameplay

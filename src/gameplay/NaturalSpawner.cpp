#include "gameplay/NaturalSpawner.hpp"

#include "gameplay/EntitySystem.hpp"
#include "world/Block.hpp"
#include "world/World.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace mc::gameplay {
namespace {

// The spawner runs one batch of attempts every second of game time, each batch
// a few independent positions — cheap enough to never touch the hot path, and
// slow enough that the population settles between waves.
constexpr unsigned int kIntervalTicks = 20U;
constexpr int kAttemptsPerCycle = 3;
// The light threshold below which a position counts as darkness for the
// MONSTER category (1.16.1 monsters need a light level under 7).
constexpr int kMonsterLightThreshold = 7;
// No mob spawns closer than this to the player (1.16.1's 24-block minimum),
// so a herd or a horde never materialises right on top of the camera.
constexpr float kMinimumSpawnDistance = 24.0F;
// The reference spawn region whose caps the 1.16.1 spawn settings name: the
// 128-block radius around the player. A smaller simulation radius scales the
// cap down with the area so density matches vanilla instead of crowding.
constexpr float kVanillaSpawnRadius = 128.0F;
constexpr float kTwoPi = 6.28318530718F;

// Small deterministic LCG (same constants as the entity wander) so a spawn
// batch is reproducible for a given world seed and tick sequence.
[[nodiscard]] std::uint32_t nextRandom(std::uint32_t& state) {
    state = state * 1664525U + 1013904223U;
    return state;
}

[[nodiscard]] float randomUnit(std::uint32_t& state) {
    return static_cast<float>(nextRandom(state) >> 8) / static_cast<float>(1U << 24);
}

// Which biomes a passive land creature may appear in: every overworld surface
// biome except the open waters and the river that cuts through them.
[[nodiscard]] bool isLandBiome(world::gen::Biome biome) {
    switch (biome) {
    case world::gen::Biome::Ocean:
    case world::gen::Biome::DeepOcean:
    case world::gen::Biome::River:
        return false;
    default:
        return true;
    }
}

// How many live creatures of a category sit inside the spawn disc. Counting
// only the nearby population (rather than the whole world) keeps the cap local:
// as the player walks on, the creatures left behind fall out of the count and a
// new area can populate — otherwise the first ten animals would block spawning
// everywhere else forever.
[[nodiscard]] int liveCountInRadius(const EntitySystem& entities,
                                    entities::MobCategory category, glm::vec3 center,
                                    float radius) {
    const float radiusSquared = radius * radius;
    int count = 0;
    for (const auto& entity : entities.entities()) {
        if (entity.dead() || entity.kind().category() != category) {
            continue;
        }
        const float dx = entity.position.x - center.x;
        const float dz = entity.position.z - center.z;
        if (dx * dx + dz * dz <= radiusSquared) {
            ++count;
        }
    }
    return count;
}

} // namespace

NaturalSpawner::NaturalSpawner(std::uint64_t seed)
    : biomes_(std::make_unique<world::gen::BiomeSource>(seed)) {
    buildSpawnTables();
}

void NaturalSpawner::setSeed(std::uint64_t seed) {
    biomes_ = std::make_unique<world::gen::BiomeSource>(seed);
    // The tables are derived from the registry at construction, which can run
    // before registerBuiltinEntities() fills it (the GameSession is built at
    // startup); a world load always comes after registration, so rebuild here.
    buildSpawnTables();
}

void NaturalSpawner::buildSpawnTables() {
    // The table is derived from the registry: every natural-spawning category
    // lands in the biomes it may appear in, so adding a species never touches
    // the spawner. Group sizes are the vanilla defaults for land animals and
    // standard hostiles. Rebuilt on every setSeed, so clear first.
    for (auto& table : tables_) {
        table.creatures.clear();
        table.monsters.clear();
    }
    for (const auto* type : entities::entityTypeRegistry().all()) {
        if (type == nullptr) {
            continue;
        }
        const auto traits = entities::mobCategoryTraits(type->category());
        if (!traits.naturalSpawn) {
            continue;
        }
        const SpawnEntry entry{*type, 1.0F, 1, 4};
        for (int biomeIndex = 0; biomeIndex < static_cast<int>(world::gen::Biome::Count);
             ++biomeIndex) {
            const auto biome = static_cast<world::gen::Biome>(biomeIndex);
            if (type->category() == entities::MobCategory::Monster) {
                tables_[biomeIndex].monsters.push_back(entry);
            } else if (type->category() == entities::MobCategory::Creature &&
                       isLandBiome(biome)) {
                tables_[biomeIndex].creatures.push_back(entry);
            }
        }
    }
}

void NaturalSpawner::tick(const world::World& world, EntitySystem& entities, glm::vec3 center,
                          float radius, Difficulty difficulty, float skyBrightness) {
    if (radius <= 0.0F) {
        return;
    }
    if (++tickCounter_ % kIntervalTicks != 0U) {
        return;
    }
    for (int attempt = 0; attempt < kAttemptsPerCycle; ++attempt) {
        spawnOnce(world, entities, center, radius, difficulty, skyBrightness);
    }
}

void NaturalSpawner::spawnOnce(const world::World& world, EntitySystem& entities,
                               glm::vec3 center, float radius, Difficulty difficulty,
                               float skyBrightness) {
    // Pick a position uniformly inside the disc around the player.
    const float angle = randomUnit(randomState_) * kTwoPi;
    const float distance = randomUnit(randomState_) * radius;
    const int spawnX = static_cast<int>(std::floor(center.x + std::cos(angle) * distance));
    const int spawnZ = static_cast<int>(std::floor(center.z + std::sin(angle) * distance));
    // 1.16.1 keeps the first ring around the player clear.
    const float nearX = static_cast<float>(spawnX) + 0.5F - center.x;
    const float nearZ = static_cast<float>(spawnZ) + 0.5F - center.z;
    if (nearX * nearX + nearZ * nearZ <
        kMinimumSpawnDistance * kMinimumSpawnDistance) {
        return;
    }
    // Scan down from a window above the player for the first solid block.
    int surfaceY = -1;
    const int topY = static_cast<int>(center.y) + 48;
    const int bottomY = std::max(static_cast<int>(center.y) - 32, 0);
    for (int y = topY; y >= bottomY; --y) {
        if (world::hasCollision(world.block(spawnX, y, spawnZ))) {
            surfaceY = y;
            break;
        }
    }
    if (surfaceY < 0) {
        return; // no terrain (open sky or a chunk the streamer has not sent)
    }
    const int spawnY = surfaceY + 1;
    // The cell the creature would stand in must be clear and dry.
    if (world::hasCollision(world.block(spawnX, spawnY, spawnZ)) ||
        world::isFluid(world.block(spawnX, spawnY, spawnZ))) {
        return;
    }

    // Combined light, Java's getLightLevel(pos, 0): the brighter of the sky and
    // block channels. The stored sky light is static full sun, so it is scaled
    // by the current sky brightness: at night open ground drops below the
    // threshold and MONSTERs spawn there, exactly like 1.16.1's checkDespawn.
    const int sky = static_cast<int>(static_cast<float>(world.skyLight(spawnX, spawnY, spawnZ)) *
                                     skyBrightness);
    const bool dark =
        std::max(sky, static_cast<int>(world.blockLight(spawnX, spawnY, spawnZ))) <
        kMonsterLightThreshold;
    const auto category =
        dark ? entities::MobCategory::Monster : entities::MobCategory::Creature;
    const auto traits = entities::mobCategoryTraits(category);
    // Peaceful worlds never host natural hostiles; MISC never spawns at all.
    if (!traits.naturalSpawn || (dark && difficulty == Difficulty::Peaceful)) {
        return;
    }
    // Category cap, scaled to the simulated area: the 1.16.1 caps are named for
    // a 128-block radius, so a smaller simulation radius keeps the same density
    // instead of crowding the same count into a quarter of the area. Only the
    // creatures inside the disc count, so a new area repopulates as the player
    // walks on and the old ones fall behind the cap.
    const int effectiveCap = std::max(
        1, static_cast<int>(traits.spawnCap *
                            ((radius * radius) / (kVanillaSpawnRadius * kVanillaSpawnRadius))));
    if (liveCountInRadius(entities, category, center, radius) >= effectiveCap) {
        return;
    }
    const auto& table = tables_[static_cast<std::size_t>(biomes_->biomeAtBlock(spawnX, spawnZ))];
    const auto& entries = dark ? table.monsters : table.creatures;
    if (entries.empty()) {
        return;
    }
    const auto& entry = pickWeighted(entries);
    const int groupSize =
        entry.minGroup + static_cast<int>(
            randomUnit(randomState_) * static_cast<float>(entry.maxGroup - entry.minGroup + 1));
    // Spawn the group side by side so overlapping boxes push apart instead of
    // stacking into one column.
    for (int index = 0; index < groupSize; ++index) {
        entities.spawn({static_cast<float>(spawnX) + 0.5F + static_cast<float>(index) * 0.4F,
                        static_cast<float>(spawnY) + 0.05F,
                        static_cast<float>(spawnZ) + 0.5F},
                       entry.type);
    }
}

const SpawnEntry& NaturalSpawner::pickWeighted(const std::vector<SpawnEntry>& entries) {
    float total = 0.0F;
    for (const auto& entry : entries) {
        total += entry.weight;
    }
    float roll = randomUnit(randomState_) * total;
    for (const auto& entry : entries) {
        roll -= entry.weight;
        if (roll <= 0.0F) {
            return entry;
        }
    }
    return entries.back();
}

} // namespace mc::gameplay

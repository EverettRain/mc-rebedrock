#include "gameplay/NaturalSpawner.hpp"

#include "gameplay/EntitySystem.hpp"
#include "gameplay/Random.hpp"
#include "gameplay/SpawnPlacements.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"
#include "world/gen/JavaRandom.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace mc::gameplay {
namespace {

// The light threshold below which a position counts as darkness for the
// MONSTER category (vanilla monsters need a light level under 7).
constexpr int kMonsterLightThreshold = 7;
// No mob spawns closer than this to the player (vanilla's 24-block minimum),
// so a herd or a horde never materialises right on top of the camera.
constexpr float kMinimumSpawnDistance = 24.0F;
// The reference spawn region whose caps the vanilla spawn settings name: the
// 128-block radius around the player. A smaller simulation radius scales the
// cap down with the area so density matches vanilla instead of crowding.
constexpr float kVanillaSpawnRadius = 128.0F;
constexpr float kTwoPi = 6.28318530718F;
constexpr float kPi = 3.14159265F;

// The categories the spawner runs the chain for, in vanilla's order. This is
// the `for (MobCategory category : SPAWNING_CATEGORIES)` loop of
// `NaturalSpawner.spawnForChunk`: each category gets its own independent
// position samples, so darkness no longer has to *decide* which category an
// attempt belongs to — it filters the category it was handed.
constexpr std::array<entities::MobCategory, 4> kSpawningCategories{
    entities::MobCategory::Monster,
    entities::MobCategory::Creature,
    entities::MobCategory::Ambient,
    entities::MobCategory::WaterCreature,
};

// Vanilla samples one column per loaded chunk per tick, per category. The
// simulation disc covers pi*r^2/256 chunks, so spreading that many samples per
// category over a second of ticks is vanilla's shape at a twentieth of its
// rate. The rate only decides how fast a fresh area fills up; where the
// population settles is the caps' job.
//
// It has to scale with the disc rather than stay the flat 3 it was, because a
// rolled height inside a column is rejected far more often than the surface
// scan it replaces — most cells of most columns are solid rock. And it is spent
// every tick rather than banked into one batch per second: the biome query the
// chain opens with costs ~3.6 us, so a second's worth of samples taken at once
// is a ~2 ms hitch in a single tick, against ~70 us per tick spread out.
constexpr int kTicksPerSecond = 20;
constexpr int kMinimumColumnSamples = 1;
constexpr int kMaximumColumnSamples = 16;

[[nodiscard]] int columnSamplesPerTick(float radius) {
    const float chunks = (kPi * radius * radius) /
                         static_cast<float>(world::kChunkWidth * world::kChunkDepth);
    return std::clamp(static_cast<int>(chunks) / kTicksPerSecond, kMinimumColumnSamples,
                      kMaximumColumnSamples);
}

[[nodiscard]] int floorDiv(int value, int divisor) {
    int quotient = value / divisor;
    if (value % divisor < 0) {
        --quotient;
    }
    return quotient;
}

// Heightmap.Types.WORLD_SURFACE for one column: the highest non-air cell, or
// -1 when the column is empty or its chunk has not streamed in yet.
//
// Read through the chunk and its sections rather than World::block so one
// sample costs one hash lookup and a handful of section emptiness checks
// instead of up to 256 lookups — an all-air section is skipped whole, and a
// surface world is mostly those.
[[nodiscard]] int worldSurfaceY(const world::World& world, int x, int z) {
    const int chunkX = floorDiv(x, world::kChunkWidth);
    const int chunkZ = floorDiv(z, world::kChunkDepth);
    const world::Chunk* chunk = world.chunk({chunkX, chunkZ});
    if (chunk == nullptr) {
        return world::kMinY - 1;  // an unloaded column: a value below the world
    }
    const int localX = x - chunkX * world::kChunkWidth;
    const int localZ = z - chunkZ * world::kChunkDepth;
    for (int sectionY = world::kSectionCount - 1; sectionY >= 0; --sectionY) {
        if (chunk->section(sectionY).empty()) {
            continue;
        }
        const int base = world::sectionOriginY(sectionY);
        for (int offset = world::kSectionSize - 1; offset >= 0; --offset) {
            if (chunk->block(localX, base + offset, localZ) != world::Block::Air) {
                return base + offset;
            }
        }
    }
    // No solid surface: the same below-the-world sentinel. −1 is a legal world
    // row in the 384-tall column now, so it can no longer stand in for "none".
    return world::kMinY - 1;
}

// The shared deterministic generator (mc::rng, Java's LegacyRandomSource core,
// same core the entity wander advances) so a spawn batch is reproducible for a
// given world seed and tick sequence. A value in [0, 1) is nextFloat.
[[nodiscard]] float randomUnit(std::uint64_t& state) { return mc::rng::nextFloat(state); }

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

// WeightedList#getRandom, drawn from the world-generation JavaRandom stream
// rather than the tick spawner's small LCG — spawnForChunkGeneration shares one
// stream across the whole pass (position, species, group size, retry jitter),
// matching vanilla's WorldgenRandom usage in spawnMobsForChunkGeneration.
[[nodiscard]] const SpawnerData* pickWeightedGeneration(
    const std::vector<SpawnerData>& entries, world::gen::JavaRandom& random) {
    int total = 0;
    for (const auto& entry : entries) {
        total += entry.weight;
    }
    if (total <= 0) {
        return nullptr;
    }
    int roll = random.nextInt(total);
    for (const auto& entry : entries) {
        roll -= entry.weight;
        if (roll < 0) {
            return &entry;
        }
    }
    return &entries.back();
}

} // namespace

NaturalSpawner::NaturalSpawner(std::uint64_t seed)
    : biomes_(std::make_unique<world::gen::BiomeSource>(seed)) {
    tables_.loadBuiltinDefaults();
}

void NaturalSpawner::setSeed(std::uint64_t seed) {
    biomes_ = std::make_unique<world::gen::BiomeSource>(seed);
    // The tables name species through the registry, which can still be empty
    // when the GameSession is built at startup. A world load always comes after
    // registration *and* after the pack stack has been read, so this is where
    // the spawner picks up whatever the process-wide tables ended up holding.
    tables_ = biomeSpawnTables();
}

// NaturalSpawner.spawnMobsForChunkGeneration, condensed to this game's shape:
// no world border term (SpawnPlacements.isSpawnPositionOk here already drops
// it, matching spawnOnce above), no ceiling probe in getTopNonCollidingPos
// (the overworld here has none — that branch is `dimensionType().hasCeiling()`,
// vanilla's nether-only case), and finalizeSpawn/checkSpawnObstruction/
// checkSpawnRules are named as comments rather than faked, the same "a stage
// with nothing behind it stays a comment" rule spawnOnce documents above.
void NaturalSpawner::spawnForChunkGeneration(const world::World& world, EntitySystem& entities,
                                             std::uint64_t worldSeed, int chunkX,
                                             int chunkZ) const {
    // getRandomSpawnMobAt reads the biome at the chunk's own centre column in
    // vanilla's spawnOriginalMobs (worldGenRegion.getCenter()), not per-member —
    // one biome lookup governs the whole chunk's generation-time population.
    const auto biome = biomes_->biomeAtBlock(chunkX * world::kChunkWidth + 8,
                                             chunkZ * world::kChunkDepth + 8);
    const auto& mobs = tables_.settings(biome).forCategory(entities::MobCategory::Creature);
    if (mobs.empty()) {
        return;
    }

    // setDecorationSeed (Java) / setPopulationSeed (this port, same LCG and the
    // same derivation Features::generateVegetation already draws tree placement
    // from): the whole pass is one JavaRandom stream seeded from
    // (worldSeed, chunkMinX, chunkMinZ) alone, so replaying the same seed and
    // chunk position reproduces the exact same herd every time — no wall-clock,
    // no shared/global RNG, independent of generation order or thread.
    world::gen::JavaRandom random;
    random.setPopulationSeed(worldSeed, chunkX * world::kChunkWidth, chunkZ * world::kChunkDepth);

    const int originX = chunkX * world::kChunkWidth;
    const int originZ = chunkZ * world::kChunkDepth;

    // creatureGenerationProbability's WeightedList loop: vanilla draws entries
    // until a coin flip fails, so a chunk can seed more than one species/group
    // (usually zero or one at the default 0.1 probability). kMaxDraws bounds a
    // pathological probability from spinning forever — vanilla's own bound is a
    // codec range clamp (0..0.9999999), not a loop counter, but a bound here
    // costs nothing and keeps generation latency predictable.
    constexpr float kCreatureSpawnProbability = 0.1F; // MobSpawnSettings' default
    constexpr int kMaxDraws = 64;
    for (int draws = 0; draws < kMaxDraws && random.nextFloat() < kCreatureSpawnProbability;
         ++draws) {
        const SpawnerData* entry = pickWeightedGeneration(mobs, random);
        if (entry == nullptr || entry->type == nullptr) {
            continue;
        }
        const int count =
            entry->minGroup + random.nextInt(1 + entry->maxGroup - entry->minGroup);
        const int startX = originX + random.nextInt(world::kChunkWidth);
        const int startZ = originZ + random.nextInt(world::kChunkDepth);
        int x = startX;
        int z = startZ;
        const entities::SpawnPlacement placement = entry->type->spawnPlacement();

        for (int member = 0; member < count; ++member) {
            bool success = false;
            // getTopNonCollidingPos + isSpawnPositionOk + noCollision, retried
            // with vanilla's `x/z += nextInt(5) - nextInt(5)` jitter up to four
            // times, re-clamped into the chunk's own 16x16 column whenever the
            // jitter walks the probe outside it (vanilla's do-while below the
            // attempts loop).
            for (int attempts = 0; !success && attempts < 4; ++attempts) {
                const int surfaceY = worldSurfaceY(world, x, z);
                if (surfaceY >= world::kMinY) {
                    const int spawnY = surfaceY + 1;
                    const SimulationPosition cell{x, spawnY, z};
                    if (isSpawnPositionOk(world, cell, placement)) {
                        const glm::vec3 memberPosition{
                            static_cast<float>(x) + 0.5F, static_cast<float>(spawnY),
                            static_cast<float>(z) + 0.5F};
                        if (EntitySystem::canOccupy(world, memberPosition,
                                                    entry->type->dimensions())) {
                            // --- species spawn rules ---  (checkSpawnRules: none yet)
                            // Deterministic per-individual seed drawn from the
                            // same stream, so wander/yaw reproduce too.
                            const auto individualSeed = static_cast<std::uint32_t>(
                                random.nextInt() ^ static_cast<std::int32_t>(worldSeed));
                            entities.spawn(memberPosition, *entry->type,
                                          individualSeed == 0U ? 1U : individualSeed);
                            // --- finalizeSpawn ---  (group buffs: none yet)
                            success = true;
                        }
                    }
                }
                if (success) {
                    break;
                }
                x += random.nextInt(5) - random.nextInt(5);
                z += random.nextInt(5) - random.nextInt(5);
                while (x < originX || x >= originX + world::kChunkWidth || z < originZ ||
                      z >= originZ + world::kChunkDepth) {
                    x = startX + random.nextInt(5) - random.nextInt(5);
                    z = startZ + random.nextInt(5) - random.nextInt(5);
                }
            }
        }
    }
}

void NaturalSpawner::tick(const world::World& world, EntitySystem& entities, glm::vec3 center,
                          float radius, Difficulty difficulty,
                          const EnvironmentSnapshot& environment) {
    if (radius <= 0.0F) {
        return;
    }
    const int samples = columnSamplesPerTick(radius);
    for (const auto category : kSpawningCategories) {
        // A category no species anywhere belongs to is not sampled at all.
        // AMBIENT and WATER_CREATURE are both empty in this game, and each
        // sample would otherwise pay the chain's biome query only to read an
        // empty list out of it.
        if (!tables_.anySpecies(category)) {
            continue;
        }
        for (int sample = 0; sample < samples; ++sample) {
            spawnOnce(world, entities, center, radius, difficulty, environment, category);
        }
    }
}

// The spawn rule chain, in vanilla's order, as a named sequence: every stage
// below is one of `NaturalSpawner.spawnCategoryForPosition` /
// `isValidSpawnPostitionForType`'s tests, in the order it runs them.
//
// Two stages have no content in this game and are named rather than
// implemented — the species spawn rules (`SpawnPlacements.checkSpawnRules`,
// which vanilla uses for things like "a drowned needs to be in a river"; no
// species here has one beyond the category light rule) and the world border.
// A stage with nothing behind it is a claim the code cannot make good on, so
// they stay comments until there is something to check. Inserting one later is
// a job of dropping it into a named sequence, not of rediscovering its place.
void NaturalSpawner::spawnOnce(const world::World& world, EntitySystem& entities,
                               glm::vec3 center, float radius, Difficulty difficulty,
                               const EnvironmentSnapshot& environment,
                               entities::MobCategory category) {
    const auto traits = entities::mobCategoryTraits(category);
    // MISC never spawns naturally, and Peaceful removes MONSTERs the same tick
    // they arrive. This used to read `dark && difficulty == Peaceful`, which was
    // the same test back when darkness was what picked the category.
    if (!traits.naturalSpawn ||
        (traits.disallowedInPeaceful && difficulty == Difficulty::Peaceful)) {
        return;
    }

    // --- position sample (getRandomPosWithin) ---
    // A random column in the disc, then a random height *inside* it, bounded by
    // the column's surface. Rolling the height is the whole reason a cave can
    // spawn anything: the surface is one cell out of the column's hundreds, and
    // the old downward scan offered nothing else.
    const float angle = randomUnit(randomState_) * kTwoPi;
    const float distance = randomUnit(randomState_) * radius;
    const int spawnX = static_cast<int>(std::floor(center.x + std::cos(angle) * distance));
    const int spawnZ = static_cast<int>(std::floor(center.z + std::sin(angle) * distance));
    const int surfaceY = worldSurfaceY(world, spawnX, spawnZ);
    if (surfaceY < world::kMinY) {
        return; // an empty column, or a chunk the streamer has not sent
    }
    // Mth.randomBetweenInclusive(random, minY, surface + 1): inclusive at both
    // ends, so the cell just above the terrain is drawn like any other.
    const auto columnHeight = static_cast<std::uint32_t>(surfaceY - world::kMinY + 2);
    const int spawnY =
        world::kMinY + static_cast<int>(mc::rng::nextInt(randomState_, columnHeight));
    const SimulationPosition position{spawnX, spawnY, spawnZ};

    // --- distance to the player (isRightDistanceToPlayerAndSpawnPoint) ---
    // vanilla keeps the first ring around the player clear. Vanilla always
    // measured this in three dimensions; it only looked two-dimensional here
    // because y was pinned to the surface the player stood on.
    const float nearX = static_cast<float>(spawnX) + 0.5F - center.x;
    const float nearY = static_cast<float>(spawnY) - center.y;
    const float nearZ = static_cast<float>(spawnZ) + 0.5F - center.z;
    if (nearX * nearX + nearY * nearY + nearZ * nearZ <
        kMinimumSpawnDistance * kMinimumSpawnDistance) {
        return;
    }

    // --- biome table (getRandomSpawnMobAt) ---
    const auto& table = tables_.settings(biomes_->biomeAtBlock(spawnX, spawnZ));
    const auto& entries = table.forCategory(category);
    if (entries.empty()) {
        return;
    }
    const auto& entry = pickWeighted(entries);

    // --- category cap ---
    // Scaled to the simulated area: the vanilla caps are named for a 128-block
    // radius, so a smaller simulation radius keeps the same density instead of
    // crowding the same count into a quarter of the area. Only the creatures
    // inside the disc count, so a new area repopulates as the player walks on
    // and the old ones fall behind the cap.
    const int effectiveCap = std::max(
        1, static_cast<int>(static_cast<float>(traits.spawnCap) *
                            ((radius * radius) / (kVanillaSpawnRadius * kVanillaSpawnRadius))));
    if (liveCountInRadius(entities, category, center, radius) >= effectiveCap) {
        return;
    }

    // --- placement type (SpawnPlacements.isSpawnPositionOk) ---
    // The species decides what kind of cell it is born in: a pig needs a floor
    // and two clear cells, a squid needs water. This is the stage the old code
    // had no notion of — it hard-coded ON_GROUND, at the surface.
    const entities::SpawnPlacement placement = entry.type->spawnPlacement();
    if (!isSpawnPositionOk(world, position, placement)) {
        return;
    }

    // --- environment / light ---
    // getMaxLocalRawBrightness: the brighter of the block channel and the
    // stored static full-sun sky light minus the tick's ambient darkness. This
    // used to multiply the sky light by the render curve's sky brightness,
    // which is a smoothstep with a 0.05 floor — it darkened the whole scale
    // instead of subtracting levels, so a torch-lit clearing dimmed at dusk
    // exactly like an open field. Subtracting leaves the block channel alone,
    // which is what keeps lit ground mob-free through the night.
    const bool dark = environment::maxLocalRawBrightness(world, spawnX, spawnY, spawnZ,
                                                         environment) < kMonsterLightThreshold;
    if (dark != traits.spawnsInDarkness) {
        return;
    }

    // --- species spawn rules ---   (SpawnPlacements.checkSpawnRules: none yet)
    // --- world border ---          (this game has no world border)

    // --- pack loop (the group the entry names) ---
    const int groupSize =
        entry.minGroup + static_cast<int>(
            randomUnit(randomState_) * static_cast<float>(entry.maxGroup - entry.minGroup + 1));
    // Spawn the group side by side so overlapping boxes push apart instead of
    // stacking into one column. Every member is judged on its own: the placement
    // type again, because a later member sits in a neighbouring column that may
    // have no floor under it at all, and then the complete species AABB, because
    // a tall mob needs headroom the single-cell placement probe cannot see.
    for (int index = 0; index < groupSize; ++index) {
        const glm::vec3 memberPosition{
            static_cast<float>(spawnX) + 0.5F + static_cast<float>(index) * 0.4F,
            static_cast<float>(spawnY) + 0.05F,
            static_cast<float>(spawnZ) + 0.5F};
        const SimulationPosition memberCell{
            static_cast<int>(std::floor(memberPosition.x)), spawnY, spawnZ};
        // --- collision / fluid ---
        if (!isSpawnPositionOk(world, memberCell, placement) ||
            !EntitySystem::canOccupy(world, memberPosition, entry.type->dimensions())) {
            continue;
        }
        // --- spawn ---
        entities.spawn(memberPosition, *entry.type);
    }
}

// WeightedRandom#getRandomItem: roll against the summed weights and walk. The
// weights are vanilla's integers (pig 10 against cow 8), so the roll is an
// integer one — a float sum of large weights loses the low bits that decide
// between a zombie at 95 and anything rarer.
const SpawnerData& NaturalSpawner::pickWeighted(const std::vector<SpawnerData>& entries) {
    int total = 0;
    for (const auto& entry : entries) {
        total += entry.weight;
    }
    if (total <= 0) {
        return entries.back();
    }
    int roll =
        static_cast<int>(mc::rng::nextInt(randomState_, static_cast<std::uint32_t>(total)));
    for (const auto& entry : entries) {
        roll -= entry.weight;
        if (roll < 0) {
            return entry;
        }
    }
    return entries.back();
}

} // namespace mc::gameplay

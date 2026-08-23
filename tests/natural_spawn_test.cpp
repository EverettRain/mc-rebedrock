#include "gameplay/Difficulty.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/NaturalSpawner.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"
#include "world/gen/Biome.hpp"
#include "world/gen/BiomeSource.hpp"

#include <cassert>
#include <cstdint>
#include <utility>

// Exercises NaturalSpawner headlessly: the per-biome tables carry the right
// species, a dark surface accumulates MONSTERs up to the area-scaled cap, the
// player's 24-block ring stays clear, Peaceful keeps the world empty, and the
// day/night sky brightness decides what spawns on a full-sun surface.

namespace {

// A flat stone floor large enough for the 64-block spawn disc and its 24-block
// exclusion ring: chunks -4..4 span -64..80.
mc::world::World makeFlatWorld() {
    mc::world::World world;
    for (int chunkZ = -4; chunkZ <= 4; ++chunkZ) {
        for (int chunkX = -4; chunkX <= 4; ++chunkX) {
            mc::world::Chunk chunk;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    chunk.setBlock(x, 0, z, mc::world::Block::Stone);
                }
            }
            world.setChunk({chunkX, chunkZ}, std::move(chunk));
        }
    }
    return world;
}

std::size_t countCategory(const mc::gameplay::EntitySystem& entities,
                          mc::gameplay::entities::MobCategory category) {
    std::size_t count = 0U;
    for (const auto& entity : entities.entities()) {
        if (!entity.dead() && entity.kind().category() == category) {
            ++count;
        }
    }
    return count;
}

// The nearest spawned creature must sit at least 24 blocks from the player
// (1.16.1's minimum spawn distance), never in the cleared ring.
std::size_t countInsideRing(const mc::gameplay::EntitySystem& entities, glm::vec3 center) {
    std::size_t count = 0U;
    constexpr float kRing = 24.0F * 24.0F;
    for (const auto& entity : entities.entities()) {
        const float dx = entity.position.x - center.x;
        const float dz = entity.position.z - center.z;
        if (dx * dx + dz * dz < kRing) {
            ++count;
        }
    }
    return count;
}

} // namespace

int main() {
    using namespace mc;

    // A spawner created before the registry is populated (the GameSession is
    // built at startup, ahead of registerBuiltinEntities) must rebuild its
    // tables when a world seed is set on load — regression for the empty-table
    // bug that kept natural spawns silent in the actual game.
    mc::gameplay::NaturalSpawner earlySpawner(0x1234ABCDU);
    assert(earlySpawner.table(world::gen::Biome::Plains)
               .empty(gameplay::entities::MobCategory::Creature));

    mc::gameplay::entities::registerBuiltinEntities();
    // The process-wide tables are what setSeed copies from, so they have to be
    // rebuilt after registration too. Before this, Application loaded them
    // ahead of the registry and every spawner inherited an empty table — a
    // world that never spawned a single mob, with nothing in the logs.
    mc::gameplay::biomeSpawnTables().loadBuiltinDefaults();
    earlySpawner.setSeed(0x1234ABCDU);
    assert(!earlySpawner.table(world::gen::Biome::Plains)
                .empty(gameplay::entities::MobCategory::Creature));

    auto world = makeFlatWorld();

    // The tables derive from the registry: land biomes host the passive species,
    // oceans never do, and every biome can host a MONSTER.
    mc::gameplay::NaturalSpawner spawner(0x1234ABCDU);
    using gameplay::entities::MobCategory;
    assert(!spawner.table(world::gen::Biome::Plains).empty(MobCategory::Creature));
    assert(spawner.table(world::gen::Biome::Ocean).empty(MobCategory::Creature));
    assert(!spawner.table(world::gen::Biome::Ocean).empty(MobCategory::Monster));
    // A desert has no farm animals in vanilla, and now none here either: the
    // old table put every passive species in every land biome at weight 1.
    assert(spawner.table(world::gen::Biome::Desert).empty(MobCategory::Creature));
    bool hasPig = false;
    bool hasCow = false;
    for (const auto& entry : spawner.table(world::gen::Biome::Plains).forCategory(
             MobCategory::Creature)) {
        hasPig = hasPig || entry.type->id().matches("pig");
        hasCow = hasCow || entry.type->id().matches("cow");
        // BiomeDefaultFeatures.farmAnimals' own numbers: groups of four, and a
        // pig is 10/8 as likely as a cow.
        assert(entry.minGroup == 4 && entry.maxGroup == 4);
    }
    assert(hasPig && hasCow);
    for (const auto& entry : spawner.table(world::gen::Biome::Plains).forCategory(
             MobCategory::Creature)) {
        assert(entry.type->id().matches("pig") ? entry.weight == 10 : entry.weight == 8);
    }
    for (const auto& entry : spawner.table(world::gen::Biome::Plains).forCategory(
             MobCategory::Monster)) {
        assert(entry.weight == 95 && entry.minGroup == 4 && entry.maxGroup == 4);
    }

    // A dark surface is monster territory. At a 64-block radius the 1.16.1 cap
    // of 70 scales down with the area (× (64/128)² ≈ 17); nothing may spawn in
    // the 24-block ring around the player.
    mc::gameplay::EntitySystem entities;
    const glm::vec3 player{0.0F, 1.0F, 0.0F};
    for (int tick = 0; tick < 400; ++tick) {
        spawner.tick(world, entities, player, 64.0F, mc::gameplay::Difficulty::Normal);
    }
    const std::size_t monsters = countCategory(entities, gameplay::entities::MobCategory::Monster);
    assert(monsters > 0U);
    // Soft cap: a group is checked before it spawns, so it can overshoot by one
    // group size, but it must stay near the area-scaled ~17.
    assert(monsters <= 17U + 4U);
    // 1.16.1 keeps the ring around the player clear.
    assert(countInsideRing(entities, player) == 0U);
    // Every group member was validated with its complete species AABB, not just
    // the first member's feet cell.
    for (const auto& entity : entities.entities()) {
        assert(mc::gameplay::EntitySystem::canOccupy(
            world, entity.position, entity.dimensions()));
    }

    // Peaceful worlds never host natural hostiles, so the same dark surface
    // stays empty.
    mc::gameplay::EntitySystem peacefulEntities;
    mc::gameplay::NaturalSpawner peacefulSpawner(0x0BADF00DU);
    for (int tick = 0; tick < 400; ++tick) {
        peacefulSpawner.tick(world, peacefulEntities, player, 64.0F,
                             mc::gameplay::Difficulty::Peaceful);
    }
    assert(peacefulEntities.entities().empty());

    // The day/night pair travels as the resolved environment rather than a
    // brightness factor: at midnight the ambient darkness takes an open field's
    // static full-sun sky light below the monster threshold.
    const auto noon = mc::gameplay::EnvironmentSnapshot::resolve(6000.0, 0.0F, 0.0F);
    const auto midnight = mc::gameplay::EnvironmentSnapshot::resolve(18000.0, 0.0F, 0.0F);
    assert(noon.ambientDarkness == 0);
    assert(midnight.ambientDarkness == 11);

    // Day/night: a full-sun surface is not dark by day (no MONSTERs), and the
    // same surface at night goes dark and spawns MONSTERs — the check is
    // biome-independent because MONSTERs exist in every biome's table.
    {
        mc::world::World litWorld;
        for (int chunkZ = -4; chunkZ <= 4; ++chunkZ) {
            for (int chunkX = -4; chunkX <= 4; ++chunkX) {
                mc::world::Chunk chunk;
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        chunk.setBlock(x, 0, z, mc::world::Block::Stone);
                    }
                }
                litWorld.setChunk({chunkX, chunkZ}, std::move(chunk));
            }
        }
        for (int z = -64; z < 80; ++z) {
            for (int x = -64; x < 80; ++x) {
                litWorld.setSkyLight(x, 1, z, 15U);
            }
        }
        mc::gameplay::EntitySystem dayEntities;
        mc::gameplay::NaturalSpawner daySpawner(0x5EED5EEDU);
        for (int tick = 0; tick < 400; ++tick) {
            daySpawner.tick(litWorld, dayEntities, player, 64.0F,
                            mc::gameplay::Difficulty::Normal, noon);
        }
        assert(countCategory(dayEntities, gameplay::entities::MobCategory::Monster) == 0U);

        mc::gameplay::EntitySystem nightEntities;
        mc::gameplay::NaturalSpawner nightSpawner(0x0BADF00DU);
        for (int tick = 0; tick < 400; ++tick) {
            nightSpawner.tick(litWorld, nightEntities, player, 64.0F,
                              mc::gameplay::Difficulty::Normal, midnight);
        }
        assert(countCategory(nightEntities, gameplay::entities::MobCategory::Monster) > 0U);
        assert(countInsideRing(nightEntities, player) == 0U);
    }

    // A canopy is collision geometry but not a land-spawn surface. The scan
    // must continue to the real floor instead of placing the entire population
    // on the highest leaf layer.
    {
        auto canopyWorld = makeFlatWorld();
        for (int z = -64; z < 80; ++z) {
            for (int x = -64; x < 80; ++x) {
                assert(canopyWorld.setBlock(x, 6, z, mc::world::Block::OakLeaves));
                assert(canopyWorld.setSkyLight(x, 1, z, 15U));
            }
        }
        mc::gameplay::EntitySystem canopyEntities;
        mc::gameplay::NaturalSpawner canopySpawner(0x51AFC0DEU);
        // This case is about the surface scan finding the floor rather than the
        // canopy, so it must not also depend on which biome the seed lands in:
        // vanilla's tables give a desert or a beach no farm animals at all.
        for (int index = 0; index < static_cast<int>(world::gen::Biome::Count); ++index) {
            canopySpawner.spawnTables().set(
                static_cast<world::gen::Biome>(index),
                gameplay::entities::MobCategory::Creature,
                {{mc::gameplay::entities::entityTypeRegistry().byId("pig"), 10, 4, 4}});
        }
        for (int tick = 0; tick < 600; ++tick) {
            canopySpawner.tick(canopyWorld, canopyEntities, player, 64.0F,
                               mc::gameplay::Difficulty::Normal, noon);
        }
        assert(!canopyEntities.entities().empty());
        for (const auto& entity : canopyEntities.entities()) {
            assert(entity.position.y < 2.0F);
        }
    }

    // CS-4: spawnForChunkGeneration — the world-generation-time population
    // pass. Every biome is forced to carry a known CREATURE entry so the test
    // is independent of where the biome map lands this chunk, the same way the
    // canopy case above sidesteps it.
    {
        auto genWorld = makeFlatWorld();
        mc::gameplay::NaturalSpawner genSpawner(0x9E3779B9U);
        for (int index = 0; index < static_cast<int>(world::gen::Biome::Count); ++index) {
            genSpawner.spawnTables().set(
                static_cast<world::gen::Biome>(index), MobCategory::Creature,
                {{mc::gameplay::entities::entityTypeRegistry().byId("pig"), 10, 4, 4},
                 {mc::gameplay::entities::entityTypeRegistry().byId("cow"), 8, 4, 4}});
        }

        // Determinism: replaying the same (worldSeed, chunkX, chunkZ) — the
        // only inputs the pass takes — reproduces the exact same herd, down to
        // species, position and individual seed. No wall-clock, no global RNG.
        // Chunk (-2, 3) is a fixed point where this seed's first probability
        // draw against the 0.1 threshold is known (< 0.1, verified offline) to
        // actually enter the WeightedList draw, so the test does not depend on
        // a coin flip landing the way the assertions below assume.
        mc::gameplay::EntitySystem firstRun;
        genSpawner.spawnForChunkGeneration(genWorld, firstRun, 0xC0FFEEULL, -2, 3);
        mc::gameplay::EntitySystem secondRun;
        genSpawner.spawnForChunkGeneration(genWorld, secondRun, 0xC0FFEEULL, -2, 3);
        assert(firstRun.entities().size() == secondRun.entities().size());
        assert(!firstRun.entities().empty());
        for (std::size_t index = 0; index < firstRun.entities().size(); ++index) {
            const auto& a = firstRun.entities()[index];
            const auto& b = secondRun.entities()[index];
            assert(a.type == b.type);
            assert(a.position == b.position);
            assert(a.rngState == b.rngState);
            assert(a.yaw == b.yaw);
        }
        // Every individual landed inside the requested chunk's own 16x16
        // column (vanilla's xo/zo clamp — a species never spills into the
        // neighbour it was not generated for).
        for (const auto& entity : firstRun.entities()) {
            const int floorX = static_cast<int>(std::floor(entity.position.x));
            const int floorZ = static_cast<int>(std::floor(entity.position.z));
            const int chunkX = (floorX >= 0 ? floorX : floorX - 15) / 16;
            const int chunkZ = (floorZ >= 0 ? floorZ : floorZ - 15) / 16;
            assert(chunkX == -2 && chunkZ == 3);
        }
        // Every spawned individual passed the same placement/collision rules
        // spawnOnce enforces — a floor beneath it, headroom above, and no
        // overlap with world geometry.
        for (const auto& entity : firstRun.entities()) {
            assert(mc::gameplay::EntitySystem::canOccupy(genWorld, entity.position,
                                                          entity.dimensions()));
        }

        // A different chunk position (still same seed) draws from a different
        // point in the stream and is not required to reproduce run one's
        // layout — this is a sanity check that the position actually enters
        // the seed derivation rather than being ignored. Chunk (3, 3) is
        // another fixed point confirmed offline to also enter the draw, so
        // both sides of the comparison carry a real, non-empty herd.
        mc::gameplay::EntitySystem otherChunk;
        genSpawner.spawnForChunkGeneration(genWorld, otherChunk, 0xC0FFEEULL, 3, 3);
        assert(!otherChunk.entities().empty());
        bool anyDifferentPosition = otherChunk.entities().size() != firstRun.entities().size();
        if (!anyDifferentPosition) {
            for (std::size_t index = 0; index < otherChunk.entities().size(); ++index) {
                if (otherChunk.entities()[index].position != firstRun.entities()[index].position) {
                    anyDifferentPosition = true;
                    break;
                }
            }
        }
        assert(anyDifferentPosition);

        // A biome whose CREATURE table is empty (every real ocean/desert/etc.
        // biome, before this test's override above) spawns nothing — the pass
        // is a no-op rather than falling back to some default species, even at
        // a chunk position that would otherwise pass the probability draw.
        mc::gameplay::NaturalSpawner emptySpawner(0x9E3779B9U);
        for (int index = 0; index < static_cast<int>(world::gen::Biome::Count); ++index) {
            emptySpawner.spawnTables().set(static_cast<world::gen::Biome>(index),
                                           MobCategory::Creature, {});
        }
        mc::gameplay::EntitySystem emptyEntities;
        emptySpawner.spawnForChunkGeneration(genWorld, emptyEntities, 0xC0FFEEULL, -2, 3);
        assert(emptyEntities.entities().empty());

        // Biome selection is wired to *this chunk's own* biome (vanilla's
        // getRandomSpawnMobAt reading worldGenRegion.getCenter()), not a
        // hardcoded one and not a neighbour's: give exactly one biome a table
        // and confirm the pass only produces something at chunk positions
        // whose real biome (queried independently through BiomeSource, the
        // same source spawnForChunkGeneration itself is built on — see
        // NaturalSpawner::NaturalSpawner) is that biome, and never elsewhere.
        // This is the test the "every biome forced to the same table" cases
        // above cannot catch: with a uniform table, spawning the wrong
        // biome's list is indistinguishable from spawning the right one.
        // Seed 16 is a fixed point (confirmed offline) whose ±4-chunk window
        // contains both a Plains and an Ocean chunk, so both arms of the
        // comparison below are reachable without widening the search.
        world::gen::BiomeSource independentBiomes{16U};
        int landChunkX = 0;
        int landChunkZ = 0;
        int oceanChunkX = 0;
        int oceanChunkZ = 0;
        bool haveLand = false;
        bool haveOcean = false;
        for (int cx = -4; cx <= 4 && !(haveLand && haveOcean); ++cx) {
            for (int cz = -4; cz <= 4 && !(haveLand && haveOcean); ++cz) {
                const auto found = independentBiomes.biomeAtBlock(cx * 16 + 8, cz * 16 + 8);
                if (!haveLand && found == world::gen::Biome::Plains) {
                    landChunkX = cx;
                    landChunkZ = cz;
                    haveLand = true;
                } else if (!haveOcean &&
                          (found == world::gen::Biome::Ocean ||
                           found == world::gen::Biome::DeepOcean)) {
                    oceanChunkX = cx;
                    oceanChunkZ = cz;
                    haveOcean = true;
                }
            }
        }
        // Both a Plains and an Ocean chunk must exist within the probed
        // window for this seed, or the test cannot tell "right biome" from
        // "wrong biome" — widen the window rather than silently pass.
        assert(haveLand && haveOcean);

        mc::gameplay::NaturalSpawner plainsOnlySpawner(16U);
        for (int index = 0; index < static_cast<int>(world::gen::Biome::Count); ++index) {
            plainsOnlySpawner.spawnTables().set(static_cast<world::gen::Biome>(index),
                                                MobCategory::Creature, {});
        }
        plainsOnlySpawner.spawnTables().set(
            world::gen::Biome::Plains, MobCategory::Creature,
            {{mc::gameplay::entities::entityTypeRegistry().byId("pig"), 10, 4, 4}});

        // A big flat world under every probed chunk, so a spawn that does
        // happen always has somewhere to land regardless of which position is
        // being tested this iteration.
        mc::world::World biomeTestWorld;
        for (int chunkZ = -5; chunkZ <= 5; ++chunkZ) {
            for (int chunkX = -5; chunkX <= 5; ++chunkX) {
                mc::world::Chunk chunk;
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        chunk.setBlock(x, 0, z, mc::world::Block::Stone);
                    }
                }
                biomeTestWorld.setChunk({chunkX, chunkZ}, std::move(chunk));
            }
        }

        // At the Ocean chunk, only Plains carries a species: nothing spawns.
        mc::gameplay::EntitySystem oceanAttempt;
        plainsOnlySpawner.spawnForChunkGeneration(biomeTestWorld, oceanAttempt, 0xC0FFEEULL,
                                                  oceanChunkX, oceanChunkZ);
        assert(oceanAttempt.entities().empty());

        // At the Plains chunk (same seed, same table), the pass can spawn —
        // scan a handful of world seeds so the assertion is not itself at the
        // mercy of one seed's probability draw landing zero.
        bool spawnedAtPlains = false;
        for (std::uint64_t trialSeed = 1U; trialSeed < 40U && !spawnedAtPlains; ++trialSeed) {
            mc::gameplay::EntitySystem plainsAttempt;
            plainsOnlySpawner.spawnForChunkGeneration(biomeTestWorld, plainsAttempt, trialSeed,
                                                      landChunkX, landChunkZ);
            if (!plainsAttempt.entities().empty()) {
                spawnedAtPlains = true;
            }
        }
        assert(spawnedAtPlains);
    }

    return 0;
}

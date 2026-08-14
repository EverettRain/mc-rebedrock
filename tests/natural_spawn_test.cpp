#include "gameplay/Difficulty.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/NaturalSpawner.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/gen/Biome.hpp"

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

    return 0;
}

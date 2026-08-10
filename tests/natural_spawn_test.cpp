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
    assert(earlySpawner.table(world::gen::Biome::Plains).creatures.empty());

    mc::gameplay::entities::registerBuiltinEntities();
    earlySpawner.setSeed(0x1234ABCDU);
    assert(!earlySpawner.table(world::gen::Biome::Plains).creatures.empty());

    auto world = makeFlatWorld();

    // The tables derive from the registry: land biomes host the passive species,
    // oceans never do, and every biome can host a MONSTER.
    mc::gameplay::NaturalSpawner spawner(0x1234ABCDU);
    assert(!spawner.table(world::gen::Biome::Plains).creatures.empty());
    assert(spawner.table(world::gen::Biome::Ocean).creatures.empty());
    assert(!spawner.table(world::gen::Biome::Ocean).monsters.empty());
    bool hasPig = false;
    bool hasCow = false;
    for (const auto& entry : spawner.table(world::gen::Biome::Plains).creatures) {
        hasPig = hasPig || entry.type.id().matches("pig");
        hasCow = hasCow || entry.type.id().matches("cow");
    }
    assert(hasPig && hasCow);

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

    // Day/night: the stored sky light is static full sun, so the day/night sky
    // brightness decides. A full-sun surface is not dark by day (no MONSTERs),
    // and the same surface at night goes dark and spawns MONSTERs — the check is
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
                            mc::gameplay::Difficulty::Normal, 1.0F);
        }
        assert(countCategory(dayEntities, gameplay::entities::MobCategory::Monster) == 0U);

        mc::gameplay::EntitySystem nightEntities;
        mc::gameplay::NaturalSpawner nightSpawner(0x0BADF00DU);
        for (int tick = 0; tick < 400; ++tick) {
            nightSpawner.tick(litWorld, nightEntities, player, 64.0F,
                              mc::gameplay::Difficulty::Normal, 0.0F);
        }
        assert(countCategory(nightEntities, gameplay::entities::MobCategory::Monster) > 0U);
        assert(countInsideRing(nightEntities, player) == 0U);
    }

    return 0;
}

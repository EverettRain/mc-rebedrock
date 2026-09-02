#include "gameplay/Difficulty.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/NaturalSpawner.hpp"
#include "gameplay/SpawnPlacements.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/gen/Biome.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

// SpawnPlacements + the rule chain's position sampling.
//
// The thing under test is the half of C2' the data pass left open: a species
// says what kind of cell it is born in, and the spawner rolls a *height* inside
// a column instead of scanning down to the roof. Before this, ON_GROUND at the
// surface was the only shape a spawn could take, so a cave and an ocean were
// both permanently empty.

namespace {

using mc::gameplay::entities::MobCategory;
using mc::gameplay::entities::SpawnPlacement;

// The smallest world that still holds the 24-block exclusion ring and a
// 48-block spawn disc: chunks -3..3 span -48..47.
constexpr int kChunkMin = -3;
constexpr int kChunkMax = 3;
constexpr int kBlockMin = kChunkMin * 16;
constexpr int kBlockMax = (kChunkMax + 1) * 16;
constexpr float kRadius = 48.0F;

// A species needs behaviour to be built at all (EntityType::Builder::create
// takes the AI the way registering an EntityType takes an EntityFactory). These
// creatures only ever have to exist, so the brain stays empty.
class InertAi final : public mc::gameplay::entities::EntityAi {
  public:
    void configureBrain(mc::gameplay::entities::MobBrain& brain) const override {
        static_cast<void>(brain);
    }
};
const InertAi kInertAi;

// A WATER_CREATURE registered IN_WATER, which is what a squid is in 26.1. The
// game has no water species yet, so the placement needs one to be exercised at
// all — the alternative is a code path with no caller.
const mc::gameplay::entities::EntityType& waterSpecies() {
    static const mc::gameplay::entities::EntityType type =
        mc::gameplay::entities::EntityType::Builder::create(MobCategory::WaterCreature, kInertAi)
            .spawnPlacement(SpawnPlacement::InWater)
            .sized(0.8F, 0.8F)
            .health(10.0F)
            .vanillaName("squid")
            .build("squid");
    return type;
}

mc::world::World makeWorld() {
    mc::world::World world;
    for (int chunkZ = kChunkMin; chunkZ <= kChunkMax; ++chunkZ) {
        for (int chunkX = kChunkMin; chunkX <= kChunkMax; ++chunkX) {
            world.setChunk({chunkX, chunkZ}, mc::world::Chunk{});
        }
    }
    return world;
}

void fillLayers(mc::world::World& world, int fromY, int toY, mc::world::Block block) {
    for (int y = fromY; y <= toY; ++y) {
        for (int z = kBlockMin; z < kBlockMax; ++z) {
            for (int x = kBlockMin; x < kBlockMax; ++x) {
                assert(world.setBlock(x, y, z, block));
            }
        }
    }
}

void lightLayers(mc::world::World& world, int fromY, int toY, std::uint8_t sky,
                 std::uint8_t blockLight) {
    for (int y = fromY; y <= toY; ++y) {
        for (int z = kBlockMin; z < kBlockMax; ++z) {
            for (int x = kBlockMin; x < kBlockMax; ++x) {
                // The setters report whether the nibble *changed*, so writing a
                // zero over a zero is a legitimate false.
                world.setSkyLight(x, y, z, sky);
                world.setBlockLight(x, y, z, blockLight);
            }
        }
    }
}

std::size_t countCategory(const mc::gameplay::EntitySystem& entities, MobCategory category) {
    std::size_t count = 0U;
    for (const auto& entity : entities.entities()) {
        if (!entity.dead() && entity.kind().category() == category) {
            ++count;
        }
    }
    return count;
}

// Installs one species as the whole table for its category, in every biome, so
// a placement test never also depends on which biome the seed happened to put
// under the disc.
void installEverywhere(mc::gameplay::NaturalSpawner& spawner, MobCategory category,
                       const mc::gameplay::SpawnerData& entry) {
    for (int index = 0; index < static_cast<int>(mc::world::gen::Biome::Count); ++index) {
        spawner.spawnTables().set(static_cast<mc::world::gen::Biome>(index), category, {entry});
    }
}

} // namespace

int main() {
    using namespace mc;
    using mc::gameplay::isSpawnPositionOk;

    gameplay::entities::registerBuiltinEntities();
    gameplay::biomeSpawnTables().loadBuiltinDefaults();

    const auto noon = gameplay::EnvironmentSnapshot::resolve(6000.0, 0.0F, 0.0F);
    const glm::vec3 player{0.0F, 25.0F, 0.0F};

    // --- isSpawnPositionOk, the four placements ---
    {
        auto probe = makeWorld();
        // Stone floor at y=0, a two-cell gap, a stone ceiling at y=3.
        fillLayers(probe, 0, 0, world::Block::Stone);
        fillLayers(probe, 3, 3, world::Block::Stone);

        // ON_GROUND: a valid floor below and two clear cells for the body.
        assert(isSpawnPositionOk(probe, {0, 1, 0}, SpawnPlacement::OnGround));
        // Inside the floor: the cell itself is a full collision cube.
        assert(!isSpawnPositionOk(probe, {0, 0, 0}, SpawnPlacement::OnGround));
        // Nothing to stand on.
        assert(!isSpawnPositionOk(probe, {0, 2, 0}, SpawnPlacement::OnGround));
        // A one-block crawlspace: the floor is fine but the head cell is rock.
        assert(probe.setBlock(0, 2, 0, world::Block::Stone));
        assert(!isSpawnPositionOk(probe, {0, 1, 0}, SpawnPlacement::OnGround));
        assert(probe.setBlock(0, 2, 0, world::Block::Air));

        // NO_RESTRICTIONS takes every one of those, including solid rock.
        assert(isSpawnPositionOk(probe, {0, 0, 0}, SpawnPlacement::NoRestrictions));
        assert(isSpawnPositionOk(probe, {0, 2, 0}, SpawnPlacement::NoRestrictions));

        // isValidSpawn's floor rule, both halves. A canopy is collision geometry
        // but never a spawn surface, and a light source of 14 or more is not one
        // either (BlockState#isValidSpawn's `getLightEmission() < 14`).
        assert(probe.setBlock(0, 0, 0, world::Block::OakLeaves));
        assert(!isSpawnPositionOk(probe, {0, 1, 0}, SpawnPlacement::OnGround));
        assert(probe.setBlock(0, 0, 0, world::Block::Glowstone));
        assert(!isSpawnPositionOk(probe, {0, 1, 0}, SpawnPlacement::OnGround));
        assert(probe.setBlock(0, 0, 0, world::Block::Stone));
        assert(isSpawnPositionOk(probe, {0, 1, 0}, SpawnPlacement::OnGround));

        // IN_WATER: the cell is water and nothing conductive caps it.
        assert(!isSpawnPositionOk(probe, {0, 1, 0}, SpawnPlacement::InWater));
        assert(probe.setBlock(0, 1, 0, world::Block::Water));
        assert(isSpawnPositionOk(probe, {0, 1, 0}, SpawnPlacement::InWater));
        assert(probe.setBlock(0, 2, 0, world::Block::Stone));
        assert(!isSpawnPositionOk(probe, {0, 1, 0}, SpawnPlacement::InWater));
        // Glass is a full cube but not a redstone conductor, so it does not cap.
        assert(probe.setBlock(0, 2, 0, world::Block::Glass));
        assert(isSpawnPositionOk(probe, {0, 1, 0}, SpawnPlacement::InWater));
        // Water is not ground: an ON_GROUND species is refused the same cell.
        assert(!isSpawnPositionOk(probe, {0, 1, 0}, SpawnPlacement::OnGround));

        // IN_LAVA.
        assert(!isSpawnPositionOk(probe, {0, 1, 0}, SpawnPlacement::InLava));
        assert(probe.setBlock(0, 1, 0, world::Block::Lava));
        assert(isSpawnPositionOk(probe, {0, 1, 0}, SpawnPlacement::InLava));
        // Lava is a full collision cube in this game, so it is also the cell an
        // ON_GROUND species must never be born in.
        assert(probe.setBlock(0, 2, 0, world::Block::Air));
        assert(!isSpawnPositionOk(probe, {0, 1, 0}, SpawnPlacement::OnGround));
    }

    // The default placement is ON_GROUND, which is why no land species restates
    // it — the same way vanilla only registers the exceptions.
    assert(gameplay::entities::entityTypeRegistry().byId("pig")->spawnPlacement() ==
           SpawnPlacement::OnGround);
    assert(gameplay::entities::entityTypeRegistry().byId("zombie")->spawnPlacement() ==
           SpawnPlacement::OnGround);
    assert(waterSpecies().spawnPlacement() == SpawnPlacement::InWater);

    // --- a cave spawns monsters; the lit surface above it does not ---
    // Stone to y=24 with a two-cell cavity at y=12..13, and full sun on the
    // roof. Both the cavity floor and the surface are valid ON_GROUND cells, so
    // the only thing separating them is light.
    {
        auto caveWorld = makeWorld();
        fillLayers(caveWorld, 0, 24, world::Block::Stone);
        fillLayers(caveWorld, 12, 13, world::Block::Air);
        lightLayers(caveWorld, 25, 26, 15U, 0U);

        gameplay::EntitySystem entities;
        gameplay::NaturalSpawner spawner;
        for (int tick = 0; tick < 1200; ++tick) {
            spawner.tick(caveWorld, entities, player, kRadius, gameplay::Difficulty::Normal, noon);
        }
        const std::size_t monsters = countCategory(entities, MobCategory::Monster);
        assert(monsters > 0U);
        // Every one of them stands on the cavity floor. The old spawner scanned
        // down to the first standing surface, so this count was exactly zero and
        // the surface — lit, in broad daylight — was the only place it looked.
        for (const auto& entity : entities.entities()) {
            if (entity.kind().category() != MobCategory::Monster) {
                continue;
            }
            assert(entity.position.y >= 12.0F && entity.position.y < 13.0F);
        }

        // The light threshold still decides. Light the cavity and it goes quiet:
        // a torch-lit cave is spawn-proof, which is the whole point of torches.
        auto litCaveWorld = makeWorld();
        fillLayers(litCaveWorld, 0, 24, world::Block::Stone);
        fillLayers(litCaveWorld, 12, 13, world::Block::Air);
        lightLayers(litCaveWorld, 25, 26, 15U, 0U);
        lightLayers(litCaveWorld, 12, 13, 0U, 15U);

        gameplay::EntitySystem litEntities;
        gameplay::NaturalSpawner litSpawner;
        for (int tick = 0; tick < 1200; ++tick) {
            litSpawner.tick(litCaveWorld, litEntities, player, kRadius,
                            gameplay::Difficulty::Normal, noon);
        }
        assert(countCategory(litEntities, MobCategory::Monster) == 0U);
    }

    // --- water spawns an IN_WATER species, and only that species ---
    // Stone to y=9, water to y=14, open air above, the column lit through.
    {
        auto seaWorld = makeWorld();
        fillLayers(seaWorld, 0, 9, world::Block::Stone);
        fillLayers(seaWorld, 10, 14, world::Block::Water);
        lightLayers(seaWorld, 10, 15, 15U, 0U);

        const glm::vec3 swimmer{0.0F, 12.0F, 0.0F};
        gameplay::EntitySystem entities;
        gameplay::NaturalSpawner spawner;
        installEverywhere(spawner, MobCategory::WaterCreature, {&waterSpecies(), 10, 4, 4});
        for (int tick = 0; tick < 1200; ++tick) {
            spawner.tick(seaWorld, entities, swimmer, kRadius, gameplay::Difficulty::Normal, noon);
        }
        assert(countCategory(entities, MobCategory::WaterCreature) > 0U);
        for (const auto& entity : entities.entities()) {
            if (entity.kind().category() != MobCategory::WaterCreature) {
                continue;
            }
            assert(entity.position.y >= 10.0F && entity.position.y < 15.0F);
        }
        // Nothing ON_GROUND belongs in a flooded world: the sea floor's cells are
        // all water and the water's surface is not a floor. Both built-in land
        // categories stay at zero.
        assert(countCategory(entities, MobCategory::Creature) == 0U);
        assert(countCategory(entities, MobCategory::Monster) == 0U);
    }

    // --- the surface has not regressed ---
    // A lit flat plain still fills with CREATUREs standing on the ground, which
    // is the behaviour the random-height sample had to keep while gaining the
    // two above.
    {
        auto plainWorld = makeWorld();
        fillLayers(plainWorld, 0, 0, world::Block::Stone);
        lightLayers(plainWorld, 1, 2, 15U, 0U);

        const glm::vec3 walker{0.0F, 1.0F, 0.0F};
        gameplay::EntitySystem entities;
        gameplay::NaturalSpawner spawner;
        installEverywhere(spawner, MobCategory::Creature,
                          {gameplay::entities::entityTypeRegistry().byId("pig"), 10, 4, 4});
        for (int tick = 0; tick < 1200; ++tick) {
            spawner.tick(plainWorld, entities, walker, kRadius, gameplay::Difficulty::Normal, noon);
        }
        assert(countCategory(entities, MobCategory::Creature) > 0U);
        for (const auto& entity : entities.entities()) {
            assert(entity.position.y >= 1.0F && entity.position.y < 2.0F);
            // Every member of every group was validated with its whole AABB, so
            // none of them is inside a block.
            assert(gameplay::EntitySystem::canOccupy(plainWorld, entity.position,
                                                     entity.dimensions()));
        }
    }

    return 0;
}

// DIM-1: the per-dimension Level bundle and the access routing (headless).
//
// Two things this proves:
//   1. Level is a value-member bundle (no heap object graph, no vtable) whose
//      world reference and clock id route correctly — the DOD shape DIM-1 owes.
//   2. GameSession's per-dimension systems now live in the primary Level, and
//      every legacy accessor (worldEntities/itemEntities/weatherSystem) routes
//      to that same Level object — the "singularity hoist" is wired, and the
//      routing is identity, not a copy, so behaviour is unchanged.
#include "gameplay/Difficulty.hpp"
#include "gameplay/GameSession.hpp"  // SimulationHost lives here
#include "gameplay/Level.hpp"
#include "gameplay/entities/CowEntity.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "runtime/GameRuntime.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkStreamer.hpp"
#include "world/Dimension.hpp"
#include "world/World.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <type_traits>

using mc::gameplay::Level;
using mc::world::DimensionId;
using mc::world::kDimensionCount;

namespace {

// The smallest host that satisfies SimulationHost: this test never ticks, it
// only inspects structure, so every callback is a no-op.
struct SilentHost final : public mc::gameplay::SimulationHost {
    void submitWorldEdit(int, int, int, mc::world::Block, std::uint8_t,
                         std::optional<mc::world::BlockOrientation>) override {}
    void submitWorldStateEdit(int, int, int, mc::world::BlockState) override {}
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(mc::world::Block, glm::vec3) override {}
    void playItemPickup(glm::vec3) override {}
    void playEat(glm::vec3) override {}
    void playPlayerHurt(glm::vec3) override {}
    void playPlayerFall(glm::vec3, bool) override {}
    void playBurp(glm::vec3) override {}
    void playCreatureHurt(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playFootstep(mc::world::Block, glm::vec3, float) override {}
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, mc::world::Block) override {}
    void onPlayerDied() override {}
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {}
    void onEatingCancelled() override {}
};

// A small flat stone floor at y=0, the mob_brain_test pattern, so ticked
// creatures have ground to stand and wander on.
mc::world::World makeFlatWorld() {
    mc::world::World world;
    for (int chunkX = 0; chunkX < 2; ++chunkX) {
        for (int chunkZ = 0; chunkZ < 2; ++chunkZ) {
            mc::world::Chunk chunk;
            for (int x = 0; x < 16; ++x) {
                for (int z = 0; z < 16; ++z) {
                    chunk.setBlock(x, 0, z, mc::world::Block::Stone);
                }
            }
            world.setChunk({chunkX, chunkZ}, std::move(chunk));
        }
    }
    return world;
}

// A stable digest of every creature's pose, quantised so float noise does not
// make it brittle: a tick-order swap or a perturbed RNG stream moves creatures
// and changes this number.
std::uint64_t poseDigest(const mc::gameplay::EntitySystem& entities) {
    std::uint64_t digest = 1469598103934665603ULL;  // FNV-1a offset basis
    const auto mix = [&](std::int64_t value) {
        digest ^= static_cast<std::uint64_t>(value);
        digest *= 1099511628211ULL;  // FNV-1a prime
    };
    for (const auto& entity : entities.entities()) {
        mix(std::llround(entity.position.x * 1000.0F));
        mix(std::llround(entity.position.y * 1000.0F));
        mix(std::llround(entity.position.z * 1000.0F));
        mix(std::llround(entity.yaw * 1000.0F));
    }
    return digest;
}

}  // namespace

int main() {
    // Register species first: a NaturalSpawner built before the entity registry
    // is populated prints a (harmless here) "no mob will ever spawn" diagnostic,
    // and this test constructs several spawners.
    mc::gameplay::entities::registerBuiltinEntities();

    // --- Level is a value bundle, not an object graph -------------------------
    // Sabotage ②'s guard: had Level been rebuilt as a unique_ptr<graph> + virtual
    // interface, it would not be a trivially-relocatable value aggregate and the
    // systems would not be reachable as direct value members.
    static_assert(std::is_standard_layout_v<DimensionId>);
    static_assert(std::is_same_v<decltype(std::declval<Level&>().entities),
                                 mc::gameplay::EntitySystem>,
                  "Level::entities is a value member, not a pointer to a graph");
    static_assert(std::is_same_v<decltype(std::declval<Level&>().items),
                                 mc::gameplay::ItemEntitySystem>,
                  "Level::items is a value member");
    static_assert(std::is_same_v<decltype(std::declval<Level&>().weather),
                                 mc::gameplay::WeatherSystem>,
                  "Level::weather is a value member");
    static_assert(std::is_move_constructible_v<Level>,
                  "Level moves wholesale into the level table (value bundle)");

    // --- Level in isolation: world binding + clock alignment ------------------
    {
        Level level;
        level.id = DimensionId::Overworld;
        assert(!level.hasWorld());  // a fresh level has no world bound yet
        mc::world::World world;
        level.bindWorld(world);
        assert(level.hasWorld());
        assert(&level.world() == &world);  // world() is the bound reference
        // The clock id is index-aligned with the dimension id.
        assert(level.clockId() == mc::world::ClockId::Overworld);

        Level nether;
        nether.id = DimensionId::Nether;
        assert(nether.clockId() == mc::world::ClockId::Nether);
    }

    // --- GameSession: the singleton hoist is wired ---------------------------
    const auto saveRoot = std::filesystem::temp_directory_path() / "level-test";
    std::filesystem::remove_all(saveRoot);
    std::filesystem::create_directories(saveRoot);

    mc::world::ChunkStreamer streamer{0U, 4, 4};
    SilentHost host;
    mc::runtime::GameRuntime runtime{host, streamer, saveRoot};
    auto& session = runtime.gameSession();

    // The level table is sized for every dimension (DIM-2 walks it), but only the
    // Overworld is the player's for now.
    assert(session.primaryDimension() == DimensionId::Overworld);
    static_assert(kDimensionCount == 3U, "table holds Overworld/Nether/End");

    // The runtime bound its World into the primary level (DIM-1 wiring).
    assert(session.primaryLevel().hasWorld());
    assert(&session.primaryLevel().world() == &runtime.world());
    assert(session.primaryLevel().id == DimensionId::Overworld);

    // Every legacy accessor routes to the primary level's systems — identity, not
    // a copy. Sabotage ①'s guard: a caller still pointing at a stale standalone
    // singleton would fail these address comparisons.
    assert(&session.worldEntities() == &session.primaryLevel().entities);
    assert(&session.itemEntities() == &session.primaryLevel().items);
    assert(&session.weatherSystem() == &session.primaryLevel().weather);

    // primaryLevel() and level(Overworld) name the same object while the player
    // is in the Overworld.
    assert(&session.primaryLevel() == &session.level(DimensionId::Overworld));
    // The Nether/End levels exist and are distinct objects (dormant for now).
    assert(&session.level(DimensionId::Nether) != &session.level(DimensionId::Overworld));
    assert(&session.level(DimensionId::End) != &session.level(DimensionId::Nether));

    // --- Equivalence: a Level ticks its creatures deterministically ----------
    // DIM-1 is a pure refactor: ticking creatures through the hoisted Level bundle
    // must reproduce byte-for-byte what the old exclusive EntitySystem did. This
    // pins the outcome to a golden digest — Sabotage ③'s guard: a silent tick-
    // order change or a perturbed RNG stream moves the herd and breaks it. The
    // digest is quantised, so it is stable across platforms but sensitive to any
    // real behavioural drift.
    {
        mc::gameplay::Level a;
        a.id = DimensionId::Overworld;
        auto worldA = makeFlatWorld();
        a.bindWorld(worldA);
        // Fixed spawn seeds -> reproducible wander RNG (mob_brain_test pattern).
        a.entities.spawn({8.0F, 1.001F, 8.0F}, mc::gameplay::entities::CowEntity::type(), 11U);
        a.entities.spawn({12.0F, 1.001F, 8.0F}, mc::gameplay::entities::CowEntity::type(), 12U);
        for (int t = 0; t < 40; ++t) {
            static_cast<void>(a.entities.tick(a.world(), glm::vec3{100.0F, 1.0F, 100.0F},
                                              0.6F, 1.8F, mc::gameplay::Difficulty::Normal));
        }
        const std::uint64_t digestA = poseDigest(a.entities);

        // Same seeds, same ticks, a fresh Level -> identical result (determinism:
        // the per-Level RNG stream is self-contained, DIM DESIGN §3.5).
        mc::gameplay::Level b;
        b.id = DimensionId::Overworld;
        auto worldB = makeFlatWorld();
        b.bindWorld(worldB);
        b.entities.spawn({8.0F, 1.001F, 8.0F}, mc::gameplay::entities::CowEntity::type(), 11U);
        b.entities.spawn({12.0F, 1.001F, 8.0F}, mc::gameplay::entities::CowEntity::type(), 12U);
        for (int t = 0; t < 40; ++t) {
            static_cast<void>(b.entities.tick(b.world(), glm::vec3{100.0F, 1.0F, 100.0F},
                                              0.6F, 1.8F, mc::gameplay::Difficulty::Normal));
        }
        const std::uint64_t digestB = poseDigest(b.entities);

        assert(digestA == digestB);  // two runs, same seed -> same herd
        // The golden value the current (correct) refactor produces. Captured from
        // this build; a behavioural regression (tick order / RNG stream) changes it.
        assert(digestA == 0xB4525A6586129FEDULL);
        // The herd actually moved (the digest is not the empty/degenerate value).
        assert(digestA != 1469598103934665603ULL);
    }

    std::filesystem::remove_all(saveRoot);
    return 0;
}

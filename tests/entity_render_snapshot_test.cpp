#include "gameplay/EntityRenderSnapshot.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/entities/PigEntity.hpp"
#include "gameplay/entities/ZombieEntity.hpp"

#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <cstdint>
#include <optional>
#include <vector>
#include <stdexcept>
#include <string>

// P3 Step 5: creatures are drawn from a snapshot the tick publishes, not from
// the live entity vector. The vector is the simulation's working set — the tick
// reorders it, compacts it and resizes it — so a draw pass holding references
// into it is only safe while the two run on the same thread. What is pinned
// here is that the snapshot really is a decoupled copy, and that it carries
// what the draw pass actually reads.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"entity_render_snapshot_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using namespace mc;

struct SilentHost final : gameplay::SimulationHost {
    void submitWorldEdit(int, int, int, world::Block, std::uint8_t,
                         std::optional<world::BlockOrientation>) override {}
    void submitWorldStateEdit(int, int, int, world::BlockState) override {}
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(world::Block, glm::vec3) override {}
    void playItemPickup(glm::vec3) override {}
    void playEat(glm::vec3) override {}
    void playPlayerHurt(glm::vec3) override {}
    void playPlayerFall(glm::vec3, bool) override {}
    void playBurp(glm::vec3) override {}
    void playCreatureHurt(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playFootstep(world::Block, glm::vec3, float) override {}
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, world::Block) override {}
    void onPlayerDied() override {}
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {}
    void onEatingCancelled() override {}
};

void buildFloor(world::World& world) {
    world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, world::Block::Stone);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
}

} // namespace

int main() {
    const std::vector<gameplay::ItemEntity> noItems;
    const std::vector<gameplay::FallingBlockEntity> noFallingBlocks;

    // --- Capturing copies what the draw pass reads, keyed by the stable id. ---
    {
        world::World world;
        buildFloor(world);
        gameplay::GameSession session;
        SilentHost host;
        session.worldEntities().spawn({4.5F, 1.0F, 4.5F},
                                      gameplay::entities::PigEntity::type(), 7U);
        session.worldEntities().spawn({6.5F, 1.0F, 6.5F},
                                      gameplay::entities::ZombieEntity::type(), 9U);
        session.tick(world, host);

        const auto& snapshot = session.entitySnapshot().entities();
        REQUIRE(snapshot.size() == 2U);
        for (const auto& state : snapshot) {
            REQUIRE(state.type != nullptr);
            REQUIRE(state.id != 0U);
        }
        // Both interpolation endpoints are carried; without the previous one the
        // renderer cannot interpolate and creatures snap between ticks.
        REQUIRE(snapshot[0].previousPosition.y > 0.0F);
    }

    // --- The snapshot is a copy, not a view. Mutating the source afterwards
    // must not change what the renderer would draw — that decoupling is the
    // entire point, and a snapshot that aliased the live vector would pass
    // every other check here while still crashing once the tick threads. ---
    {
        // SimpleEntity is deliberately non-copyable (its MobBrain owns goal
        // state), which is itself a reason the renderer cannot just hold one.
        std::vector<gameplay::SimpleEntity> live;
        auto& pig = live.emplace_back();
        pig.type = &gameplay::entities::PigEntity::type();
        pig.id = 41U;
        pig.position = {1.0F, 2.0F, 3.0F};
        pig.previousPosition = {1.0F, 2.0F, 2.0F};
        pig.yaw = 0.5F;
        pig.previousYaw = 0.25F;
        pig.walkDistance = 4.0F;
        pig.previousWalkDistance = 3.5F;
        pig.damage.hurtTicks = 6;
        pig.damage.deathTicks = 2;

        gameplay::EntityRenderSnapshot snapshot;
        snapshot.capture(live, noItems, noFallingBlocks);
        REQUIRE(snapshot.entities().size() == 1U);
        const auto& state = snapshot.entities().at(0);
        REQUIRE(state.id == 41U);
        REQUIRE(state.type == &gameplay::entities::PigEntity::type());
        REQUIRE(state.position == glm::vec3(1.0F, 2.0F, 3.0F));
        REQUIRE(state.previousPosition == glm::vec3(1.0F, 2.0F, 2.0F));
        REQUIRE(state.yaw == 0.5F);
        REQUIRE(state.previousYaw == 0.25F);
        REQUIRE(state.walkDistance == 4.0F);
        REQUIRE(state.previousWalkDistance == 3.5F);
        // The two DamageState fields the draw pass reads, and only those.
        REQUIRE(state.hurtTicks == 6);
        REQUIRE(state.deathTicks == 2);

        // Move the source, and drop it entirely: the snapshot is unaffected.
        live.at(0).position = {99.0F, 99.0F, 99.0F};
        live.clear();
        REQUIRE(snapshot.entities().size() == 1U);
        REQUIRE(snapshot.entities().at(0).position == glm::vec3(1.0F, 2.0F, 3.0F));

        // Re-capturing from an empty source is what makes a removed creature
        // stop being drawn.
        snapshot.capture(live, noItems, noFallingBlocks);
        REQUIRE(snapshot.empty());
    }

    // --- Re-capturing reuses the buffer and reflects the new population, which
    // is what happens every tick. ---
    {
        gameplay::EntityRenderSnapshot snapshot;
        std::vector<gameplay::SimpleEntity> live;
        for (std::uint64_t id = 1U; id <= 3U; ++id) {
            auto& entity = live.emplace_back();
            entity.type = &gameplay::entities::ZombieEntity::type();
            entity.id = id;
        }
        snapshot.capture(live, noItems, noFallingBlocks);
        REQUIRE(snapshot.entities().size() == 3U);
        live.pop_back();
        snapshot.capture(live, noItems, noFallingBlocks);
        REQUIRE(snapshot.entities().size() == 2U);
        REQUIRE(snapshot.entities().at(1).id == 2U);
    }


    // --- Items and falling blocks travel in the same snapshot, and are copies
    // too: the draw pass must not walk the simulation's live vectors for those
    // either. ---
    {
        gameplay::EntityRenderSnapshot snapshot;
        std::vector<gameplay::SimpleEntity> creatures;
        std::vector<gameplay::ItemEntity> items;
        auto& dropped = items.emplace_back();
        dropped.position = {3.0F, 4.0F, 5.0F};
        dropped.stack = {world::Block::Stone, 2U};
        std::vector<gameplay::FallingBlockEntity> falling;
        auto& sand = falling.emplace_back();
        sand.position = {6.0F, 7.0F, 8.0F};
        sand.block = world::Block::Sand;

        snapshot.capture(creatures, items, falling);
        REQUIRE(snapshot.items().size() == 1U);
        REQUIRE(snapshot.items().at(0).position == glm::vec3(3.0F, 4.0F, 5.0F));
        REQUIRE(snapshot.fallingBlocks().size() == 1U);
        REQUIRE(snapshot.fallingBlocks().at(0).block == world::Block::Sand);
        REQUIRE(!snapshot.empty());

        // Copies, not views.
        items.clear();
        falling.clear();
        REQUIRE(snapshot.items().size() == 1U);
        REQUIRE(snapshot.fallingBlocks().size() == 1U);

        snapshot.capture(creatures, items, falling);
        REQUIRE(snapshot.empty());
    }

    // --- The session publishes all three lists together, once per tick. ---
    {
        world::World world;
        buildFloor(world);
        gameplay::GameSession session;
        SilentHost host;
        session.itemEntities().spawn({4.5F, 2.0F, 4.5F}, {world::Block::Stone, 1U});
        session.tick(world, host);
        REQUIRE(session.entitySnapshot().items().size() == 1U);
    }
    return 0;
}

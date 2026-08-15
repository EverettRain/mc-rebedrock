// N1's interaction acceptance: the dig, place, bucket, attack and eat paths
// run headless through GameSession's command-driven PlayerInteraction, and a
// dig consumes the same whole number of ticks regardless of frame rate (the
// timeline is tick-owned, so there is no frame path left to drift).

#include "gameplay/GameSession.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/DayNightCycle.hpp"
#include "world/World.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <optional>

using namespace mc;

namespace {

struct TestHost final : gameplay::SimulationHost {
    int blockBreaks = 0;
    int blockPlaces = 0;
    int splashes = 0;
    int entityHits = 0;
    int itemBreaks = 0;
    int eatingStarted = 0;

    void submitWorldEdit(int, int, int, world::Block, std::uint8_t,
                         std::optional<world::BlockOrientation>) override {}
    void submitWorldStateEdit(int, int, int, world::BlockState) override {}
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(world::Block, glm::vec3) override { ++blockBreaks; }
    void playBlockHit(world::Block, glm::vec3) override {}
    void playBlockPlace(world::Block, glm::vec3) override { ++blockPlaces; }
    void playItemBreak(glm::vec3) override { ++itemBreaks; }
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
    void playSplash(glm::vec3, float) override { ++splashes; }
    void spawnBlockBreakParticles(glm::ivec3, world::Block) override {}
    void spawnWaterSplash(glm::vec3) override {}
    void onPlayerDied() override {}
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override { ++eatingStarted; }
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

// The whole-number tick count a dig of `block` with `stack` lands on, given the
// player is not underwater and not on the ground (the default floating spawn).
[[nodiscard]] std::uint64_t digTicks(gameplay::GameSession& session,
                                     const gameplay::ItemStack& stack, world::Block block) {
    const float duration = gameplay::miningSeconds(block, stack, session.player().inWater(),
                                                   !session.player().onGround());
    return static_cast<std::uint64_t>(std::ceil(static_cast<double>(duration) *
                                                world::DayNightCycle::kTicksPerSecond));
}

} // namespace

int main() {
    gameplay::entities::registerBuiltinEntities();

    // --- Dig: a StartDestroy command digs the block over a whole number of
    // ticks, and that count is the same however the frame rate would have
    // paced it (there is no frame path left). ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        world.setState(5, 1, 5, world::BlockState{world::Block::Stone});
        session.enqueueCommand(gameplay::PlayerAction{gameplay::PlayerAction::Kind::StartDestroy,
                                                      glm::ivec3{5, 1, 5}});
        const auto expected = digTicks(session, session.inventory().selectedStack(),
                                       world::Block::Stone);
        for (std::uint64_t tick = 0; tick < expected; ++tick) {
            session.tick(world, host);
        }
        // The block gave way exactly on the computed tick.
        assert(world.block(5, 1, 5) == world::Block::Air);
        assert(host.blockBreaks == 1);
        // The swing timeline ran with the dig (the arc restarts every ~half
        // swing, so the sequence advanced past the first swing).
        assert(session.playerActions().swing.sequence >= 1U);
    }

    // --- Place: a UseItemOn with a block stack in hand places the block. ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        session.inventory().mutableSlot(0) = {world::Block::Stone, 1U, nullptr};
        session.inventory().selectHotbar(0);
        gameplay::UseItemOn use;
        use.block = glm::ivec3{5, 1, 5};
        use.adjacent = glm::ivec3{5, 2, 5};
        use.face = world::BlockOrientation::Up;
        use.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(use));
        session.tick(world, host);
        assert(world.block(5, 2, 5) == world::Block::Stone);
        assert(host.blockPlaces == 1);
    }

    // --- Held dig hand-off: no release is needed between two instant blocks.
    // The renderer sends a new StartDestroy when the vanished first target lets
    // the ray reach the next cell; the interaction remains armed and accepts it
    // on the following tick. This is the grass/plant continuous-break path. ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        world.setState(5, 1, 5, world::BlockState{world::Block::Grass});
        world.setState(5, 1, 6, world::BlockState{world::Block::Grass});
        session.enqueueCommand(gameplay::PlayerAction{gameplay::PlayerAction::Kind::StartDestroy,
                                                      glm::ivec3{5, 1, 5}});
        session.tick(world, host);
        assert(world.block(5, 1, 5) == world::Block::Air);
        assert(session.interaction().destroying());

        // Same held mouse button, new ray target; importantly, no AbortDestroy.
        session.enqueueCommand(gameplay::PlayerAction{gameplay::PlayerAction::Kind::StartDestroy,
                                                      glm::ivec3{5, 1, 6}});
        // Creative keeps vanilla's five-tick destroy delay between blocks.
        for (int tick = 0; tick < 5; ++tick) {
            session.tick(world, host);
        }
        assert(world.block(5, 1, 6) == world::Block::Air);
        assert(host.blockBreaks == 2);
    }

    // --- Bucket: UseItemOn a water source with an empty bucket collects it. ---
    {
        TestHost host;
        gameplay::GameSession session;
        session.setGameMode(gameplay::GameMode::Survival);
        world::World world;
        buildFloor(world);
        world.setState(5, 1, 5, world::BlockState{world::Block::Water});
        session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Bucket};
        session.inventory().selectHotbar(0);
        gameplay::UseItemOn use;
        use.block = glm::ivec3{5, 1, 5};
        use.adjacent = glm::ivec3{5, 1, 5};
        use.face = world::BlockOrientation::Up;
        use.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};
        session.enqueueCommand(std::move(use));
        session.tick(world, host);
        assert(world.block(5, 1, 5) == world::Block::Air);
        assert(session.inventory().selectedStack().item == &gameplay::items::WaterBucket);
        assert(host.splashes == 1);
    }

    // --- Attack: a StartDestroy at a creature hits it once. ---
    {
        TestHost host;
        gameplay::GameSession session;
        world::World world;
        buildFloor(world);
        const auto* pigType = gameplay::entities::entityTypeRegistry().byId("pig");
        assert(pigType != nullptr);
        session.worldEntities().spawn({5.5F, 2.0F, 5.5F}, *pigType);
        // Find the spawned id.
        std::uint64_t pigId = 0U;
        for (const auto& entity : session.worldEntities().entities()) {
            if (entity.type != nullptr && std::string{entity.type->id().path} == "pig") {
                pigId = entity.id;
                break;
            }
        }
        assert(pigId != 0U);
        const float healthBefore =
            session.worldEntities().byId(pigId) != nullptr
                ? session.worldEntities().byId(pigId)->damage.health
                : 10.0F;
        gameplay::PlayerAction action;
        action.kind = gameplay::PlayerAction::Kind::StartDestroy;
        action.entity = true;
        action.entityId = pigId;
        session.enqueueCommand(std::move(action));
        session.tick(world, host);
        // A bare-hand attack deals one damage.
        assert(session.worldEntities().byId(pigId) != nullptr);
        assert(session.worldEntities().byId(pigId)->damage.health == healthBefore - 1.0F);
        assert(session.playerActions().swing.sequence == 1U);
    }

    // --- Eat: holding the use button runs the vanilla 32-tick meal. ---
    {
        TestHost host;
        gameplay::GameSession session;
        session.setGameMode(gameplay::GameMode::Creative);
        session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Apple};
        session.inventory().selectHotbar(0);
        session.enqueueCommand(gameplay::UseItem{});
        world::World world;
        buildFloor(world);
        for (int tick = 0; tick < gameplay::GameSession::kEatTicks; ++tick) {
            session.tick(world, host);
        }
        session.enqueueCommand(gameplay::UseItemStop{});
        session.tick(world, host);
        assert(host.eatingStarted == 1);
        assert(!session.eating());
    }

    // --- Attacking cancels a meal (Minecraft#doAttack interrupts eating). ---
    {
        TestHost host;
        gameplay::GameSession session;
        session.inventory().mutableSlot(0) = {world::Block::Air, 1U, &gameplay::items::Apple};
        session.inventory().selectHotbar(0);
        session.enqueueCommand(gameplay::UseItem{});
        world::World world;
        buildFloor(world);
        session.tick(world, host);  // the meal starts
        assert(session.eating());
        gameplay::PlayerAction action;
        action.kind = gameplay::PlayerAction::Kind::StartDestroy;  // attack nothing
        session.enqueueCommand(std::move(action));
        session.tick(world, host);
        assert(!session.eating());  // the attack cancelled the meal
    }

    return 0;
}

// RW-1: bow + arrow — charge draw (right-click reuses PlayerActionState::
// startUsing, UseAnimation::Bow) then release spawns a real arrow projectile
// through RW-0's ProjectileSystem/GameSession::spawnProjectile seam. Headless,
// command-driven — no Vulkan, no window; every interaction goes through
// GameSession::enqueueCommand + GameSession::tick exactly like
// equipment_wear_test.cpp / player_interaction_test.cpp.

#include "gameplay/GameSession.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

using namespace mc;
using namespace mc::gameplay;

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"bow_arrow_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

struct TestHost final : SimulationHost {
    bool playerDied = false;
    void submitWorldEdit(int, int, int, world::Block, std::uint8_t,
                         std::optional<world::BlockOrientation>) override {}
    void submitWorldStateEdit(int, int, int, world::BlockState) override {}
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(world::Block, glm::vec3) override {}
    void playBlockHit(world::Block, glm::vec3) override {}
    void playBlockPlace(world::Block, glm::vec3) override {}
    void playItemBreak(glm::vec3) override {}
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
    void spawnWaterSplash(glm::vec3) override {}
    void onPlayerDied() override { playerDied = true; }
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

// Arms an already-constructed session with a bow in hand (hotbar slot 0)
// and, unless the caller clears it, one stack of arrows in slot 1 — the
// common "can shoot" starting point. GameSession has no move/copy (it owns a
// mutex), so callers construct it in place and pass it here rather than
// having this return one by value.
void armSession(GameSession& session, world::World& world, bool withArrow = true) {
    buildFloor(world);
    session.setGameMode(GameMode::Survival);
    session.player().setPosition({5.5F, 5.0F, 5.5F});
    session.inventory().mutableSlot(0) = ItemStack{world::Block::Air, 1U, &items::Bow};
    if (withArrow) {
        session.inventory().mutableSlot(1) = ItemStack{world::Block::Air, 5U, &items::Arrow};
    }
    session.inventory().selectHotbar(0);
}

// --- Draw: right-clicking a bow with an arrow available starts the shared
// use timeline (UseAnimation::Bow), and its remaining-ticks countdown
// advances one tick at a time — draw progress increases with elapsed ticks,
// full at 20. ---
void testDrawProgressesAndReachesFullAt20Ticks() {
    TestHost host;
    world::World world;
    GameSession session;
    armSession(session, world);

    session.enqueueCommand(UseItem{});
    session.tick(world, host);
    REQUIRE(session.playerActions().use.active);
    REQUIRE(session.playerActions().use.animation == UseAnimation::Bow);
    // PlayerActionState::tick() advances BEFORE PlayerInteraction::tick() runs
    // (GameSession::tick's own ordering — the same shape beginEating/
    // tickEating already rely on), so the tick that STARTS the draw does not
    // itself count as an elapsed tick: remaining == duration right after
    // startUsing.
    const std::uint32_t durationAfterStart = session.playerActions().use.durationTicks;
    const std::uint32_t remainingAfterStart = session.playerActions().use.remainingTicks;
    REQUIRE(durationAfterStart - remainingAfterStart == 0U);

    // 20 more ticks reaches full pull progress (bowPullProgress(20) == 1.0).
    for (int i = 0; i < 20; ++i) {
        session.tick(world, host);
    }
    REQUIRE(session.playerActions().use.active);  // getMaxUseTime is 72000 — still drawing
    const std::uint32_t elapsed =
        session.playerActions().use.durationTicks - session.playerActions().use.remainingTicks;
    REQUIRE(elapsed == 20U);
    REQUIRE(bowPullProgress(elapsed) == 1.0F);
    REQUIRE(bowPullProgress(elapsed / 2U) < 1.0F);
    std::cout << "testDrawProgressesAndReachesFullAt20Ticks OK\n";
}

// --- Sabotage③ target: a non-creative player with NO arrow anywhere in the
// inventory must not even start the draw. ---
void testCannotDrawWithoutArrowInSurvival() {
    TestHost host;
    world::World world;
    GameSession session;
    armSession(session, world, /*withArrow=*/false);
    REQUIRE(!session.inventory().findFirstArrowSlot().has_value());

    session.enqueueCommand(UseItem{});
    session.tick(world, host);

    REQUIRE(!session.playerActions().use.active);
    std::cout << "testCannotDrawWithoutArrowInSurvival OK\n";
}

// Creative may draw with no arrow at all (BowItem#use's `bl` creative escape).
void testCreativeCanDrawWithoutArrow() {
    TestHost host;
    world::World world;
    GameSession session;
    armSession(session, world, /*withArrow=*/false);
    session.setGameMode(GameMode::Creative);

    session.enqueueCommand(UseItem{});
    session.tick(world, host);

    REQUIRE(session.playerActions().use.active);
    REQUIRE(session.playerActions().use.animation == UseAnimation::Bow);
    std::cout << "testCreativeCanDrawWithoutArrow OK\n";
}

// --- Release: draws to full (20 ticks), then releases — a real arrow
// projectile lands in RW-0's pool with the vanilla full-draw velocity length
// (3.0) and damage (ceil(3.0 * 2.0) == 6), and the critical flag set. Also
// the acceptance's "hit" requirement: it is a genuine ProjectileSystem entry
// (assert on the pool, not a private stub), which is what RW-0's own
// Damage.hpp-routed hit test then exercises exactly as projectile_system_test
// already proves for any projectile in this pool. ---
void testFullDrawSpawnsFullVelocityCriticalArrow() {
    TestHost host;
    world::World world;
    GameSession session;
    armSession(session, world);

    // 21 ticks total: the first STARTS the draw (0 elapsed, see
    // testDrawProgressesAndReachesFullAt20Ticks's comment), the following 20
    // bring elapsed to exactly 20 — bowPullProgress(20) == 1.0F, full draw.
    session.enqueueCommand(UseItem{});
    for (int i = 0; i < 21; ++i) {
        session.tick(world, host);
    }
    REQUIRE(session.projectiles().entities().empty());
    session.enqueueCommand(UseItemStop{});
    session.tick(world, host);

    REQUIRE(session.projectiles().entities().size() == 1U);
    const auto& arrow = session.projectiles().entities().front();
    REQUIRE(arrow.critical);
    REQUIRE(arrow.pickupItem.item == &items::Arrow);
    // 3.0 base velocity, scattered by the session's own deterministic
    // projectileRandom_ (RW-1a #16's spawn() triangle jitter) — so the length is
    // close to, not exactly, 3.0.
    const float speed = glm::length(arrow.velocity);
    REQUIRE(speed > 2.8F && speed < 3.2F);
    // RW-1a #8 — the projectile stores the arrow's BASE damage (2.0), not the
    // velocity-scaled value; the hit-time `ceil(velocity.length() * base)` (~6
    // at this ~3.0 launch speed) is derived in ProjectileSystem::tick, proven by
    // projectile_system_test's range-damage case.
    REQUIRE(arrow.damage == kArrowBaseDamage);
    REQUIRE(!session.playerActions().use.active);  // the draw ended
    std::cout << "testFullDrawSpawnsFullVelocityCriticalArrow OK\n";
}

// --- Sabotage① target: a partial draw must be strictly weaker (lower
// velocity/damage) and non-critical, never clamped up to the full-draw
// numbers. ---
void testPartialDrawSpawnsWeakerNonCriticalArrow() {
    TestHost host;
    world::World world;
    GameSession session;
    armSession(session, world);

    session.enqueueCommand(UseItem{});
    // Only 6 ticks of draw (well under the 20-tick full pull, and above the
    // 0.1 minimum-pull threshold so a shot still fires).
    for (int i = 0; i < 6; ++i) {
        session.tick(world, host);
    }
    session.enqueueCommand(UseItemStop{});
    session.tick(world, host);

    REQUIRE(session.projectiles().entities().size() == 1U);
    const auto& arrow = session.projectiles().entities().front();
    REQUIRE(!arrow.critical);
    const float speed = glm::length(arrow.velocity);
    REQUIRE(speed < 2.8F);  // strictly below the full-draw range
    // RW-1a #8 — a weak draw is weaker through its LOWER launch VELOCITY (the
    // hit-time `ceil(velocity.length() * base)` then lands softer), not through a
    // shrunken stored base: the base damage field is the same 2.0 for every
    // arrow now.
    REQUIRE(arrow.damage == kArrowBaseDamage);
    std::cout << "testPartialDrawSpawnsWeakerNonCriticalArrow OK\n";
}

// A tap-and-release under the 0.1 minimum pull progress fires nothing at all
// (BowItem.onStoppedUsing's `if (!(f < 0.1)) { ... }` guard).
void testBelowMinimumPullFiresNothing() {
    TestHost host;
    world::World world;
    GameSession session;
    armSession(session, world);

    session.enqueueCommand(UseItem{});
    session.tick(world, host);  // 1 tick elapsed: bowPullProgress(1) well under 0.1
    session.enqueueCommand(UseItemStop{});
    session.tick(world, host);

    REQUIRE(session.projectiles().entities().empty());
    // The arrow was never spent on a shot that did not fire.
    REQUIRE(session.inventory().slot(1).count == 5U);
    std::cout << "testBelowMinimumPullFiresNothing OK\n";
}

// --- Sabotage② target: a non-creative release must consume exactly one
// arrow; creative must consume none. Also checks the bow's own durability
// spends exactly one point per shot either way. ---
void testNonCreativeConsumesOneArrowAndDamagesBow() {
    TestHost host;
    world::World world;
    GameSession session;
    armSession(session, world);
    REQUIRE(session.inventory().slot(1).count == 5U);
    REQUIRE(session.inventory().selectedStack().damage == 0U);

    // 21 ticks: 1 to start the draw + 20 elapsed (see
    // testDrawProgressesAndReachesFullAt20Ticks) — a full draw.
    session.enqueueCommand(UseItem{});
    for (int i = 0; i < 21; ++i) {
        session.tick(world, host);
    }
    session.enqueueCommand(UseItemStop{});
    session.tick(world, host);

    REQUIRE(session.projectiles().entities().size() == 1U);
    REQUIRE(session.inventory().slot(1).count == 4U);  // one arrow spent
    REQUIRE(session.inventory().selectedStack().item == &items::Bow);
    REQUIRE(session.inventory().selectedStack().damage == 1U);  // one durability point spent
    std::cout << "testNonCreativeConsumesOneArrowAndDamagesBow OK\n";
}

void testCreativeConsumesNoArrowButStillDamagesBow() {
    TestHost host;
    world::World world;
    GameSession session;
    armSession(session, world, /*withArrow=*/false);
    session.setGameMode(GameMode::Creative);

    session.enqueueCommand(UseItem{});
    for (int i = 0; i < 21; ++i) {
        session.tick(world, host);
    }
    session.enqueueCommand(UseItemStop{});
    session.tick(world, host);

    REQUIRE(session.projectiles().entities().size() == 1U);
    // No arrow slot existed at all, and none was ever populated — creative
    // never touches the inventory for ammunition.
    REQUIRE(!session.inventory().findFirstArrowSlot().has_value());
    // Vanilla still spends the bow's own durability in creative (only the
    // ammunition/consumeItem branch is creative-exempt).
    REQUIRE(session.inventory().selectedStack().damage == 1U);
    std::cout << "testCreativeConsumesNoArrowButStillDamagesBow OK\n";
}

// --- Determinism: the SAME operation sequence on two independently
// constructed sessions with the same world seed must scatter the SAME
// launch velocity — proof the release path always draws through the
// session's own deterministic projectileRandom_, never the wall clock. ---
void testDeterministicReleaseSequence() {
    TestHost hostA;
    TestHost hostB;
    world::World worldA;
    world::World worldB;
    GameSession sessionA;
    GameSession sessionB;
    armSession(sessionA, worldA);
    armSession(sessionB, worldB);
    sessionA.setWorldSeed(0xC0FFEEULL);
    sessionB.setWorldSeed(0xC0FFEEULL);

    const auto runDraw = [&](GameSession& session, world::World& world, TestHost& host) {
        session.enqueueCommand(UseItem{});
        for (int i = 0; i < 14; ++i) {
            session.tick(world, host);
        }
        session.enqueueCommand(UseItemStop{});
        session.tick(world, host);
    };
    runDraw(sessionA, worldA, hostA);
    runDraw(sessionB, worldB, hostB);

    REQUIRE(sessionA.projectiles().entities().size() == 1U);
    REQUIRE(sessionB.projectiles().entities().size() == 1U);
    REQUIRE(sessionA.projectiles().entities().front().velocity ==
            sessionB.projectiles().entities().front().velocity);
    REQUIRE(sessionA.projectiles().entities().front().damage ==
            sessionB.projectiles().entities().front().damage);
    std::cout << "testDeterministicReleaseSequence OK\n";
}

} // namespace

int main() {
    entities::registerBuiltinEntities();

    testDrawProgressesAndReachesFullAt20Ticks();
    testCannotDrawWithoutArrowInSurvival();
    testCreativeCanDrawWithoutArrow();

    testFullDrawSpawnsFullVelocityCriticalArrow();
    testPartialDrawSpawnsWeakerNonCriticalArrow();
    testBelowMinimumPullFiresNothing();

    testNonCreativeConsumesOneArrowAndDamagesBow();
    testCreativeConsumesNoArrowButStillDamagesBow();

    testDeterministicReleaseSequence();

    std::cout << "bow_arrow_test: all tests passed\n";
    return 0;
}

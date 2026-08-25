// RW-0: the projectile system — the third non-living SoA pool, alongside
// ItemEntitySystem and ExperienceOrbSystem. Exercises it headless: gravity/
// drag flight physics with previousPosition recorded, an entity hit routed
// through Damage.hpp (so a future armored target automatically takes less —
// the EQ coupling the card requires), a block hit sticking the projectile
// (never passing through), pickup/despawn, save round trip and the snapshot
// codec, plus the launch-sequence determinism guarantee.

#include "gameplay/Damage.hpp"
#include "gameplay/DamageType.hpp"
#include "gameplay/Difficulty.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/GameSnapshotCodec.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "gameplay/entities/MobBrain.hpp"
#include "gameplay/entities/ProjectileSystem.hpp"
#include "persistence/SaveRepository.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/gen/JavaRandom.hpp"

#include <glm/geometric.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::gameplay;

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"projectile_system_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

// A minimal AI so a locally-built EntityType is complete, matching
// entity_fire_test's own IdleAi precedent.
class IdleAi final : public entities::EntityAi {
  public:
    void configureBrain(entities::MobBrain&) const override {}
};

const IdleAi kIdleAi;

const entities::EntityType& targetType() {
    static const entities::EntityType type =
        entities::EntityType::Builder::create(entities::MobCategory::Creature, kIdleAi)
            .sized(0.9F, 0.9F)
            .health(20.0F)
            .build("test_projectile_target");
    return type;
}

// A flat stone floor at y == 0 with open sky above, wide enough that a
// projectile's raycast and physics never run off the loaded area.
world::World buildTestWorld() {
    world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, world::Block::Stone);
        }
    }
    world::World world;
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

// A world with a solid wall so a horizontally-fired projectile can hit a
// block face rather than the floor.
world::World buildWallWorld() {
    world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, world::Block::Stone);
        }
    }
    // A wall spanning y in [1,5] at x == 8.
    for (int z = 0; z < 16; ++z) {
        for (int y = 1; y <= 5; ++y) {
            chunk.setBlock(8, y, z, world::Block::Stone);
        }
    }
    world::World world;
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

// --- Flight/physics: a launched projectile falls under gravity and drag;
// previousPosition is recorded every tick (render interpolation testable). ---
void testFlightRecordsPreviousPositionAndFalls() {
    world::World world = buildTestWorld();
    ProjectileSystem projectiles;
    EntitySystem entities;
    world::gen::JavaRandom rng(1U);
    projectiles.spawn({3.0F, 10.0F, 3.0F}, {0.0F, 0.0F, 0.0F}, ActorReference::player(), 2.0F);
    REQUIRE(projectiles.entities().size() == 1U);
    const glm::vec3 startPosition = projectiles.entities().front().position;

    bool everMovedFromPrevious = false;
    float lowestY = startPosition.y;
    for (int tick = 0; tick < 40 && !projectiles.entities().empty(); ++tick) {
        const glm::vec3 beforeTick = projectiles.entities().front().position;
        static_cast<void>(projectiles.tick(world, entities, glm::vec3{100.0F, 100.0F, 100.0F},
                                           /*playerPresent=*/false, rng));
        if (projectiles.entities().empty()) break;
        const auto& projectile = projectiles.entities().front();
        // previousPosition must equal what position was before this tick ran.
        REQUIRE(projectile.previousPosition == beforeTick);
        if (projectile.position != projectile.previousPosition) {
            everMovedFromPrevious = true;
        }
        lowestY = std::min(lowestY, projectile.position.y);
    }
    REQUIRE(everMovedFromPrevious);
    // Gravity pulled it down from its 10.0 spawn height.
    REQUIRE(lowestY < startPosition.y - 0.5F);
    std::cout << "testFlightRecordsPreviousPositionAndFalls OK\n";
}

// --- Hit entity: a projectile reaching a mob applies damage through
// Damage.hpp (never bypassed) plus knockback; critical adds a bonus. ---
void testHitEntityAppliesDamageThroughPipeline() {
    world::World world = buildTestWorld();
    EntitySystem entities;
    entities.spawn({6.0F, 1.0F, 3.0F}, targetType(), /*seed=*/7U);
    const auto targetId = entities.entities().front().id;
    const float healthBefore = entities.byIdConst(targetId)->damage.health;

    ProjectileSystem projectiles;
    world::gen::JavaRandom rng(2U);
    // Fired straight at the target's centre, fast enough to reach it in one tick.
    projectiles.spawn({3.0F, 1.5F, 3.0F}, {3.2F, 0.0F, 0.0F}, ActorReference::player(), 6.0F);

    bool everHit = false;
    for (int tick = 0; tick < 10; ++tick) {
        static_cast<void>(projectiles.tick(world, entities, glm::vec3{100.0F, 100.0F, 100.0F},
                                           /*playerPresent=*/false, rng));
        const float healthNow = entities.byIdConst(targetId)->damage.health;
        if (healthNow < healthBefore) {
            everHit = true;
            break;
        }
    }
    REQUIRE(everHit);
    // The pipeline stamped the invulnerability window and lastSource — proof
    // the hit actually went through applyDamage()/Damage.hpp rather than a
    // direct health subtraction that bypasses it (sabotage① target: the same
    // assertion a bypassed hit would fail, since a raw subtraction would never
    // touch these fields).
    const auto* target = entities.byIdConst(targetId);
    REQUIRE(target->damage.invulnerableTicks > 0);
    REQUIRE(target->damage.lastSource == DamageType::Projectile);
    // The projectile is consumed on a non-piercing hit.
    REQUIRE(projectiles.entities().empty());
    std::cout << "testHitEntityAppliesDamageThroughPipeline OK\n";
}

// --- The pipeline coupling itself: DamageType::Projectile does not bypass
// armor (unlike OutOfWorld/Generic), which is what lets a future EQ armor
// stage reduce it automatically — the exact "sabotage① target" assertion. ---
void testProjectileDamageTypeDoesNotBypassArmor() {
    REQUIRE(!hasDamageTag(DamageType::Projectile, DamageTag::BypassesArmor));
    REQUIRE(hasDamageTag(DamageType::Projectile, DamageTag::IsProjectile));
    std::cout << "testProjectileDamageTypeDoesNotBypassArmor OK\n";
}

// --- Critical hits deal at least as much as, and on average MORE than, a
// non-critical hit of the same base damage. RW-1a #13 makes the crit bonus an
// integer `nextInt(i/2 + 2)` roll, so a single crit can roll +0 (vanilla lets
// it too); the guarantee is over several shots the crit total strictly exceeds
// the non-crit total, and no crit ever lands BELOW the non-crit base. ---
void testCriticalHitDealsMoreDamage() {
    world::World world = buildTestWorld();

    // A single hit's applied damage for the given crit flag, threading `rng` so
    // successive crit shots draw distinct bonuses from one stream.
    const auto runOnce = [&](bool critical, world::gen::JavaRandom& rng) {
        EntitySystem entities;
        entities.spawn({6.0F, 1.0F, 3.0F}, targetType(), /*seed=*/9U);
        const auto targetId = entities.entities().front().id;
        const float healthBefore = entities.byIdConst(targetId)->damage.health;
        ProjectileSystem projectiles;
        // Base 2.0 at ~3.2 launch speed keeps even a max crit (base i = 7, bonus
        // up to +4) below the 20-health target, so a kill never clamps the
        // measured damage and hides the crit bonus.
        projectiles.spawn({3.0F, 1.5F, 3.0F}, {3.2F, 0.0F, 0.0F}, ActorReference::player(), 2.0F,
                          critical);
        for (int tick = 0; tick < 10; ++tick) {
            static_cast<void>(projectiles.tick(world, entities, glm::vec3{100.0F, 100.0F, 100.0F},
                                               false, rng));
            if (entities.byIdConst(targetId)->damage.health < healthBefore) break;
        }
        return healthBefore - entities.byIdConst(targetId)->damage.health;
    };

    world::gen::JavaRandom normalRng(3U);
    const float normalDamage = runOnce(false, normalRng);
    REQUIRE(normalDamage > 0.0F);

    world::gen::JavaRandom critRng(3U);
    float critTotal = 0.0F;
    for (int shot = 0; shot < 8; ++shot) {
        const float critDamage = runOnce(true, critRng);
        REQUIRE(critDamage >= normalDamage);  // a crit is never weaker than the base
        critTotal += critDamage;
    }
    // Averaged over the crit shots, the integer bonus adds real damage.
    REQUIRE(critTotal > normalDamage * 8.0F);
    std::cout << "testCriticalHitDealsMoreDamage OK\n";
}

// --- Hit block: a projectile reaching a solid wall sticks (inGround,
// inBlockPos recorded, velocity zeroed) instead of passing through. ---
void testHitBlockSticksInGround() {
    world::World world = buildWallWorld();
    EntitySystem entities;
    ProjectileSystem projectiles;
    world::gen::JavaRandom rng(4U);
    // Fired straight at the wall (x == 8) from x == 3, fast enough to cross
    // several blocks a tick so a "pass through" bug is unambiguous.
    projectiles.spawn({3.0F, 3.0F, 8.0F}, {4.0F, 0.0F, 0.0F}, ActorReference::player(), 2.0F);

    for (int tick = 0; tick < 20 && !projectiles.entities().empty(); ++tick) {
        static_cast<void>(projectiles.tick(world, entities, glm::vec3{100.0F, 100.0F, 100.0F},
                                           false, rng));
        if (!projectiles.entities().empty() && projectiles.entities().front().inGround) {
            break;
        }
    }
    REQUIRE(!projectiles.entities().empty());
    const auto& projectile = projectiles.entities().front();
    REQUIRE(projectile.inGround);
    REQUIRE(projectile.velocity == glm::vec3{0.0F});
    // Stuck on the near face of the wall, not past it (sabotage③ target: a
    // "does not stick" bug would leave this well past x == 8).
    REQUIRE(projectile.position.x < 8.5F);
    REQUIRE(projectile.inBlockPos.x == 8);
    std::cout << "testHitBlockSticksInGround OK\n";
}

// --- Pickup: a landed Pickupable projectile contacted by a player is
// removed and gives its pickupItem. ---
void testPickupGivesItemAndRemovesProjectile() {
    world::World world = buildTestWorld();
    EntitySystem entities;
    ProjectileSystem projectiles;
    world::gen::JavaRandom rng(5U);
    const ItemStack pickupStack{world::Block::Air, 1, &items::Stick};
    projectiles.restore({3.0F, 1.5F, 3.0F}, {0.0F, 0.0F, 0.0F}, ActorReference::player(), 2.0F,
                        false, ProjectilePickupState::Pickupable, pickupStack, /*inGround=*/true,
                        {3, 1, 3}, /*lifeTicks=*/0U);
    const glm::vec3 player{3.0F, 1.5F, 3.0F};

    std::vector<ItemStack> collected;
    for (int tick = 0; tick < 5 && !projectiles.entities().empty(); ++tick) {
        auto pickedUp =
            projectiles.tick(world, entities, player, /*playerPresent=*/true, rng);
        collected.insert(collected.end(), pickedUp.begin(), pickedUp.end());
    }
    REQUIRE(projectiles.entities().empty());
    REQUIRE(collected.size() == 1U);
    REQUIRE(collected.front().item == &items::Stick);
    std::cout << "testPickupGivesItemAndRemovesProjectile OK\n";
}

// --- RW-1a #7: a full inventory must NOT swallow a landed arrow. With a
// pickupInventory whose every slot is occupied by a different, maxed stack, the
// contact pickup fails to stow and the projectile stays on the ground (still
// pickupable next tick), rather than being silently deleted. ---
void testFullInventoryLeavesArrowOnGround() {
    world::World world = buildTestWorld();
    EntitySystem entities;
    ProjectileSystem projectiles;
    world::gen::JavaRandom rng(21U);
    const ItemStack pickupStack{world::Block::Air, 1, &items::Stick};
    projectiles.restore({3.0F, 1.5F, 3.0F}, {0.0F, 0.0F, 0.0F}, ActorReference::player(), 2.0F,
                        false, ProjectilePickupState::Pickupable, pickupStack, /*inGround=*/true,
                        {3, 1, 3}, /*lifeTicks=*/0U);
    const glm::vec3 player{3.0F, 1.5F, 3.0F};

    // Cram every slot with a maxed Arrow stack (a different item than the Stick
    // pickup), so Inventory::add finds neither a matching stack with room nor an
    // empty slot — a genuinely full backpack.
    Inventory inventory;
    for (std::size_t i = 0; i < Inventory::kSlotCount; ++i) {
        inventory.mutableSlot(i) = ItemStack{world::Block::Air, 64U, &items::Arrow};
    }

    std::vector<ItemStack> collected;
    for (int tick = 0; tick < 5; ++tick) {
        auto pickedUp =
            projectiles.tick(world, entities, player, /*playerPresent=*/true, rng, &inventory);
        collected.insert(collected.end(), pickedUp.begin(), pickedUp.end());
    }
    // Nothing was stowed and the arrow is still there to be retrieved later.
    REQUIRE(collected.empty());
    REQUIRE(projectiles.entities().size() == 1U);
    REQUIRE(projectiles.entities().front().inGround);

    // Free one slot: now the very next tick collects it and removes it (proof
    // the arrow was preserved, not lost, while the pack was full).
    inventory.mutableSlot(0) = ItemStack{};
    auto pickedUp = projectiles.tick(world, entities, player, true, rng, &inventory);
    REQUIRE(pickedUp.size() == 1U);
    REQUIRE(pickedUp.front().item == &items::Stick);
    REQUIRE(projectiles.entities().empty());
    // The Stick actually landed in the freed slot.
    REQUIRE(inventory.slot(0).item == &items::Stick);
    std::cout << "testFullInventoryLeavesArrowOnGround OK\n";
}

// --- RW-1a #8: a shot that strikes at high speed deals MORE than the same base
// arrow striking at low speed, because the applied damage is
// `ceil(velocity.length() * baseDamage)` read at hit time — the acceptance's
// "far/slow shot < near/fast shot" case. ---
void testRangeDamageScalesWithImpactSpeed() {
    world::World world = buildTestWorld();

    const auto damageAtLaunchSpeed = [&](float speed) {
        EntitySystem entities;
        // Target dead ahead, under a block away, so even the slow shot reaches
        // it on the first tick before drag bleeds off much speed.
        entities.spawn({4.0F, 1.5F, 3.0F}, targetType(), /*seed=*/33U);
        const auto targetId = entities.entities().front().id;
        const float healthBefore = entities.byIdConst(targetId)->damage.health;
        ProjectileSystem projectiles;
        world::gen::JavaRandom rng(34U);
        // Same base damage (2.0) for both; only the launch speed differs. Spawn
        // at y == 2.0 (inside the target's 1.5..2.4 box) so a tick's small
        // gravity dip never drops the ray under the box.
        projectiles.spawn({3.2F, 2.0F, 3.0F}, {speed, 0.0F, 0.0F}, ActorReference::player(), 2.0F);
        for (int tick = 0; tick < 10; ++tick) {
            static_cast<void>(projectiles.tick(world, entities,
                                               glm::vec3{100.0F, 100.0F, 100.0F}, false, rng));
            if (entities.byIdConst(targetId)->damage.health < healthBefore) break;
        }
        return healthBefore - entities.byIdConst(targetId)->damage.health;
    };

    const float fastDamage = damageAtLaunchSpeed(3.0F);   // a full-draw arrow
    const float slowDamage = damageAtLaunchSpeed(1.0F);   // a weak, slow arrow
    REQUIRE(slowDamage > 0.0F);
    REQUIRE(fastDamage > slowDamage);  // faster impact = more damage
    std::cout << "testRangeDamageScalesWithImpactSpeed OK\n";
}

// --- RW-1a #13: the critical bonus is an INTEGER draw `nextInt(i/2 + 2)` from
// the tick's deterministic stream — the same seed reproduces the same crit
// damage sequence (a wall-clock/global draw would not), and the bonus is a
// whole number, never a flat 1.5x. ---
void testCriticalBonusIsIntegerAndDeterministic() {
    world::World world = buildTestWorld();

    // The applied crit damage for a fixed base, seed and launch — captured over
    // several fresh crit shots so the bonus roll actually varies.
    const auto critDamageSequence = [&](std::uint64_t seed) {
        std::vector<float> damages;
        world::gen::JavaRandom rng(seed);
        for (int shot = 0; shot < 6; ++shot) {
            EntitySystem entities;
            entities.spawn({4.0F, 1.5F, 3.0F}, targetType(), /*seed=*/40U);
            const auto targetId = entities.entities().front().id;
            const float healthBefore = entities.byIdConst(targetId)->damage.health;
            ProjectileSystem projectiles;
            // base 4.0, launched at ~1.0 speed so applied base i = ceil(1*4)=4,
            // crit bonus bound = 4/2 + 2 = 4 -> nextInt(4) in {0,1,2,3}. Spawn
            // at y == 2.0 (inside the 1.5..2.4 box) and x == 3.2 (under a block
            // away) so the slow shot always lands on the first tick.
            projectiles.spawn({3.2F, 2.0F, 3.0F}, {1.0F, 0.0F, 0.0F}, ActorReference::player(),
                              4.0F, /*critical=*/true);
            for (int tick = 0; tick < 10; ++tick) {
                static_cast<void>(projectiles.tick(world, entities,
                                                   glm::vec3{100.0F, 100.0F, 100.0F}, false, rng));
                if (entities.byIdConst(targetId)->damage.health < healthBefore) break;
            }
            damages.push_back(healthBefore - entities.byIdConst(targetId)->damage.health);
        }
        return damages;
    };

    const auto runA = critDamageSequence(0xBEEFULL);
    const auto runB = critDamageSequence(0xBEEFULL);
    REQUIRE(runA.size() == 6U);
    REQUIRE(runA == runB);  // same seed -> identical crit sequence (determinism)

    // Every applied crit damage is a whole number (integer arithmetic, not a
    // fractional 1.5x multiply), and at least one shot rolled a strictly
    // positive bonus above the un-crit base of 4 (so the bonus roll is real).
    bool sawBonusAboveBase = false;
    for (const float damage : runA) {
        REQUIRE(damage == std::floor(damage));  // integer half-hearts
        REQUIRE(damage >= 4.0F);                 // never below the base i = 4
        if (damage > 4.0F) sawBonusAboveBase = true;
    }
    REQUIRE(sawBonusAboveBase);
    std::cout << "testCriticalBonusIsIntegerAndDeterministic OK\n";
}

// --- A NoPickup projectile is never collected even on direct contact. ---
void testNoPickupStateIsNeverCollected() {
    world::World world = buildTestWorld();
    EntitySystem entities;
    ProjectileSystem projectiles;
    world::gen::JavaRandom rng(6U);
    projectiles.restore({3.0F, 1.5F, 3.0F}, {0.0F, 0.0F, 0.0F}, ActorReference::player(), 2.0F,
                        false, ProjectilePickupState::NoPickup, ItemStack{}, true, {3, 1, 3}, 0U);
    const glm::vec3 player{3.0F, 1.5F, 3.0F};
    for (int tick = 0; tick < 5; ++tick) {
        static_cast<void>(projectiles.tick(world, entities, player, true, rng));
    }
    REQUIRE(projectiles.entities().size() == 1U);
    std::cout << "testNoPickupStateIsNeverCollected OK\n";
}

// --- lifeTicks timeout despawns a stuck projectile. ---
void testLifetimeTimeoutDespawns() {
    world::World world = buildTestWorld();
    EntitySystem entities;
    ProjectileSystem projectiles;
    world::gen::JavaRandom rng(8U);
    projectiles.restore({3.0F, 1.5F, 3.0F}, {0.0F, 0.0F, 0.0F}, ActorReference::player(), 2.0F,
                        false, ProjectilePickupState::Pickupable, ItemStack{}, true, {3, 1, 3},
                        kProjectileLifetimeTicks - 1U);
    // Far away player, so pickup never triggers — only the timeout can remove it.
    const glm::vec3 farPlayer{200.0F, 100.0F, 200.0F};
    static_cast<void>(projectiles.tick(world, entities, farPlayer, true, rng));
    REQUIRE(projectiles.entities().empty());
    std::cout << "testLifetimeTimeoutDespawns OK\n";
}

// --- Determinism: the SAME JavaRandom seed and launch sequence must produce
// the SAME trajectory/hits. A wall-clock or global-RNG crit/scatter draw
// would fail this every run (sabotage② target). ---
void testDeterministicLaunchSequence() {
    world::World worldA = buildTestWorld();
    world::World worldB = buildTestWorld();
    EntitySystem entitiesA;
    EntitySystem entitiesB;
    ProjectileSystem projectilesA;
    ProjectileSystem projectilesB;
    world::gen::JavaRandom rngA(0xC0FFEEULL);
    world::gen::JavaRandom rngB(0xC0FFEEULL);

    // The launch itself draws from each system's own JavaRandom stream (the
    // AbstractArrow-style scatter, spawn()'s `rng` parameter) — same seed, same
    // call, so the scattered launch velocity must already agree bit for bit
    // before either tick() runs.
    projectilesA.spawn({3.0F, 10.0F, 3.0F}, {0.6F, 1.0F, 0.2F}, ActorReference::player(), 2.0F,
                       true, ProjectilePickupState::Pickupable, ItemStack{}, &rngA);
    projectilesB.spawn({3.0F, 10.0F, 3.0F}, {0.6F, 1.0F, 0.2F}, ActorReference::player(), 2.0F,
                       true, ProjectilePickupState::Pickupable, ItemStack{}, &rngB);
    REQUIRE(projectilesA.entities().front().velocity == projectilesB.entities().front().velocity);
    // Not a vacuous match: the scatter must have actually perturbed the
    // velocity away from the raw input (guards against a stub that silently
    // ignores `rng` and would "match" trivially).
    REQUIRE(projectilesA.entities().front().velocity != glm::vec3(0.6F, 1.0F, 0.2F));

    for (int tick = 0; tick < 30; ++tick) {
        static_cast<void>(projectilesA.tick(worldA, entitiesA, glm::vec3{100.0F, 100.0F, 100.0F},
                                            false, rngA));
        static_cast<void>(projectilesB.tick(worldB, entitiesB, glm::vec3{100.0F, 100.0F, 100.0F},
                                            false, rngB));
        REQUIRE(projectilesA.entities().size() == projectilesB.entities().size());
        for (std::size_t i = 0; i < projectilesA.entities().size(); ++i) {
            REQUIRE(projectilesA.entities()[i].position == projectilesB.entities()[i].position);
            REQUIRE(projectilesA.entities()[i].velocity == projectilesB.entities()[i].velocity);
            REQUIRE(projectilesA.entities()[i].inGround == projectilesB.entities()[i].inGround);
        }
    }
    std::cout << "testDeterministicLaunchSequence OK\n";
}

// --- Save round trip: gather -> PersistentProjectile -> restore must land on
// the same position/velocity/damage/critical/pickupState/inGround/lifeTicks. ---
void testSaveRoundTrip() {
    ProjectileSystem source;
    const ItemStack pickupStack{world::Block::Air, 1, &items::Stick};
    source.restore({1.5F, 2.5F, 3.5F}, {0.1F, -0.02F, 0.05F}, ActorReference::player(), 6.0F,
                   true, ProjectilePickupState::Pickupable, pickupStack, true, {1, 2, 3}, 42U);
    source.restore({9.0F, 8.0F, 7.0F}, {0.0F, -0.03F, 0.0F}, ActorReference::entity(77U), 2.0F,
                   false, ProjectilePickupState::NoPickup, ItemStack{}, false, {0, 0, 0}, 0U);

    std::vector<persistence::PersistentProjectile> records;
    for (const auto& projectile : source.entities()) {
        const std::uint8_t shooterKind =
            projectile.shooterId.kind == ActorReference::Kind::Player   ? 1U
            : projectile.shooterId.kind == ActorReference::Kind::Entity ? 2U
                                                                         : 0U;
        records.push_back(persistence::PersistentProjectile{
            projectile.position.x, projectile.position.y, projectile.position.z,
            projectile.velocity.x, projectile.velocity.y, projectile.velocity.z, shooterKind,
            projectile.shooterId.entityId, projectile.damage, projectile.critical,
            static_cast<std::uint8_t>(projectile.pickupState), projectile.pickupItem,
            projectile.inGround, projectile.inBlockPos.x, projectile.inBlockPos.y,
            projectile.inBlockPos.z, projectile.lifeTicks});
    }

    ProjectileSystem restored;
    for (const auto& record : records) {
        const auto shooter = record.shooterKind == 1U
            ? ActorReference::player()
            : (record.shooterKind == 2U ? ActorReference::entity(record.shooterEntityId)
                                        : ActorReference{});
        restored.restore({record.x, record.y, record.z}, {record.vx, record.vy, record.vz},
                         shooter, record.damage, record.critical,
                         static_cast<ProjectilePickupState>(record.pickupState),
                         record.pickupItem, record.inGround,
                         {record.inBlockX, record.inBlockY, record.inBlockZ}, record.lifeTicks);
    }

    REQUIRE(restored.entities().size() == source.entities().size());
    for (std::size_t i = 0; i < source.entities().size(); ++i) {
        REQUIRE(restored.entities()[i].position == source.entities()[i].position);
        REQUIRE(restored.entities()[i].velocity == source.entities()[i].velocity);
        REQUIRE(restored.entities()[i].shooterId == source.entities()[i].shooterId);
        REQUIRE(restored.entities()[i].damage == source.entities()[i].damage);
        REQUIRE(restored.entities()[i].critical == source.entities()[i].critical);
        REQUIRE(restored.entities()[i].pickupState == source.entities()[i].pickupState);
        REQUIRE(restored.entities()[i].inGround == source.entities()[i].inGround);
        REQUIRE(restored.entities()[i].inBlockPos == source.entities()[i].inBlockPos);
        REQUIRE(restored.entities()[i].lifeTicks == source.entities()[i].lifeTicks);
    }
    std::cout << "testSaveRoundTrip OK\n";
}

// --- SaveGame/world.dat round trip through the real PJTL block encoder. ---
void testWorldDatPjtlBlockRoundTrip() {
    const auto tempDir = std::filesystem::temp_directory_path() / "mc_rebedrock_rw0_test";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);
    persistence::SaveRepository repository{tempDir};
    auto save = repository.create("rw0-world", 42ULL);
    const ItemStack pickupStack{world::Block::Air, 1, &items::Stick};
    save.projectiles = {
        {1.0F, 2.0F, 3.0F, 0.1F, -0.03F, 0.05F, 1U, 0U, 6.0F, true, 1U, pickupStack, true, 1, 2, 3,
         120U},
        {-4.0F, 70.0F, 8.0F, 0.0F, -0.02F, 0.0F, 2U, 55U, 2.0F, false, 0U, ItemStack{}, false, 0, 0,
         0, 0U},
    };
    repository.save(save);

    const auto reloaded = repository.load(save.summary.identifier);
    REQUIRE(reloaded.projectiles.size() == 2U);
    REQUIRE(reloaded.projectiles[0].damage == 6.0F);
    REQUIRE(reloaded.projectiles[0].critical);
    REQUIRE(reloaded.projectiles[0].pickupItem.item == &items::Stick);
    REQUIRE(reloaded.projectiles[0].inGround);
    REQUIRE(reloaded.projectiles[0].inBlockX == 1);
    REQUIRE(reloaded.projectiles[0].lifeTicks == 120U);
    REQUIRE(reloaded.projectiles[1].shooterKind == 2U);
    REQUIRE(reloaded.projectiles[1].shooterEntityId == 55U);
    REQUIRE(!reloaded.projectiles[1].inGround);

    std::filesystem::remove_all(tempDir);
    std::cout << "testWorldDatPjtlBlockRoundTrip OK\n";
}

// --- A pre-RW-0 save (no PJTL block) must load with an empty projectile list
// instead of throwing — the same "old world migrates cleanly" guarantee XPOB
// gives. ---
void testEmptyProjectileListRoundTrips() {
    const auto tempDir = std::filesystem::temp_directory_path() / "mc_rebedrock_rw0_test_empty";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);
    persistence::SaveRepository repository{tempDir};
    auto save = repository.create("rw0-empty-world", 7ULL);
    REQUIRE(save.projectiles.empty());
    repository.save(save);
    const auto reloaded = repository.load(save.summary.identifier);
    REQUIRE(reloaded.projectiles.empty());
    std::filesystem::remove_all(tempDir);
    std::cout << "testEmptyProjectileListRoundTrips OK\n";
}

// --- Snapshot codec round trip: the render/network wire format carries
// position/previousPosition/critical/inGround for every projectile. ---
void testSnapshotCodecRoundTrip() {
    EntityRenderSnapshot snapshot;
    std::vector<Projectile> projectiles;
    Projectile flying;
    flying.position = {1.0F, 2.0F, 3.0F};
    flying.previousPosition = {0.9F, 2.1F, 3.0F};
    flying.critical = true;
    flying.inGround = false;
    Projectile stuck;
    stuck.position = {-5.0F, 64.0F, 5.0F};
    stuck.previousPosition = {-5.0F, 64.0F, 5.0F};
    stuck.inGround = true;
    projectiles.push_back(flying);
    projectiles.push_back(stuck);
    snapshot.assign({}, {}, {}, projectiles, {});

    const auto encoded = encodeEntitySnapshot(snapshot);
    const auto decoded = decodeEntitySnapshot(encoded, nullptr);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->projectiles().size() == 2U);
    REQUIRE(decoded->projectiles()[0].position == flying.position);
    REQUIRE(decoded->projectiles()[0].previousPosition == flying.previousPosition);
    REQUIRE(decoded->projectiles()[0].critical);
    REQUIRE(!decoded->projectiles()[0].inGround);
    REQUIRE(decoded->projectiles()[1].inGround);
    std::cout << "testSnapshotCodecRoundTrip OK\n";
}

// --- A projectile never hits its own shooter (a mob-fired shot must not
// immediately register as hitting that same mob). ---
void testProjectileDoesNotHitItsOwnShooter() {
    world::World world = buildTestWorld();
    EntitySystem entities;
    entities.spawn({3.0F, 1.5F, 3.0F}, targetType(), /*seed=*/11U);
    const auto shooterId = entities.entities().front().id;
    const float healthBefore = entities.byIdConst(shooterId)->damage.health;

    ProjectileSystem projectiles;
    world::gen::JavaRandom rng(12U);
    // Spawned right at the shooter's own position, as if it just fired.
    projectiles.spawn({3.0F, 1.5F, 3.0F}, {0.5F, 0.0F, 0.0F}, ActorReference::entity(shooterId),
                      6.0F);
    static_cast<void>(
        projectiles.tick(world, entities, glm::vec3{100.0F, 100.0F, 100.0F}, false, rng));
    REQUIRE(entities.byIdConst(shooterId)->damage.health == healthBefore);
    std::cout << "testProjectileDoesNotHitItsOwnShooter OK\n";
}

} // namespace

int main() {
    testFlightRecordsPreviousPositionAndFalls();
    testHitEntityAppliesDamageThroughPipeline();
    testProjectileDamageTypeDoesNotBypassArmor();
    testCriticalHitDealsMoreDamage();
    testHitBlockSticksInGround();
    testPickupGivesItemAndRemovesProjectile();
    testFullInventoryLeavesArrowOnGround();
    testRangeDamageScalesWithImpactSpeed();
    testCriticalBonusIsIntegerAndDeterministic();
    testNoPickupStateIsNeverCollected();
    testLifetimeTimeoutDespawns();
    testDeterministicLaunchSequence();
    testSaveRoundTrip();
    testWorldDatPjtlBlockRoundTrip();
    testEmptyProjectileListRoundTrips();
    testSnapshotCodecRoundTrip();
    testProjectileDoesNotHitItsOwnShooter();
    std::cout << "projectile_system_test: all tests passed\n";
    return 0;
}

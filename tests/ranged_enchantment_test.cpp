// RW-4: ranged (bow) enchantment effects — Power / Punch / Flame / Infinity, the
// remote half of the ENCH.gate, driven through the DDC-2 effect-component engine
// (not a hardcoded per-enchant branch). Exercises it headless:
//   * the DDC-2 evaluation surface: powerDamageFactor / punchKnockbackStrength /
//     flameArrowIgniteSeconds each read a compiled EffectProgram (value bucket /
//     post_attack ignite), and infinityKeepsArrow is the pure switch;
//   * the integrated consumer (ProjectileSystem::tick): a Power arrow deals more
//     damage, a Punch arrow shoves the target harder, a Flame arrow lights it,
//     and an unenchanted arrow does none of these (the identity guarantee);
//   * determinism: the same seed reproduces the same effect sequence, and the
//     effect values themselves carry no RNG (a Flame/Power draw off a wall clock
//     would break this).

#include "gameplay/RangedEnchantment.hpp"

#include "gameplay/DamageType.hpp"
#include "gameplay/Enchantment.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "gameplay/entities/MobBrain.hpp"
#include "gameplay/entities/ProjectileSystem.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/gen/JavaRandom.hpp"

#include <glm/geometric.hpp>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::gameplay;

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"ranged_enchantment_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

bool nearly(float a, float b, float eps = 1e-4F) { return std::fabs(a - b) < eps; }

// A minimal AI so a locally-built EntityType is complete.
class IdleAi final : public entities::EntityAi {
  public:
    void configureBrain(entities::MobBrain&) const override {}
};
const IdleAi kIdleAi;

const entities::EntityType& targetType() {
    static const entities::EntityType type =
        entities::EntityType::Builder::create(entities::MobCategory::Creature, kIdleAi)
            .sized(0.9F, 0.9F)
            .health(200.0F)  // large pool so no shot ever kills (clamps damage)
            .build("test_ranged_target");
    return type;
}

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

ItemStack bowWith(EnchantmentId id, std::uint8_t level) {
    ItemStack bow{world::Block::Air, 1U, &items::Bow};
    setEnchantmentLevel(bow, id, level);
    return bow;
}

// --- Power: the damage FACTOR matches vanilla `0.25*(level+1)`, and the folded
// base damage is `base*(1+factor)`; level 0 is the identity. ---
void testPowerDamageFactorMatchesVanilla() {
    // sabotage ①: a wrong coefficient (say 0.5*(level+1)) fails these exact
    // vanilla numbers.
    REQUIRE(powerDamageFactor(0) == 0.0F);              // no enchant ⇒ identity
    REQUIRE(nearly(powerDamageFactor(1), 0.5F));        // 0.25*(1+1) = 0.5
    REQUIRE(nearly(powerDamageFactor(2), 0.75F));       // 0.25*(2+1) = 0.75
    REQUIRE(nearly(powerDamageFactor(5), 1.5F));        // 0.25*(5+1) = 1.5 (Power V)

    // The folded base damage the bow bakes into the arrow.
    REQUIRE(nearly(powerArrowBaseDamage(2.0F, static_cast<std::uint8_t>(0)), 2.0F));  // identity
    REQUIRE(nearly(powerArrowBaseDamage(2.0F, static_cast<std::uint8_t>(1)), 3.0F));  // 2*(1+0.5)
    REQUIRE(nearly(powerArrowBaseDamage(2.0F, static_cast<std::uint8_t>(5)), 5.0F));  // 2*(1+1.5)

    // Off a real bow stack.
    REQUIRE(nearly(powerArrowBaseDamage(2.0F, bowWith(EnchantmentId::Power, 5U)), 5.0F));
    REQUIRE(nearly(powerArrowBaseDamage(2.0F, ItemStack{world::Block::Air, 1U, &items::Bow}),
                   2.0F));  // plain bow ⇒ unchanged
    std::cout << "testPowerDamageFactorMatchesVanilla OK\n";
}

// --- Punch: the extra knockback strength scales with level; level 0 identity. ---
void testPunchKnockbackStrength() {
    REQUIRE(punchKnockbackStrength(0) == 0.0F);          // identity
    REQUIRE(nearly(punchKnockbackStrength(1), 0.5F));    // 0.5*1
    REQUIRE(nearly(punchKnockbackStrength(2), 1.0F));    // 0.5*2 (Punch II is max)
    REQUIRE(nearly(punchKnockbackStrength(bowWith(EnchantmentId::Punch, 2U)), 1.0F));
    REQUIRE(punchKnockbackStrength(ItemStack{world::Block::Air, 1U, &items::Bow}) == 0.0F);
    std::cout << "testPunchKnockbackStrength OK\n";
}

// --- Flame: a Flame bow ignites for a flat 100 seconds (level-independent),
// through the DDC-2 post_attack ignite action; no Flame ⇒ 0. ---
void testFlameIgniteSeconds() {
    REQUIRE(flameArrowIgniteSeconds(0) == 0);            // identity — never ignites
    REQUIRE(flameArrowIgniteSeconds(1) == 100);          // setSecondsOnFire(100)
    REQUIRE(flameArrowIgniteSeconds(bowWith(EnchantmentId::Flame, 1U)) == 100);
    REQUIRE(flameArrowIgniteSeconds(ItemStack{world::Block::Air, 1U, &items::Bow}) == 0);
    std::cout << "testFlameIgniteSeconds OK\n";
}

// --- Infinity: a pure switch off the bow; only present when enchanted. ---
void testInfinityKeepsArrow() {
    REQUIRE(!infinityKeepsArrow(0));                     // identity — consumes arrow
    REQUIRE(infinityKeepsArrow(1));
    REQUIRE(infinityKeepsArrow(bowWith(EnchantmentId::Infinity, 1U)));
    REQUIRE(!infinityKeepsArrow(ItemStack{world::Block::Air, 1U, &items::Bow}));
    std::cout << "testInfinityKeepsArrow OK\n";
}

// --- The DDC-2 programs compiled cleanly (no unknown component/value/action/
// condition), proving the effect JSON really went through the engine, not a
// hand path (the same audit ENCH-1b/EQ-4 make). ---
void testProgramsCompileWithoutUnknowns() {
    for (std::size_t index = 0; index < kBowEffectEnchantments.size(); ++index) {
        const auto& program = detail::bowEffectProgram(index);
        REQUIRE(program.totalUnknown() == 0);
    }
    std::cout << "testProgramsCompileWithoutUnknowns OK\n";
}

// A single arrow's applied damage against a fresh target, base 2.0 at ~3.2
// launch speed, using the given baked base damage.
float damageForBase(world::World& world, float baseDamage) {
    EntitySystem entities;
    entities.spawn({6.0F, 1.0F, 3.0F}, targetType(), /*seed=*/7U);
    const auto targetId = entities.entities().front().id;
    const float healthBefore = entities.byIdConst(targetId)->damage.health;
    ProjectileSystem projectiles;
    world::gen::JavaRandom rng(2U);
    projectiles.spawn({3.0F, 1.5F, 3.0F}, {3.2F, 0.0F, 0.0F}, ActorReference::player(), baseDamage);
    for (int tick = 0; tick < 10; ++tick) {
        static_cast<void>(projectiles.tick(world, entities, glm::vec3{100.0F, 100.0F, 100.0F},
                                           false, rng));
        if (entities.byIdConst(targetId)->damage.health < healthBefore) break;
    }
    return healthBefore - entities.byIdConst(targetId)->damage.health;
}

// --- Integrated: a Power-boosted arrow deals strictly more than a plain one
// through the real hit pipeline (Power baked into the projectile's base). ---
void testPowerArrowDealsMoreDamage() {
    world::World world = buildTestWorld();
    const float plain = damageForBase(world, powerArrowBaseDamage(2.0F, static_cast<std::uint8_t>(0)));
    const float powerV = damageForBase(world, powerArrowBaseDamage(2.0F, static_cast<std::uint8_t>(5)));
    REQUIRE(plain > 0.0F);
    REQUIRE(powerV > plain);  // Power V hits harder
    std::cout << "testPowerArrowDealsMoreDamage OK\n";
}

// The horizontal knockback speed a target picked up from an arrow with the given
// punch strength / flame seconds. Returns {knockbackSpeed, fireTicks}.
struct HitResult {
    float knockbackSpeed = 0.0F;
    int fireTicks = 0;
    bool hit = false;
};

HitResult hitWith(world::World& world, float punch, int flameSeconds) {
    EntitySystem entities;
    entities.spawn({6.0F, 1.0F, 3.0F}, targetType(), /*seed=*/7U);
    const auto targetId = entities.entities().front().id;
    const float healthBefore = entities.byIdConst(targetId)->damage.health;
    ProjectileSystem projectiles;
    world::gen::JavaRandom rng(2U);
    projectiles.spawn({3.0F, 1.5F, 3.0F}, {3.2F, 0.0F, 0.0F}, ActorReference::player(), 2.0F,
                      /*critical=*/false, ProjectilePickupState::Pickupable, ItemStack{},
                      &rng, kProjectileDefaultInaccuracy, punch, flameSeconds);
    HitResult result;
    for (int tick = 0; tick < 10; ++tick) {
        static_cast<void>(projectiles.tick(world, entities, glm::vec3{100.0F, 100.0F, 100.0F},
                                           false, rng));
        if (entities.byIdConst(targetId)->damage.health < healthBefore) {
            const auto* target = entities.byIdConst(targetId);
            const glm::vec3 v = target->velocity;
            result.knockbackSpeed = std::sqrt(v.x * v.x + v.z * v.z);
            result.fireTicks = target->fireTicks;
            result.hit = true;
            break;
        }
    }
    return result;
}

// --- Integrated: a Punch arrow shoves the target harder than a plain one. ---
void testPunchArrowIncreasesKnockback() {
    world::World world = buildTestWorld();
    const HitResult plain = hitWith(world, /*punch=*/0.0F, /*flameSeconds=*/0);
    const HitResult punched = hitWith(world, punchKnockbackStrength(2), /*flameSeconds=*/0);
    REQUIRE(plain.hit);
    REQUIRE(punched.hit);
    REQUIRE(punched.knockbackSpeed > plain.knockbackSpeed);  // Punch pushes harder
    std::cout << "testPunchArrowIncreasesKnockback OK\n";
}

// --- Integrated: a Flame arrow lights the target; a plain one never does. ---
void testFlameArrowIgnitesTarget() {
    world::World world = buildTestWorld();
    const HitResult plain = hitWith(world, /*punch=*/0.0F, /*flameSeconds=*/0);
    const HitResult flaming = hitWith(world, /*punch=*/0.0F, flameArrowIgniteSeconds(1));
    REQUIRE(plain.hit);
    REQUIRE(flaming.hit);
    REQUIRE(plain.fireTicks == 0);       // no Flame ⇒ never ablaze
    REQUIRE(flaming.fireTicks > 0);      // Flame set the target on fire
    std::cout << "testFlameArrowIgnitesTarget OK\n";
}

// --- Determinism: the effect helper values carry no RNG, so a fixed level always
// yields the identical Power/Punch/Flame numbers across repeated calls, and the
// integrated Flame ignite is identical across two runs of the same seed. ---
void testDeterministicEffectValues() {
    for (std::uint8_t level = 1; level <= 5; ++level) {
        REQUIRE(powerDamageFactor(level) == powerDamageFactor(level));
    }
    for (std::uint8_t level = 1; level <= 2; ++level) {
        REQUIRE(punchKnockbackStrength(level) == punchKnockbackStrength(level));
    }
    REQUIRE(flameArrowIgniteSeconds(1) == flameArrowIgniteSeconds(1));

    // The integrated Flame ignite is reproducible for a given seed (no wall clock).
    world::World worldA = buildTestWorld();
    world::World worldB = buildTestWorld();
    const HitResult a = hitWith(worldA, punchKnockbackStrength(2), flameArrowIgniteSeconds(1));
    const HitResult b = hitWith(worldB, punchKnockbackStrength(2), flameArrowIgniteSeconds(1));
    REQUIRE(a.hit && b.hit);
    REQUIRE(a.fireTicks == b.fireTicks);
    REQUIRE(a.knockbackSpeed == b.knockbackSpeed);
    std::cout << "testDeterministicEffectValues OK\n";
}

// --- Identity: an unenchanted bow leaves damage, knockback and fire untouched
// (the whole no-remote-enchant contract). ---
void testNoEnchantIsIdentity() {
    const ItemStack plainBow{world::Block::Air, 1U, &items::Bow};
    REQUIRE(nearly(powerArrowBaseDamage(2.0F, plainBow), 2.0F));
    REQUIRE(punchKnockbackStrength(plainBow) == 0.0F);
    REQUIRE(flameArrowIgniteSeconds(plainBow) == 0);
    REQUIRE(!infinityKeepsArrow(plainBow));
    std::cout << "testNoEnchantIsIdentity OK\n";
}

} // namespace

int main() {
    testPowerDamageFactorMatchesVanilla();
    testPunchKnockbackStrength();
    testFlameIgniteSeconds();
    testInfinityKeepsArrow();
    testProgramsCompileWithoutUnknowns();
    testPowerArrowDealsMoreDamage();
    testPunchArrowIncreasesKnockback();
    testFlameArrowIgnitesTarget();
    testDeterministicEffectValues();
    testNoEnchantIsIdentity();
    std::cout << "ranged_enchantment_test: all tests passed\n";
    return 0;
}

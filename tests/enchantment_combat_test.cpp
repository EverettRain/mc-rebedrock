// ENCH-1: outgoing melee weapon enchantment effects — Sharpness/Smite/Bane of
// Arthropods damage bonus, Knockback's extra shove, Fire Aspect's burn. Both
// the pure per-enchant formulas (EnchantmentCombat.hpp) and the two live
// systems they feed (EntitySystem::hurt's knockback strength,
// EntitySystem::setOnFire's burn ticks) are exercised here so a regression in
// either the formula or the wiring trips a test.

#include "gameplay/EnchantmentCombat.hpp"
#include "gameplay/Enchantment.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/MiningSystem.hpp"
#include "gameplay/StatusEffect.hpp"
#include "gameplay/entities/BuiltinSpecies.hpp"
#include "gameplay/entities/EntityRegistry.hpp"

#include <glm/vec3.hpp>

#include <cassert>
#include <cstdint>
#include <iostream>

using namespace mc;
using namespace mc::gameplay;
using mc::gameplay::entities::entityTypeRegistry;

namespace {

ItemStack ironSword() { return ItemStack{world::Block::Air, 1U, &items::IronSword}; }

// Sharpness N adds exactly 0.5N + 0.5, all targets, no category dependence.
void testSharpnessFormula() {
    assert(sharpnessBonusDamage(0U) == 0.0F);
    assert(sharpnessBonusDamage(1U) == 1.0F);  // worked example: N=1 -> +1.0
    assert(sharpnessBonusDamage(5U) == 3.0F);  // worked example: N=5 -> +3.0
    assert(sharpnessBonusDamage(2U) == 1.5F);

    auto sword = ironSword();
    setEnchantmentLevel(sword, EnchantmentId::Sharpness, 1U);
    assert(meleeDamageEnchantBonus(sword, /*undead=*/false, /*arthropod=*/false) == 1.0F);
    assert(meleeDamageEnchantBonus(sword, /*undead=*/true, /*arthropod=*/false) == 1.0F);
    std::cout << "testSharpnessFormula OK\n";
}

// Smite N adds 2.5N vs an undead target, 0 vs anything else — the
// target-category gate must actually gate, not apply unconditionally.
void testSmiteCategoryGate() {
    assert(smiteBonusDamage(3U, /*targetIsUndead=*/true) == 7.5F);
    assert(smiteBonusDamage(3U, /*targetIsUndead=*/false) == 0.0F);

    auto sword = ironSword();
    setEnchantmentLevel(sword, EnchantmentId::Smite, 2U);
    // vs a cow (non-undead): the bonus must be exactly 0.
    assert(meleeDamageEnchantBonus(sword, /*undead=*/false, /*arthropod=*/false) == 0.0F);
    // vs a zombie/husk (undead): 2.5 * 2 = 5.0.
    assert(meleeDamageEnchantBonus(sword, /*undead=*/true, /*arthropod=*/false) == 5.0F);
    std::cout << "testSmiteCategoryGate OK\n";
}

// Bane of Arthropods N adds 2.5N vs an arthropod target, 0 otherwise — the
// same category-gate shape as Smite, asserted against the synthetic flag
// since no arthropod mob exists in this build yet (see file banner in
// EnchantmentCombat.hpp).
void testBaneOfArthropodsCategoryGate() {
    assert(baneOfArthropodsBonusDamage(4U, /*targetIsArthropod=*/true) == 10.0F);
    assert(baneOfArthropodsBonusDamage(4U, /*targetIsArthropod=*/false) == 0.0F);

    auto sword = ironSword();
    setEnchantmentLevel(sword, EnchantmentId::BaneOfArthropods, 1U);
    assert(meleeDamageEnchantBonus(sword, /*undead=*/false, /*arthropod=*/false) == 0.0F);
    assert(meleeDamageEnchantBonus(sword, /*undead=*/false, /*arthropod=*/true) == 2.5F);
    std::cout << "testBaneOfArthropodsCategoryGate OK\n";
}

// Knockback N increases the applied knockback strength by 0.5N (the
// EnchantmentHelper.getKnockback contribution folded additively into the
// single knockback application — see EnchantmentCombat.hpp's
// meleeKnockbackEnchantBonus for the exact-replay-vs-simplification note).
void testKnockbackBonus() {
    auto sword = ironSword();
    assert(meleeKnockbackEnchantBonus(sword) == 0.0F);
    setEnchantmentLevel(sword, EnchantmentId::Knockback, 2U);
    assert(meleeKnockbackEnchantBonus(sword) == 1.0F);

    // Integration: EntitySystem::hurt's applied horizontal velocity scales
    // with kKnockbackStrength + extraKnockbackStrength. A hit with Knockback 2
    // (extra 1.0) must push harder than an otherwise-identical hit with none.
    entities::registerBuiltinEntities();
    const auto* cowType = entityTypeRegistry().byId("cow");
    assert(cowType != nullptr);

    EntitySystem plain;
    plain.spawn({0.0F, 0.0F, 0.0F}, *cowType, 1U);
    const std::uint64_t plainId = plain.entities()[0].id;
    assert(plain.hurt(plainId, 1.0F, {0.0F, 0.0F, -1.0F}));
    const float plainSpeedSquared = plain.entities()[0].velocity.x * plain.entities()[0].velocity.x +
        plain.entities()[0].velocity.z * plain.entities()[0].velocity.z;

    EntitySystem enchanted;
    enchanted.spawn({0.0F, 0.0F, 0.0F}, *cowType, 2U);
    const std::uint64_t enchantedId = enchanted.entities()[0].id;
    assert(enchanted.hurt(enchantedId, 1.0F, {0.0F, 0.0F, -1.0F}, ActorReference::player(),
                          DamageType::EntityAttack, meleeKnockbackEnchantBonus(sword)));
    const float enchantedSpeedSquared =
        enchanted.entities()[0].velocity.x * enchanted.entities()[0].velocity.x +
        enchanted.entities()[0].velocity.z * enchanted.entities()[0].velocity.z;

    assert(enchantedSpeedSquared > plainSpeedSquared);
    std::cout << "testKnockbackBonus OK\n";
}

// Fire Aspect N sets the target burning for 4N seconds — asserted both as a
// pure formula and via EM1's real setOnFire burn-ticks state
// (kTicksPerSecond * seconds).
void testFireAspectDuration() {
    auto sword = ironSword();
    assert(meleeFireAspectSeconds(sword) == 0);
    setEnchantmentLevel(sword, EnchantmentId::FireAspect, 2U);
    assert(meleeFireAspectSeconds(sword) == 8); // worked example: N=2 -> 4*2=8s

    entities::registerBuiltinEntities();
    const auto* cowType = entityTypeRegistry().byId("cow");
    assert(cowType != nullptr);
    EntitySystem targets;
    targets.spawn({0.0F, 0.0F, 0.0F}, *cowType, 3U);
    const std::uint64_t id = targets.entities()[0].id;
    assert(targets.setOnFire(id, meleeFireAspectSeconds(sword)));
    assert(targets.entities()[0].fireTicks == 8 * kTicksPerSecond);
    std::cout << "testFireAspectDuration OK\n";
}

// Bane of Arthropods' bonus Slowness-on-hit (DamageEnchantment#onTargetDamaged):
// a landed BoA hit lands Slowness IV (amplifier 3) on an arthropod target for
// `20 + nextInt(10*level)` ticks. Asserted as the pure formula, the constant
// amplifier, the random bound, and via EntitySystem::applyBaneOfArthropodsSlowness
// landing a real EM2 Slowness effect whose duration sits in [20, 20+10*level).
void testBaneOfArthropodsSlowness() {
    // Pure duration formula: 20 + the caller-supplied draw; 0 at level 0.
    assert(baneOfArthropodsSlownessTicks(0U, 5) == 0);
    assert(baneOfArthropodsSlownessTicks(1U, 0) == 20);
    assert(baneOfArthropodsSlownessTicks(2U, 7) == 27);
    // Random bound is 10*level, exactly nextInt(10 * level).
    assert(baneOfArthropodsSlownessRandomBound(1U) == 10);
    assert(baneOfArthropodsSlownessRandomBound(3U) == 30);
    // Slowness IV — amplifier 3, independent of the enchant level.
    assert(kBaneOfArthropodsSlownessAmplifier == 3U);

    entities::registerBuiltinEntities();
    const auto* cowType = entityTypeRegistry().byId("cow");
    assert(cowType != nullptr);

    // Live wiring: applying BoA-3 lands a real Slowness IV whose duration is in
    // [20, 20 + 10*3) = [20, 50). (No arthropod species exists yet, so a cow
    // stands in for the target — the method itself does not re-check the
    // category; PlayerInteraction's gate does. See the file banner.)
    EntitySystem system;
    system.spawn({0.0F, 0.0F, 0.0F}, *cowType, 11U);
    const std::uint64_t id = system.entities()[0].id;
    assert(system.applyBaneOfArthropodsSlowness(id, 3U));
    assert(system.hasEffect(id, slownessEffect()));
    const EffectInstance* slowness = getEffect(system.entities()[0].effects, slownessEffect());
    assert(slowness != nullptr);
    assert(slowness->amplifier == 3U);
    assert(slowness->durationTicks >= 20 && slowness->durationTicks < 50);

    // Level 0 is a no-op (no enchant, no effect landed).
    EntitySystem none;
    none.spawn({0.0F, 0.0F, 0.0F}, *cowType, 12U);
    const std::uint64_t noneId = none.entities()[0].id;
    assert(!none.applyBaneOfArthropodsSlowness(noneId, 0U));
    assert(!none.hasEffect(noneId, slownessEffect()));

    // Determinism: two entities spawned with the same seed draw the identical
    // slowness duration — the draw runs off the reproducible mc::rng stream, not
    // the wall clock. Same seed 11U (matching the first entity above).
    EntitySystem replay;
    replay.spawn({0.0F, 0.0F, 0.0F}, *cowType, 11U);
    const std::uint64_t replayId = replay.entities()[0].id;
    assert(replay.applyBaneOfArthropodsSlowness(replayId, 3U));
    const EffectInstance* replaySlowness =
        getEffect(replay.entities()[0].effects, slownessEffect());
    assert(replaySlowness != nullptr);
    assert(replaySlowness->durationTicks == slowness->durationTicks);
    std::cout << "testBaneOfArthropodsSlowness OK\n";
}

// A weapon with no enchantments must change nothing: the damage bonus is
// exactly 0 and the base attack damage is untouched (regression guard).
void testNoEnchantNoChange() {
    auto sword = ironSword();
    assert(meleeDamageEnchantBonus(sword, /*undead=*/true, /*arthropod=*/true) == 0.0F);
    assert(meleeKnockbackEnchantBonus(sword) == 0.0F);
    assert(meleeFireAspectSeconds(sword) == 0);

    const auto attributes = toolAttributes(toolType(sword), toolTier(sword));
    const float baseDamage = attributes.attackDamage;
    const float totalDamage = baseDamage + meleeDamageEnchantBonus(sword, true, true);
    assert(totalDamage == baseDamage);
    std::cout << "testNoEnchantNoChange OK\n";
}

// Determinism: identical inputs must produce identical outputs every call,
// with no RNG or wall-clock in any of the five formulas.
void testDeterminism() {
    auto sword = ironSword();
    setEnchantmentLevel(sword, EnchantmentId::Sharpness, 3U);
    setEnchantmentLevel(sword, EnchantmentId::Knockback, 1U);
    setEnchantmentLevel(sword, EnchantmentId::FireAspect, 1U);
    for (int i = 0; i < 5; ++i) {
        assert(meleeDamageEnchantBonus(sword, false, false) == 2.0F);
        assert(meleeKnockbackEnchantBonus(sword) == 0.5F);
        assert(meleeFireAspectSeconds(sword) == 4);
    }
    std::cout << "testDeterminism OK\n";
}

// Zombie/husk are tagged EntityBehavior::Undead (the Smite gate reads the
// target's live EntityType, not a hand-passed bool, at the real attack site
// in PlayerInteraction.cpp) — this proves the registry wiring, not just the
// formula in isolation.
void testUndeadTagOnRegisteredSpecies() {
    entities::registerBuiltinEntities();
    const auto* zombieType = entityTypeRegistry().byId("zombie");
    const auto* huskType = entityTypeRegistry().byId("husk");
    const auto* cowType = entityTypeRegistry().byId("cow");
    assert(zombieType != nullptr && huskType != nullptr && cowType != nullptr);
    assert(zombieType->isUndead());
    assert(huskType->isUndead());
    assert(!cowType->isUndead());
    assert(!zombieType->isArthropod());
    std::cout << "testUndeadTagOnRegisteredSpecies OK\n";
}

} // namespace

int main() {
    testSharpnessFormula();
    testSmiteCategoryGate();
    testBaneOfArthropodsCategoryGate();
    testKnockbackBonus();
    testFireAspectDuration();
    testBaneOfArthropodsSlowness();
    testNoEnchantNoChange();
    testDeterminism();
    testUndeadTagOnRegisteredSpecies();
    return 0;
}

// EQ-3: the Resistance / Fire Resistance effects and the absorption stage, in
// the same three layers armor_mitigation_test uses: the pure formula
// (damageAfterResistance), the pipeline stages wired into applyDamage (bypass
// respected, fire immunity guard, absorption soaks before health, ordering),
// and the end-to-end player path through PlayerVitals with the effects applied.

#include "gameplay/Damage.hpp"
#include "gameplay/PlayerVitals.hpp"
#include "gameplay/StatusEffect.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {

using namespace mc::gameplay;

[[nodiscard]] bool nearly(float value, float expected, float epsilon = 0.001F) {
    return std::fabs(value - expected) < epsilon;
}

// --- Layer 1: the pure Resistance formula, LivingEntity#getDamageAfterMagicAbsorb. ---

// Sabotage① target: each level must remove a flat 20% — `(25 - level*5)/25`.
void testDamageAfterResistanceWorkedExamples() {
    // Level 0 (no Resistance) is the identity.
    assert(nearly(damageAfterResistance(10.0F, 0U), 10.0F));

    // The card's worked example: Resistance II (level 2) vs 10 damage →
    // 10 * (25 - 10)/25 = 10 * 0.6 = 6.0, a 40% reduction.
    assert(nearly(damageAfterResistance(10.0F, 2U), 6.0F));

    // Resistance I (level 1): −20% → 8.0.
    assert(nearly(damageAfterResistance(10.0F, 1U), 8.0F));

    // Resistance V (level 5) removes it all: 10 * (25-25)/25 = 0.
    assert(nearly(damageAfterResistance(10.0F, 5U), 0.0F));

    // An overshoot past five must not go negative — the level is clamped, so a
    // hypothetical level 6 is still full immunity, not damage flipped to
    // healing.
    assert(nearly(damageAfterResistance(10.0F, 6U), 0.0F));

    std::cout << "testDamageAfterResistanceWorkedExamples OK\n";
}

// --- Layer 2: the pipeline stages wired into applyDamage. ---

// Resistance II reduces a non-bypassing hit by 40% through the whole pipeline.
void testPipelineAppliesResistance() {
    DamageState state{20.0F, 20.0F};
    DamageContext context{DamageType::EntityAttack, 10.0F};
    context.resistanceLevel = 2U;  // Resistance II
    const auto outcome = applyDamage(state, context);
    assert(outcome.landed);
    assert(nearly(outcome.appliedDamage, 6.0F));  // 10 * 0.6
    assert(nearly(state.health, 14.0F));
    std::cout << "testPipelineAppliesResistance OK\n";
}

// Sabotage② target: BypassesResistance (the void, /kill) must ignore the
// Resistance stage entirely.
void testPipelineRespectsBypassesResistance() {
    // OutOfWorld carries BypassesResistance — full Resistance V does not soften
    // the void. It also carries BypassesArmor/BypassesInvulnerability, so the
    // whole 10 lands.
    assert(hasDamageTag(DamageType::OutOfWorld, DamageTag::BypassesResistance));
    DamageState voided{20.0F, 20.0F};
    DamageContext context{DamageType::OutOfWorld, 10.0F};
    context.resistanceLevel = 5U;  // would be full immunity if it applied
    const auto outcome = applyDamage(voided, context);
    assert(outcome.landed);
    assert(nearly(outcome.appliedDamage, 10.0F));  // unresisted
    assert(nearly(voided.health, 10.0F));

    // Starving carries BypassesEffects, which also skips the Resistance step
    // (vanilla checks it first). Full Resistance does not stop starvation.
    assert(hasDamageTag(DamageType::Starve, DamageTag::BypassesEffects));
    DamageState starving{20.0F, 20.0F};
    DamageContext starveCtx{DamageType::Starve, 4.0F};
    starveCtx.resistanceLevel = 5U;
    const auto starveOutcome = applyDamage(starving, starveCtx);
    assert(nearly(starveOutcome.appliedDamage, 4.0F));

    std::cout << "testPipelineRespectsBypassesResistance OK\n";
}

// Sabotage③ target: Fire Resistance must immunise every IsFire type (OnFire,
// InFire, Lava), and only those.
void testPipelineFireImmunity() {
    for (const DamageType fire : {DamageType::OnFire, DamageType::InFire, DamageType::Lava}) {
        assert(hasDamageTag(fire, DamageTag::IsFire));
        DamageState state{20.0F, 20.0F};
        DamageContext context{fire, 5.0F};
        context.fireImmune = true;
        const auto outcome = applyDamage(state, context);
        assert(!outcome.landed);            // the hit bounces off outright
        assert(nearly(state.health, 20.0F));
        assert(state.hurtTicks == 0);       // no flash either
    }

    // A non-fire hit still lands with fireImmune set — the guard is scoped to
    // IsFire, not "immune to everything".
    DamageState hit{20.0F, 20.0F};
    DamageContext melee{DamageType::EntityAttack, 5.0F};
    melee.fireImmune = true;
    const auto outcome = applyDamage(hit, melee);
    assert(outcome.landed);
    assert(nearly(hit.health, 15.0F));

    std::cout << "testPipelineFireImmunity OK\n";
}

// The absorption stage: the pool soaks the hit before real health, then loses
// exactly what it soaked — vanilla's `dmg = max(dmg - absorption, 0)` /
// `absorption -= originalDamage - dmg`.
void testAbsorptionSoaksBeforeHealth() {
    // 4 absorption vs a 10 hit: 4 soaked, 6 reaches health, pool empties.
    DamageState state{20.0F, 20.0F};
    state.absorptionAmount = 4.0F;
    const auto outcome = applyDamage(state, DamageType::EntityAttack, 10.0F);
    assert(outcome.landed);
    assert(nearly(outcome.absorbedDamage, 4.0F));
    assert(nearly(outcome.appliedDamage, 6.0F));
    assert(nearly(state.health, 14.0F));            // 20 - 6
    assert(nearly(state.absorptionAmount, 0.0F));   // pool spent

    // A hit smaller than the pool takes no real health and leaves a remainder.
    DamageState shielded{20.0F, 20.0F};
    shielded.absorptionAmount = 8.0F;
    const auto small = applyDamage(shielded, DamageType::EntityAttack, 3.0F);
    assert(small.landed);
    assert(nearly(small.absorbedDamage, 3.0F));
    assert(nearly(small.appliedDamage, 0.0F));       // nothing reached health
    assert(nearly(shielded.health, 20.0F));          // full health preserved
    assert(nearly(shielded.absorptionAmount, 5.0F)); // 8 - 3

    // With no absorption the stage is an identity (the default for every entity
    // today, since no source grants it yet).
    DamageState naked{20.0F, 20.0F};
    const auto plain = applyDamage(naked, DamageType::EntityAttack, 5.0F);
    assert(nearly(plain.absorbedDamage, 0.0F));
    assert(nearly(plain.appliedDamage, 5.0F));
    assert(nearly(naked.health, 15.0F));

    std::cout << "testAbsorptionSoaksBeforeHealth OK\n";
}

// Ordering: Resistance runs before absorption (vanilla getDamageAfterMagicAbsorb
// precedes the absorption subtraction). Resistance II halves-ish the 10 to 6,
// THEN 4 absorption soaks it to 2 reaching health.
void testResistanceThenAbsorptionOrder() {
    DamageState state{20.0F, 20.0F};
    state.absorptionAmount = 4.0F;
    DamageContext context{DamageType::EntityAttack, 10.0F};
    context.resistanceLevel = 2U;  // 10 -> 6 first
    const auto outcome = applyDamage(state, context);
    assert(nearly(outcome.absorbedDamage, 4.0F));   // absorbs from the resisted 6
    assert(nearly(outcome.appliedDamage, 2.0F));    // 6 - 4
    assert(nearly(state.health, 18.0F));            // 20 - 2
    assert(nearly(state.absorptionAmount, 0.0F));

    std::cout << "testResistanceThenAbsorptionOrder OK\n";
}

// --- Layer 3: end to end through PlayerVitals with the effects applied. ---

void testEndToEndResistanceThroughVitals() {
    PlayerVitals vitals;
    vitals.reset();
    // Resistance II: amplifier 1 -> level 2 in the pipeline.
    assert(vitals.applyEffect(resistanceEffect(), 200, /*amplifier=*/1U));
    assert(vitals.hurt(10.0F, DamageType::EntityAttack));
    assert(nearly(vitals.health(), 14.0F));  // 20 - 6

    std::cout << "testEndToEndResistanceThroughVitals OK\n";
}

void testEndToEndFireResistanceThroughVitals() {
    PlayerVitals vitals;
    vitals.reset();
    assert(vitals.applyEffect(fireResistanceEffect(), 200, /*amplifier=*/0U));
    // Lava and the burn tick both bounce off.
    assert(!vitals.hurt(4.0F, DamageType::Lava));
    assert(!vitals.hurt(1.0F, DamageType::OnFire));
    assert(nearly(vitals.health(), 20.0F));
    // But a mob's swing still lands.
    assert(vitals.hurt(3.0F, DamageType::EntityAttack));
    assert(nearly(vitals.health(), 17.0F));

    std::cout << "testEndToEndFireResistanceThroughVitals OK\n";
}

// The two effects register under both namespaces and carry the right shape.
void testEffectsRegistered() {
    assert(resistanceEffect().valid());
    assert(fireResistanceEffect().valid());
    assert(statusEffectByName("resistance") == resistanceEffect());
    assert(statusEffectByName("minecraft:resistance") == resistanceEffect());
    assert(statusEffectByName("fire_resistance") == fireResistanceEffect());
    assert(statusEffectByName("minecraft:fire_resistance") == fireResistanceEffect());
    // Neither has per-tick behaviour; the pipeline consults them directly.
    assert(statusEffectDef(resistanceEffect()).kind == EffectKind::None);
    assert(statusEffectDef(fireResistanceEffect()).kind == EffectKind::None);
    assert(statusEffectDef(resistanceEffect()).category == StatusEffectCategory::Beneficial);

    // resistanceLevel/isFireImmune derive the pipeline inputs off a store.
    ActiveEffects effects;
    assert(resistanceLevel(effects) == 0U);
    assert(!isFireImmune(effects));
    assert(applyEffect(effects, resistanceEffect(), 100, /*amplifier=*/2U));  // Resistance III
    assert(resistanceLevel(effects) == 3U);
    assert(applyEffect(effects, fireResistanceEffect(), 100, 0U));
    assert(isFireImmune(effects));

    std::cout << "testEffectsRegistered OK\n";
}

}  // namespace

int main() {
    testDamageAfterResistanceWorkedExamples();
    testPipelineAppliesResistance();
    testPipelineRespectsBypassesResistance();
    testPipelineFireImmunity();
    testAbsorptionSoaksBeforeHealth();
    testResistanceThenAbsorptionOrder();
    testEndToEndResistanceThroughVitals();
    testEndToEndFireResistanceThroughVitals();
    testEffectsRegistered();
    std::cout << "resistance_absorption_test: all tests passed\n";
    return 0;
}

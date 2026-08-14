#include "gameplay/Damage.hpp"

#include "gameplay/PlayerVitals.hpp"

#include <cassert>
#include <cmath>
#include <limits>

// The damage pipeline as data: the type table, the tag mask, and the fixed
// order of stages. What used to be a seven-value enum plus a difficulty
// multiplier applied by whichever caller remembered to.

namespace {

using namespace mc::gameplay;

[[nodiscard]] bool nearly(float value, float expected) {
    return std::fabs(value - expected) < 0.001F;
}

// The exhaustion column, which is the one this replaced with data. Vanilla
// charges hunger for exactly one of these.
void testExhaustionIsPerType() {
    assert(nearly(damageTypeData(DamageType::EntityAttack).exhaustion, 0.1F));
    assert(nearly(damageTypeData(DamageType::Fall).exhaustion, 0.0F));
    assert(nearly(damageTypeData(DamageType::Drown).exhaustion, 0.0F));
    assert(nearly(damageTypeData(DamageType::Starve).exhaustion, 0.0F));
    assert(nearly(damageTypeData(DamageType::OutOfWorld).exhaustion, 0.0F));
    assert(nearly(damageTypeData(DamageType::Generic).exhaustion, 0.0F));

    // And it reaches the outcome, which is how the player's hunger learns about
    // it without the vitals knowing anything about damage types.
    DamageState state{20.0F, 20.0F};
    assert(nearly(applyDamage(state, DamageType::EntityAttack, 3.0F).exhaustion, 0.1F));
    state.invulnerableTicks = 0;
    assert(nearly(applyDamage(state, DamageType::Starve, 1.0F).exhaustion, 0.0F));
}

// Starving used to cost hunger, which meant starvation accelerated itself: the
// damage that hunger caused took more hunger. The regression this pins is a
// whole player-visible behaviour, so it is checked through the vitals rather
// than the table.
void testStarvationDoesNotFeedItself() {
    PlayerVitals vitals;
    vitals.reset();
    const float before = vitals.exhaustion();
    assert(vitals.hurt(1.0F, DamageType::Starve));
    assert(nearly(vitals.exhaustion(), before));

    // A mob's hit does charge the 0.1 vanilla charges.
    PlayerVitals bitten;
    bitten.reset();
    assert(bitten.hurt(3.0F, DamageType::EntityAttack, true));
    assert(nearly(bitten.exhaustion(), 0.1F));
}

// DamageScaling: a mob's swing scales with difficulty, the world's does not,
// even though both types carry the same scaling value. The condition is who
// caused it.
void testDifficultyScaling() {
    // Hard multiplies by 1.5.
    assert(nearly(scaleDamageForDifficulty(DamageType::EntityAttack, 4.0F, Difficulty::Hard, true),
                  6.0F));
    // The same swing on Normal is untouched, and on Easy follows vanilla's
    // piecewise rule.
    assert(nearly(
        scaleDamageForDifficulty(DamageType::EntityAttack, 4.0F, Difficulty::Normal, true), 4.0F));
    assert(nearly(scaleDamageForDifficulty(DamageType::EntityAttack, 4.0F, Difficulty::Easy, true),
                  3.0F));
    // Falling on Hard is still just falling: no living non-player swung it.
    assert(
        nearly(scaleDamageForDifficulty(DamageType::Fall, 4.0F, Difficulty::Hard, false), 4.0F));
    assert(nearly(scaleDamageForDifficulty(DamageType::Drown, 2.0F, Difficulty::Hard, false),
                  2.0F));

    // End to end through the pipeline: the same swing, two difficulties.
    DamageState easy{20.0F, 20.0F};
    static_cast<void>(applyDamage(easy, DamageContext{DamageType::EntityAttack, 4.0F,
                                                      Difficulty::Easy, true}));
    DamageState hard{20.0F, 20.0F};
    static_cast<void>(applyDamage(hard, DamageContext{DamageType::EntityAttack, 4.0F,
                                                      Difficulty::Hard, true}));
    assert(nearly(easy.health, 17.0F));
    assert(nearly(hard.health, 14.0F));
}

// The invulnerability window, and the tag that walks through it.
void testInvulnerabilityWindow() {
    DamageState state{20.0F, 20.0F};
    assert(applyDamage(state, DamageType::EntityAttack, 4.0F).landed);
    assert(nearly(state.health, 16.0F));
    // A weaker hit inside the window is swallowed whole.
    assert(!applyDamage(state, DamageType::EntityAttack, 2.0F).landed);
    assert(nearly(state.health, 16.0F));
    // A stronger one lands for the difference only.
    assert(applyDamage(state, DamageType::EntityAttack, 6.0F).landed);
    assert(nearly(state.health, 14.0F));

    // BYPASSES_INVULNERABILITY: the void does not queue behind a hit that
    // landed a moment ago. This used to work only because kill() passed an
    // infinite amount, which out-compared whatever was in the window; the tag
    // says it outright, so a finite void hit works too.
    DamageState voided{20.0F, 20.0F};
    assert(applyDamage(voided, DamageType::EntityAttack, 10.0F).landed);
    assert(applyDamage(voided, DamageType::OutOfWorld, 4.0F).landed);
    assert(nearly(voided.health, 6.0F));
    assert(hasDamageTag(DamageType::OutOfWorld, DamageTag::BypassesInvulnerability));
    assert(!hasDamageTag(DamageType::EntityAttack, DamageTag::BypassesInvulnerability));

    // kill() still kills outright, whatever is in the window.
    DamageState killed{20.0F, 20.0F};
    assert(applyDamage(killed, DamageType::EntityAttack, 19.0F).landed);
    const auto outcome = kill(killed);
    assert(outcome.landed && outcome.died);
    assert(killed.dead());
}

// The tag mask itself: one bit test, and the memberships vanilla declares.
void testTags() {
    assert(hasDamageTag(DamageType::Fall, DamageTag::IsFall));
    assert(hasDamageTag(DamageType::Drown, DamageTag::IsDrowning));
    assert(hasDamageTag(DamageType::Starve, DamageTag::BypassesEffects));
    assert(hasDamageTag(DamageType::OutOfWorld, DamageTag::BypassesResistance));
    assert(hasDamageTag(DamageType::OutOfWorld, DamageTag::NoKnockback));
    // Everything the world does bypasses armor; a mob's swing does not.
    assert(hasDamageTag(DamageType::Drown, DamageTag::BypassesArmor));
    assert(hasDamageTag(DamageType::Fall, DamageTag::BypassesArmor));
    assert(!hasDamageTag(DamageType::EntityAttack, DamageTag::BypassesArmor));
    // Tags a type does not carry read false rather than tripping over the mask.
    assert(!hasDamageTag(DamageType::Fall, DamageTag::IsDrowning));
    assert(!hasDamageTag(DamageType::Generic, DamageTag::IsFall));

    // The vanilla message ids travel with the data, so the mapping is checkable
    // rather than remembered.
    assert(damageTypeData(DamageType::EntityAttack).msgId == "mob");
    assert(damageTypeData(DamageType::OutOfWorld).msgId == "outOfWorld");
}

// The guards at the head of the pipeline.
void testGuards() {
    DamageState state{20.0F, 20.0F};
    assert(!applyDamage(state, DamageType::None, 5.0F).landed);
    assert(!applyDamage(state, DamageType::EntityAttack, 0.0F).landed);
    assert(!applyDamage(state, DamageType::EntityAttack, -3.0F).landed);
    assert(nearly(state.health, 20.0F));

    // Peaceful scales a mob's swing to nothing, and a hit that scales to zero
    // must not consume the window or set the hurt flash.
    DamageState peaceful{20.0F, 20.0F};
    const auto outcome = applyDamage(
        peaceful, DamageContext{DamageType::EntityAttack, 4.0F, Difficulty::Peaceful, true});
    assert(!outcome.landed);
    assert(nearly(peaceful.health, 20.0F));
    assert(peaceful.hurtTicks == 0);

    // A dead thing takes no more damage.
    DamageState corpse{0.0F, 20.0F};
    assert(!applyDamage(corpse, DamageType::EntityAttack, 5.0F).landed);
}

} // namespace

int main() {
    testExhaustionIsPerType();
    testStarvationDoesNotFeedItself();
    testDifficultyScaling();
    testInvulnerabilityWindow();
    testTags();
    testGuards();
    return 0;
}

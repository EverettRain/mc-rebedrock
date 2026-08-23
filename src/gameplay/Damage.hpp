#pragma once

#include "gameplay/DamageType.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace mc::gameplay {

// The shared "living thing" damage state, mirroring the server-side fields a
// LivingEntity carries. Both SimpleEntity and PlayerVitals embed one so the
// damage pipeline below is a single implementation for player and mob alike.
struct DamageState {
    float health = 0.0F;
    float maxHealth = 1.0F;
    int hurtTicks = 0;          // LivingEntity#hurtTime (red overlay)
    int invulnerableTicks = 0;  // LivingEntity#timeUntilRegen
    int deathTicks = 0;         // LivingEntity#deathTime (death animation)
    float lastDamage = 0.0F;    // LivingEntity#lastDamageTaken
    DamageType lastSource = DamageType::Generic;
    bool dying = false;         // onDeath already fired; guards a second death

    [[nodiscard]] bool dead() const { return health <= 0.0F; }
};

// LivingEntity#hurtTime / #deathTime, shared by every damageable thing.
inline constexpr int kHurtTicks = 10;
// LivingEntity#timeUntilRegen: for the first half of this window a second hit
// only lands if it is stronger than the one still running.
inline constexpr int kInvulnerableWindowTicks = 20;
// LivingEntity#deathTime: the corpse tips over for this many ticks, then is
// removed.
inline constexpr int kDeathTicks = 20;

// Entity#baseTick's fire cadence: a burning thing takes one point of OnFire
// damage every second (twenty ticks) while `fireTicks` counts down, matching
// vanilla's `this.fireTicks % 20 == 0` burn. setSecondsOnFire multiplies its
// argument by this to reach a tick count, so `setOnFire(5)` burns for 100 ticks
// and deals five points of damage.
inline constexpr int kFireDamageInterval = 20;
inline constexpr int kTicksPerSecond = 20;

struct DamageOutcome final {
    bool landed = false;  // the hit applied (not swallowed by the window)
    bool died = false;    // health crossed zero on this hit
    // The hunger the hit costs its victim, straight off the damage type. The
    // damage state has no hunger of its own, so the owner that does — the
    // player's vitals — adds it. This used to be a flat 0.1 the vitals applied
    // to every source alike, which meant drowning and starving drained hunger
    // that vanilla never charges for.
    float exhaustion = 0.0F;
    // What actually landed after scaling, for callers that report or animate it.
    float appliedDamage = 0.0F;
    // EQ-2: whether the armor/toughness stage actually ran on this hit — false
    // when the type carries BypassesArmor (the void, falling, drowning,
    // starving) or when the hit never reached that stage at all (swallowed by
    // a guard or the invulnerability window). The caller uses this, not
    // "landed", to decide whether worn armor spends durability: armor that
    // was bypassed absorbed nothing and must not wear out from a hit it never
    // touched.
    bool armorApplied = false;
    // EQ-2: the damage the armor/toughness stage was handed — after the
    // invulnerability window and difficulty scaling, but before the armor
    // reduction itself — zero unless armorApplied is true. This is the value
    // PlayerInventory#damageArmor (1.16.1) divides by four for durability, so
    // the caller applying armor wear needs it rather than appliedDamage
    // (which is already-reduced) or the raw incoming amount (which is not
    // yet difficulty-scaled).
    float preArmorDamage = 0.0F;
};

// Everything one hit is. 26.1's DamageType is the type plus who caused it and
// from where; the parts that change the arithmetic here are the type, the
// difficulty, and whether a living non-player swung it.
struct DamageContext final {
    DamageType type = DamageType::Generic;
    float amount = 0.0F;
    Difficulty difficulty = Difficulty::Normal;
    // DamageScaling::WhenCausedByLivingNonPlayer's condition. False for the
    // world hurting you: falling, drowning, starving, the void.
    bool causedByLivingNonPlayer = false;
    // EQ-2: the defender's summed armor points / toughness at the moment of
    // the hit (LivingEntity#getArmor / #getAttributeValue(GENERIC_ARMOR_
    // TOUGHNESS), each a sum across the four equipped ArmorItem modifiers).
    // Zero for every mob today (no mob armor yet) and for any caller that
    // predates this field — appended at the end, defaulted, so every existing
    // aggregate-init call site (positional, ending at causedByLivingNonPlayer)
    // keeps compiling unchanged.
    float armor = 0.0F;
    float armorToughness = 0.0F;
};

// DamageUtil#getDamageLeft (1.16.1, `net.minecraft.entity.DamageUtil`),
// transcribed symbol-for-symbol rather than reconstructed from the wiki:
//
//   float f = 2.0F + armorToughness / 4.0F;
//   float g = clamp(armor - damage / f, armor * 0.2F, 20.0F);
//   return damage * (1.0F - g / 25.0F);
//
// A pure function on purpose (EQ-DESIGN.md §3): it takes the three numbers
// the formula actually needs and returns the one it produces, so it is
// unit-testable in isolation from the DamageState/EquipmentSlots machinery
// that gathers those numbers, and slots into the pipeline's named "armor /
// toughness" stage as a single call rather than inlined arithmetic.
[[nodiscard]] constexpr float damageAfterArmor(float damage, float armor, float armorToughness) {
    const float toughnessDivisor = 2.0F + armorToughness / 4.0F;
    const float reduction =
        std::clamp(armor - damage / toughnessDivisor, armor * 0.2F, 20.0F);
    return damage * (1.0F - reduction / 25.0F);
}

// LivingEntity#hurt, as the fixed pipeline it is in vanilla rather than a
// subtraction with the difficulty already folded in by whoever called it:
//
//   guards -> invulnerability window -> difficulty scaling -> armor/toughness
//   -> effects/enchantments -> absorption -> shield -> health -> exhaustion
//   -> death
//
// EQ-2 fills the armor/toughness stage; the other three (status effect,
// absorption, shield) still have no content in this game and stay named here
// rather than implemented, so the order is already right when they arrive —
// inserting a stage into a named sequence is a different job from
// rediscovering where it went. The tags each stage consults (BypassesArmor,
// BypassesEffects, BypassesResistance, BypassesShield) already exist and are
// already set.
inline DamageOutcome applyDamage(DamageState& state, const DamageContext& context) {
    // --- guards ---
    if (state.dead() || context.type == DamageType::None || context.amount <= 0.0F) {
        return {};
    }

    // --- invulnerability window ---
    // Inside the first half of the window a second hit only lands if it beats
    // the one still running, and then only for the difference. The comparison
    // is against the *unscaled* amount, the way vanilla does it: scaling
    // happens in applyDamage, after this check in LivingEntity#hurt.
    float amount = context.amount;
    const bool bypassesWindow =
        hasDamageTag(context.type, DamageTag::BypassesInvulnerability) ||
        hasDamageTag(context.type, DamageTag::BypassesCooldown);
    if (!bypassesWindow && state.invulnerableTicks > kInvulnerableWindowTicks / 2) {
        if (amount <= state.lastDamage) {
            return {};
        }
        amount -= state.lastDamage;
    }
    state.lastDamage = context.amount;
    state.invulnerableTicks = kInvulnerableWindowTicks;

    // --- difficulty scaling ---
    float applied = scaleDamageForDifficulty(context.type, amount, context.difficulty,
                                             context.causedByLivingNonPlayer);

    // --- armor / toughness ---
    // LivingEntity#applyArmorToDamage: BypassesArmor short-circuits the whole
    // stage (the void, falling, drowning, starving hit for the scaled amount
    // untouched); everything else — a mob's swing, an arrow, fire — runs
    // through the vanilla formula against whatever the defender has summed
    // from its four worn armor pieces (zero for an unarmored player or any
    // mob, which is a no-op reduction: damageAfterArmor(d, 0, *) == d).
    const bool bypassesArmor = hasDamageTag(context.type, DamageTag::BypassesArmor);
    bool armorApplied = false;
    float preArmorDamage = 0.0F;
    if (!bypassesArmor && context.armor > 0.0F) {
        preArmorDamage = applied;
        applied = damageAfterArmor(applied, context.armor, context.armorToughness);
        armorApplied = true;
    }
    // --- status effects / enchantments ---  (BypassesEffects / BypassesResistance)
    // --- absorption ---
    // --- shield ---                 (BypassesShield)
    if (applied <= 0.0F) {
        return {};
    }

    // --- health ---
    state.hurtTicks = kHurtTicks;
    state.lastSource = context.type;
    state.health = std::max(state.health - applied, 0.0F);

    DamageOutcome outcome;
    outcome.landed = true;
    outcome.died = state.dead();
    // --- exhaustion ---
    outcome.exhaustion = damageTypeData(context.type).exhaustion;
    outcome.appliedDamage = applied;
    outcome.armorApplied = armorApplied;
    outcome.preArmorDamage = preArmorDamage;
    return outcome;
}

// The short form for the many callers that have no difficulty and no attacker:
// the world hurting something.
inline DamageOutcome applyDamage(DamageState& state, DamageType type, float amount) {
    return applyDamage(state, DamageContext{type, amount});
}

// Entity#kill / LivingEntity#kill: out-of-world damage at infinite magnitude.
// The type carries BYPASSES_INVULNERABILITY, so the hit cuts through a window
// that a moment-ago hit left running instead of relying on the magnitude to
// out-compare it.
inline DamageOutcome kill(DamageState& state) {
    return applyDamage(state, DamageType::OutOfWorld,
                       std::numeric_limits<float>::infinity());
}

// The unified death-event guard, mirroring the `dead` field LivingEntity#onDeath
// sets to keep itself from running twice: returns true only the first time.
inline bool beginDeath(DamageState& state) {
    if (state.dying) {
        return false;
    }
    state.dying = true;
    return true;
}

// LivingEntity#updatePostDeath: advance the death animation one tick and report
// whether the corpse should now be removed.
inline bool advanceDeath(DamageState& state) {
    ++state.deathTicks;
    return state.deathTicks >= kDeathTicks;
}

} // namespace mc::gameplay

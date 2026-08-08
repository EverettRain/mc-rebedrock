#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace mc::gameplay {

// Unified damage source, mirroring the roles of 1.16.1's DamageSource. Replaces
// the player-only DamageCause: Void becomes OutOfWorld, and None is kept as the
// "nothing happened this tick" sentinel shared with VitalsTickResult.
enum class DamageSource : std::uint8_t {
    None,         // no damage this tick / an applyDamage() guard
    Generic,      // GENERIC: untyped damage (poison, command fallback, ...)
    EntityAttack, // a melee hit from another creature
    Fall,
    Drown,
    Starve,
    OutOfWorld,   // void damage and the /kill source (OUT_OF_WORLD)
};

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
    DamageSource lastSource = DamageSource::Generic;
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

struct DamageOutcome final {
    bool landed = false;  // the hit applied (not swallowed by the window)
    bool died = false;    // health crossed zero on this hit
};

// LivingEntity#applyDamage core: guards + the invulnerability window + the
// health subtraction. Knockback and per-owner side effects (anger, flee, loot,
// death screens) stay with the caller; this is pure damage-state math.
inline DamageOutcome applyDamage(DamageState& state, DamageSource source, float amount) {
    if (state.dead() || source == DamageSource::None || amount <= 0.0F) {
        return {};
    }
    // Inside the first half of the window a second hit only lands if it beats
    // the one still running, and then only for the difference.
    float applied = amount;
    if (state.invulnerableTicks > kInvulnerableWindowTicks / 2) {
        if (amount <= state.lastDamage) {
            return {};
        }
        applied = amount - state.lastDamage;
    }
    state.lastDamage = amount;
    state.invulnerableTicks = kInvulnerableWindowTicks;
    state.hurtTicks = kHurtTicks;
    state.lastSource = source;
    state.health = std::max(state.health - applied, 0.0F);
    return DamageOutcome{true, state.dead()};
}

// Entity#kill / LivingEntity#kill: OutOfWorld damage at infinite magnitude, so
// the hit always cuts through the invulnerability window and kills outright.
// One call serves the player and every mob; an already-dead target is a no-op.
inline DamageOutcome kill(DamageState& state) {
    return applyDamage(state, DamageSource::OutOfWorld,
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

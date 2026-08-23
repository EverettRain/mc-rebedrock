#include "gameplay/PlayerVitals.hpp"

#include <algorithm>
#include <cmath>

namespace mc::gameplay {
namespace {

// Player#checkMovementStatistics tracks distance in centimetres, so the
// exhaustion granted per block is the per-centimetre rate times one hundred.
constexpr float kSprintExhaustionPerBlock = 0.1F;
constexpr float kSwimExhaustionPerBlock = 0.01F;
constexpr float kJumpExhaustion = 0.05F;
constexpr float kSprintJumpExhaustion = 0.2F;
constexpr float kRegenerationExhaustion = 6.0F;
constexpr float kVoidHeight = -64.0F;
constexpr float kVoidDamage = 4.0F;
constexpr float kDrownDamage = 2.0F;
// LivingEntity#decreaseAirSupply runs the counter to -20 before each hit.
constexpr int kDrownDamageAirTicks = -20;

} // namespace

void PlayerVitals::addExhaustion(float amount) {
    if (dead()) {
        return;
    }
    exhaustion_ = std::min(exhaustion_ + amount, 40.0F);
}

void PlayerVitals::eat(int food, float saturationModifier) {
    // FoodStats#eat: food always lands, and saturation arrives at twice the
    // food times the modifier, capped by the new food level.
    foodLevel_ = std::min(foodLevel_ + food, kMaximumFood);
    saturation_ = std::min(
        saturation_ + static_cast<float>(food) * saturationModifier * 2.0F,
        static_cast<float>(foodLevel_));
}

void PlayerVitals::heal(float amount) {
    if (dead() || amount <= 0.0F) {
        return;
    }
    damage_.health = std::min(damage_.health + amount, damage_.maxHealth);
}

bool PlayerVitals::hurt(float amount, DamageType cause, bool causedByLivingNonPlayer,
                        float armor, float armorToughness, bool* armorApplied,
                        float* preArmorDamage) {
    // Guards, the invulnerability window and the difficulty scaling all live in
    // the shared pipeline, so the player and every mob resolve a hit the same
    // way — and the difficulty is applied once, here, rather than by whichever
    // caller remembered to. EQ-2's armor/toughness stage lives in the same
    // shared pipeline, so a player hit is reduced exactly the way a future
    // armored mob's would be.
    const DamageOutcome outcome = applyDamage(
        damage_, DamageContext{cause, amount, difficulty_, causedByLivingNonPlayer, armor,
                               armorToughness});
    if (armorApplied != nullptr) {
        *armorApplied = outcome.armorApplied;
    }
    if (preArmorDamage != nullptr) {
        *preArmorDamage = outcome.preArmorDamage;
    }
    if (!outcome.landed) {
        return false;
    }
    ticksSinceDamage_ = 0;
    // The hunger a hit costs is the damage type's own exhaustion. This was a
    // flat 0.1 charged for every source, so drowning and starving drained
    // hunger that vanilla charges nothing for — and starvation, which damages
    // *because* hunger ran out, was feeding itself.
    addExhaustion(outcome.exhaustion);
    return true;
}

void PlayerVitals::setOnFire(int seconds) {
    if (dead() || seconds <= 0) {
        return;
    }
    fireTicks_ = std::max(fireTicks_, seconds * kTicksPerSecond);
}

bool PlayerVitals::applyEffect(core::StatusEffectId effect, std::int32_t durationTicks,
                               std::uint8_t amplifier) {
    if (dead()) {
        return false;
    }
    return mc::gameplay::applyEffect(effects_, effect, durationTicks, amplifier);
}

bool PlayerVitals::removeEffect(core::StatusEffectId effect) {
    return mc::gameplay::removeEffect(effects_, effect);
}

std::size_t PlayerVitals::clearEffects() {
    return mc::gameplay::clearEffects(effects_);
}

bool PlayerVitals::hasEffect(core::StatusEffectId effect) const {
    return mc::gameplay::hasEffect(effects_, effect);
}

void PlayerVitals::reset() {
    damage_ = DamageState{kMaximumHealth, kMaximumHealth};
    foodLevel_ = kMaximumFood;
    saturation_ = 5.0F;
    exhaustion_ = 0.0F;
    foodTimer_ = 0;
    airTicks_ = kMaximumAirTicks;
    fallDistance_ = 0.0F;
    fireTicks_ = 0;
    // A respawn wipes every effect, matching vanilla's clean slate.
    mc::gameplay::clearEffects(effects_);
    speedMultiplier_ = 1.0F;
    ticksSinceDamage_ = 1000;
}

void PlayerVitals::restore(float health, int foodLevel, float saturation, int airTicks) {
    damage_.health = std::clamp(health, 0.0F, kMaximumHealth);
    damage_.invulnerableTicks = 0;
    damage_.lastDamage = 0.0F;
    damage_.deathTicks = 0;
    damage_.dying = false;
    foodLevel_ = std::clamp(foodLevel, 0, kMaximumFood);
    saturation_ = std::clamp(saturation, 0.0F, static_cast<float>(kMaximumFood));
    airTicks_ = std::clamp(airTicks, kDrownDamageAirTicks, kMaximumAirTicks);
    exhaustion_ = 0.0F;
    foodTimer_ = 0;
    fallDistance_ = 0.0F;
    ticksSinceDamage_ = 1000;
}

void PlayerVitals::tickFood(VitalsTickResult& result) {
    // FoodData#tick, on normal difficulty with natural regeneration enabled.
    if (exhaustion_ > 4.0F) {
        exhaustion_ -= 4.0F;
        if (saturation_ > 0.0F) {
            saturation_ = std::max(saturation_ - 1.0F, 0.0F);
        } else {
            foodLevel_ = std::max(foodLevel_ - 1, 0);
        }
    }

    const bool hurtPlayer = damage_.health < kMaximumHealth;
    if (saturation_ > 0.0F && hurtPlayer && foodLevel_ >= kMaximumFood) {
        ++foodTimer_;
        if (foodTimer_ >= 10) {
            const float healed = std::min(saturation_, 6.0F);
            heal(healed / 6.0F);
            addExhaustion(healed);
            foodTimer_ = 0;
        }
    } else if (foodLevel_ >= 18 && hurtPlayer) {
        ++foodTimer_;
        if (foodTimer_ >= 80) {
            heal(1.0F);
            addExhaustion(kRegenerationExhaustion);
            foodTimer_ = 0;
        }
    } else if (foodLevel_ <= 0) {
        ++foodTimer_;
        if (foodTimer_ >= 80) {
            // How far starvation is allowed to run is the difficulty's whole
            // contribution here: easy stops at five hearts, normal at half a
            // heart, hard carries on to the end.
            if (damage_.health > starvationHealthFloor(difficulty_) &&
                hurt(1.0F, DamageType::Starve)) {
                result.damageTaken = 1.0F;
                result.cause = DamageType::Starve;
            }
            foodTimer_ = 0;
        }
    } else {
        foodTimer_ = 0;
    }
}

// PlayerEntity#tick's peaceful branch: a health point a second and a food point
// every half second, for free.
void PlayerVitals::tickPeacefulRegeneration() {
    if (!regeneratesFreely(difficulty_)) {
        return;
    }
    if (damage_.health < kMaximumHealth && ageTicks_ % 20 == 0) {
        heal(1.0F);
    }
    if (foodLevel_ < kMaximumFood && ageTicks_ % 10 == 0) {
        foodLevel_ = std::min(foodLevel_ + 1, kMaximumFood);
    }
}

VitalsTickResult PlayerVitals::tick(const VitalsInput& input) {
    VitalsTickResult result;
    ++ageTicks_;
    if (damage_.invulnerableTicks > 0) {
        --damage_.invulnerableTicks;
    }
    if (ticksSinceDamage_ < 1000) {
        ++ticksSinceDamage_;
    }
    if (dead()) {
        return result;
    }

    // Entity#checkFallDistance: water, flight and standing on the ground all
    // clear the accumulated fall.
    if (input.flying || input.inWater) {
        fallDistance_ = 0.0F;
    } else if (input.onGround) {
        const float damage = std::ceil(fallDistance_ - kSafeFallDistance);
        fallDistance_ = 0.0F;
        if (damage > 0.0F && hurt(damage, DamageType::Fall)) {
            result.damageTaken = damage;
            result.cause = DamageType::Fall;
        }
    } else if (input.verticalDistance < 0.0F) {
        fallDistance_ -= input.verticalDistance;
    }

    // Air supply, then drowning damage once it runs past -20.
    if (input.headInWater) {
        --airTicks_;
        if (airTicks_ <= kDrownDamageAirTicks) {
            airTicks_ = 0;
            if (hurt(kDrownDamage, DamageType::Drown)) {
                result.damageTaken = kDrownDamage;
                result.cause = DamageType::Drown;
            }
        }
    } else if (airTicks_ < kMaximumAirTicks) {
        airTicks_ = std::min(airTicks_ + 4, kMaximumAirTicks);
    }

    if (input.feetY < kVoidHeight) {
        if (hurt(kVoidDamage, DamageType::OutOfWorld)) {
            result.damageTaken = kVoidDamage;
            result.cause = DamageType::OutOfWorld;
        }
    }

    // Entity#baseTick's fire block, the player's half. Water or rain under open
    // sky puts the fire out; otherwise it takes one point of OnFire damage each
    // second while fireTicks counts down (vanilla's `fireTicks % 20 == 0` burn).
    if (fireTicks_ > 0) {
        if (input.inWater || input.rainedOn) {
            fireTicks_ = 0;
        } else {
            if (fireTicks_ % kFireDamageInterval == 0 && hurt(1.0F, DamageType::OnFire)) {
                result.damageTaken = 1.0F;
                result.cause = DamageType::OnFire;
            }
            --fireTicks_;
        }
    }

    // LivingEntity#tickEffects, the player's half: advance the active MobEffects
    // and apply what the tick produced. Poison hurts (never below one health),
    // regeneration heals, hunger drains food exhaustion, and speed/slowness set
    // the movement factor the controller reads. The factor is recomputed every
    // tick, so it returns to 1.0 the moment neither effect is active.
    speedMultiplier_ = 1.0F;
    if (!effects_.empty()) {
        const EffectTickOutcome effectTick = tickEffects(effects_, damage_.health);
        if (effectTick.heal > 0.0F) {
            heal(effectTick.heal);
        }
        if (effectTick.damage > 0.0F && hurt(effectTick.damage, effectTick.damageType)) {
            result.damageTaken = effectTick.damage;
            result.cause = effectTick.damageType;
        }
        if (effectTick.exhaustion > 0.0F) {
            addExhaustion(effectTick.exhaustion);
        }
        speedMultiplier_ = effectTick.speedMultiplier;
    }

    if (input.jumped) {
        addExhaustion(input.sprinting ? kSprintJumpExhaustion : kJumpExhaustion);
    }
    if (input.horizontalDistance > 0.0F) {
        if (input.inWater) {
            addExhaustion(kSwimExhaustionPerBlock * input.horizontalDistance);
        } else if (input.onGround && input.sprinting) {
            addExhaustion(kSprintExhaustionPerBlock * input.horizontalDistance);
        }
    }

    tickPeacefulRegeneration();
    tickFood(result);
    result.died = dead();
    return result;
}

} // namespace mc::gameplay

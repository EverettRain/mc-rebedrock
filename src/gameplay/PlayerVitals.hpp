#pragma once

#include "core/ContentId.hpp"
#include "gameplay/Damage.hpp"
#include "gameplay/Difficulty.hpp"
#include "gameplay/StatusEffect.hpp"

#include <cstdint>

namespace mc::gameplay {

// One physics tick of the player state the survival systems react to.
struct VitalsInput final {
    // Horizontal blocks travelled this tick, used for sprinting exhaustion.
    float horizontalDistance = 0.0F;
    // Signed vertical movement, negative while falling.
    float verticalDistance = 0.0F;
    bool onGround = false;
    bool sprinting = false;
    bool jumped = false;
    bool inWater = false;
    bool headInWater = false;
    bool flying = false;
    float feetY = 0.0F;
    // Whether the player stands in the rain under open sky this tick, so a
    // burning player is put out the way Entity#baseTick's isBeingRainedOn does.
    // Defaulted false: an ignition source (lava, fire) is a later content node.
    bool rainedOn = false;
};

struct VitalsTickResult final {
    float damageTaken = 0.0F;
    DamageType cause = DamageType::None;
    bool died = false;
};

// Java 1.16.1 health, hunger and environmental damage for a survival player.
// Creative players simply do not tick this.
class PlayerVitals final {
  public:
    static constexpr float kMaximumHealth = 20.0F;
    static constexpr int kMaximumFood = 20;
    static constexpr int kMaximumAirTicks = 300;
    static constexpr float kSafeFallDistance = 3.0F;

    [[nodiscard]] float health() const { return damage_.health; }
    [[nodiscard]] int foodLevel() const { return foodLevel_; }
    [[nodiscard]] float saturation() const { return saturation_; }
    [[nodiscard]] float exhaustion() const { return exhaustion_; }
    [[nodiscard]] int airTicks() const { return airTicks_; }
    [[nodiscard]] float fallDistance() const { return fallDistance_; }
    [[nodiscard]] int fireTicks() const { return fireTicks_; }
    // The player's active MobEffects, read by the persistence/HUD paths.
    [[nodiscard]] const ActiveEffects& effects() const { return effects_; }
    // The movement-speed factor the active speed/slowness effects impose this
    // tick (1.0 with neither). PlayerController multiplies its walk speed by it,
    // so the boost/slow appears and — because it is recomputed every tick from
    // the live effect set — vanishes the instant the effect expires.
    [[nodiscard]] float speedMultiplier() const { return speedMultiplier_; }
    [[nodiscard]] bool dead() const { return damage_.dead(); }
    // The shared damage state, so GameSession can run the unified onDeath guard
    // (beginDeath) once across the player's death paths.
    [[nodiscard]] DamageState& damage() { return damage_; }
    [[nodiscard]] const DamageState& damage() const { return damage_; }
    [[nodiscard]] int invulnerableTicks() const { return damage_.invulnerableTicks; }
    // Ticks since the last hit, used to drive the HUD's damage flash.
    [[nodiscard]] int ticksSinceDamage() const { return ticksSinceDamage_; }

    [[nodiscard]] Difficulty difficulty() const { return difficulty_; }
    void setDifficulty(Difficulty difficulty) { difficulty_ = difficulty; }

    VitalsTickResult tick(const VitalsInput& input);
    // Player#causeFoodExhaustion, for actions outside movement such as mining.
    void addExhaustion(float amount);
    // FoodStats#eat: restores hunger and saturation from a finished meal.
    void eat(int food, float saturationModifier);
    void heal(float amount);
    // `causedByLivingNonPlayer` is DamageScaling::WhenCausedByLivingNonPlayer's
    // condition: a mob swung it, so a harder world swings harder. False for the
    // world hurting the player — falling, drowning, starving, the void.
    //
    // EQ-2: `armor`/`toughness` are the defender's summed armor points and
    // toughness (GameSession::hurtPlayer sums the four EquipmentSlots pieces
    // before calling this); every internal caller in this file — fall,
    // drowning, fire, starvation, effect ticks — leaves them at the zero
    // default, which is correct either way: those types all carry
    // BypassesArmor except OnFire, and an unarmored player reduces by zero
    // regardless. `armorApplied`, if given, receives whether the armor stage
    // actually ran on a landed hit (false on a miss, a bypass, or no armor
    // worn); `preArmorDamage`, if given, receives the damage the stage was
    // handed (post-difficulty, pre-reduction). The caller sums both to decide
    // whether, and by how much, worn armor spends durability.
    bool hurt(float amount, DamageType cause, bool causedByLivingNonPlayer = false,
             float armor = 0.0F, float armorToughness = 0.0F, bool* armorApplied = nullptr,
             float* preArmorDamage = nullptr);
    // Entity#setSecondsOnFire: lights the player for `seconds` of burning, the
    // single entry every ignition source routes through. Vanilla only ever
    // lengthens a burn, so this takes the max; a dead player is not relit. The
    // burn itself is resolved in tick() through the shared damage pipeline.
    void setOnFire(int seconds);
    // LivingEntity#addEffect / #removeEffect / #removeAllEffects, over the same
    // shared StatusEffect store the mobs use. clearEffects returns the count
    // removed (a milk drink reports how many it wiped). A respawn (reset) also
    // clears every effect, matching vanilla.
    bool applyEffect(core::StatusEffectId effect, std::int32_t durationTicks,
                     std::uint8_t amplifier);
    bool removeEffect(core::StatusEffectId effect);
    std::size_t clearEffects();
    [[nodiscard]] bool hasEffect(core::StatusEffectId effect) const;
    // Restores a respawning player to full health and food.
    void reset();
    void restore(float health, int foodLevel, float saturation, int airTicks);
    // Restores the persisted active effects on world load.
    void restoreEffects(const ActiveEffects& effects) { effects_ = effects; }

  private:
    void tickFood(VitalsTickResult& result);
    void tickPeacefulRegeneration();

    Difficulty difficulty_ = Difficulty::Normal;
    // PlayerEntity#age, only needed for the peaceful regeneration cadence.
    int ageTicks_ = 0;

    // The shared damage state drives health, the invulnerability window and the
    // death timers; the fields below are player-only survival bookkeeping.
    DamageState damage_{kMaximumHealth, kMaximumHealth};
    int foodLevel_ = kMaximumFood;
    float saturation_ = 5.0F;
    float exhaustion_ = 0.0F;
    int foodTimer_ = 0;
    int airTicks_ = kMaximumAirTicks;
    float fallDistance_ = 0.0F;
    // Entity#fireTicks: how long the player stays ablaze. Zero unless an
    // ignition source lit them; water and rain put it out.
    int fireTicks_ = 0;
    // The player's active MobEffects, the same fixed inline store the mobs use.
    ActiveEffects effects_{};
    // The movement factor the active speed/slowness effects impose, recomputed
    // every tick so it reverts to 1.0 the moment neither is present.
    float speedMultiplier_ = 1.0F;
    int ticksSinceDamage_ = 1000;
};

} // namespace mc::gameplay

#pragma once

#include "gameplay/Damage.hpp"
#include "gameplay/Difficulty.hpp"

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
};

struct VitalsTickResult final {
    float damageTaken = 0.0F;
    DamageSource cause = DamageSource::None;
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
    [[nodiscard]] bool dead() const { return damage_.dead(); }
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
    bool hurt(float amount, DamageSource cause);
    // Restores a respawning player to full health and food.
    void reset();
    void restore(float health, int foodLevel, float saturation, int airTicks);

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
    int ticksSinceDamage_ = 1000;
};

} // namespace mc::gameplay

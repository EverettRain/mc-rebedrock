#pragma once

#include <glm/vec3.hpp>

namespace mc::world {
class World;
}

namespace mc::gameplay {

struct PlayerInput final {
    float forward = 0.0F;
    float strafe = 0.0F;
    glm::vec3 lookDirection{0.0F, 0.0F, -1.0F};
    bool jumpHeld = false;
    bool descendHeld = false;
    bool sneakHeld = false;
    bool sprintHeld = false;
    bool forwardPressed = false;
    bool jumpPressed = false;
    bool flightAllowed = false;
    // Vanilla's `bl3` in ClientPlayerEntity#tickMovement: sprinting needs a food
    // level above 6, unless the player may fly. Survival clears this when the
    // hunger bar drops that far; creative and the tests leave it set.
    bool sprintAllowed = true;
    // Bedrock-style auto-jump: while walking forward into a one-block rise the
    // player jumps on its own instead of jamming against the face. Off by
    // default, mirroring Java 1.16.1.
    bool autoJump = false;
};

class PlayerController final {
  public:
    static constexpr float kTickSeconds = 1.0F / 20.0F;
    static constexpr float kWidth = 0.6F;
    static constexpr float kHeight = 1.8F;
    static constexpr float kEyeHeight = 1.62F;
    static constexpr float kSneakingHeight = 1.5F;
    static constexpr float kSneakingEyeHeight = 1.27F;
    // GENERIC_MOVEMENT_SPEED's base value, and the sprint modifier applied on
    // top of it. Both the acceleration and the FOV multiplier read them.
    static constexpr float kWalkSpeed = 0.1F;
    static constexpr float kSprintSpeedMultiplier = 1.3F;
    // Vanilla's base FOV before the movement multiplier scales it.
    static constexpr float kBaseFieldOfViewDegrees = 70.0F;

    explicit PlayerController(glm::vec3 feetPosition);

    void tick(const world::World& world, const PlayerInput& input);
    void setPosition(glm::vec3 feetPosition);
    // ServerPlayerEntity#respawn: drops the new body onto the respawn point with
    // every transient state cleared — momentum, flight/sprint/sneak, fall
    // distance, jump cooldowns and the FOV multiplier — so nothing from the
    // death is carried across.
    void resetForRespawn(glm::vec3 feetPosition);
    // Entity#addVelocity, for shoves that come from outside the player's own
    // input — currently the creatures they walk into.
    void applyExternalPush(glm::vec3 velocity) { velocity_ += velocity; }

    [[nodiscard]] glm::vec3 position() const { return position_; }
    [[nodiscard]] glm::vec3 eyePosition() const;
    [[nodiscard]] float eyeHeight() const {
        return sneaking_ ? kSneakingEyeHeight : kEyeHeight;
    }
    [[nodiscard]] glm::vec3 velocity() const { return velocity_; }
    [[nodiscard]] bool onGround() const { return onGround_; }
    [[nodiscard]] bool flying() const { return flying_; }
    [[nodiscard]] bool sprinting() const { return sprinting_; }
    [[nodiscard]] bool inWater() const { return inWater_; }
    [[nodiscard]] bool sneaking() const { return sneaking_; }
    [[nodiscard]] float horizontalSpeed() const { return horizontalSpeed_; }
    [[nodiscard]] float previousHorizontalSpeed() const { return previousHorizontalSpeed_; }
    [[nodiscard]] float strideDistance() const { return strideDistance_; }
    [[nodiscard]] float previousStrideDistance() const { return previousStrideDistance_; }
    [[nodiscard]] bool intersectsBlock(int x, int y, int z) const;
    // Whether the last move was stopped by a wall on X or Z, the way vanilla's
    // Entity#horizontalCollision reads after move(). Cancels sprinting.
    [[nodiscard]] bool horizontalCollision() const { return horizontalCollision_; }
    // Entity#fallDistance: how far the player has fallen since last touching the
    // ground, in blocks. Accumulated each airborne tick from the downward motion
    // and reset once on the ground. The farmland trample reads it on landing.
    [[nodiscard]] float fallDistance() const { return fallDistance_; }
    // Whether the last tick actually left the ground under its own power. The
    // jump is decided inside the tick now that a held key repeats it, so the
    // hunger exhaustion has to read the result rather than the input.
    [[nodiscard]] bool jumpedThisTick() const { return jumpedThisTick_; }
    // AbstractClientPlayerEntity#fovMultiplier: what the base 70° FOV is scaled
    // by. Held as a previous/current pair so the render frame can interpolate it
    // between physics ticks, exactly like the player position.
    [[nodiscard]] float fieldOfViewMultiplier() const { return fovMultiplier_; }
    [[nodiscard]] float previousFieldOfViewMultiplier() const {
        return previousFovMultiplier_;
    }

  private:
    [[nodiscard]] bool collidesAt(const world::World& world, glm::vec3 position) const;
    // Moves one axis and reports whether the move was stopped by a collision,
    // so moveWithCollisions can retry the horizontal move from a step up.
    [[nodiscard]] bool moveAxis(const world::World& world, int axis, float distance);
    void moveWithCollisions(const world::World& world, glm::vec3 distance);
    // LivingEntity#maxUpStep: re-attempt a wall-blocked horizontal move from a
    // step up, keeping the move only if the lifted body clears it and lands.
    void stepUp(const world::World& world, glm::vec3 distance, glm::vec3 beforeHorizontal);
    // Whether a jump clears the one-block rise the player is currently walking
    // into: the body lifted a full block over the blocked forward cell is open.
    [[nodiscard]] bool autoJumpCanClear(const world::World& world, glm::vec3 forward) const;
    [[nodiscard]] glm::vec3 adjustMovementForSneaking(
        const world::World& world,
        glm::vec3 distance) const;
    void updateFieldOfViewMultiplier();
    [[nodiscard]] float collisionHeight() const {
        return sneaking_ ? kSneakingHeight : kHeight;
    }

    glm::vec3 position_;
    glm::vec3 velocity_{0.0F};
    bool onGround_ = false;
    bool flying_ = false;
    bool sprinting_ = false;
    bool inWater_ = false;
    bool sneaking_ = false;
    bool horizontalCollision_ = false;
    float fallDistance_ = 0.0F;
    bool jumpedThisTick_ = false;
    int flightToggleWindowTicks_ = 0;
    int sprintDoubleTapWindowTicks_ = 0;
    // LivingEntity#jumpingCooldown: ten ticks between two jumps, which is what
    // lets a held jump key bunny-hop without re-pressing it.
    int jumpingCooldownTicks_ = 0;
    float fovMultiplier_ = 1.0F;
    float previousFovMultiplier_ = 1.0F;
    float horizontalSpeed_ = 0.0F;
    float previousHorizontalSpeed_ = 0.0F;
    float strideDistance_ = 0.0F;
    float previousStrideDistance_ = 0.0F;
};

} // namespace mc::gameplay

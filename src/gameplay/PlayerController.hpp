#pragma once

#include <glm/vec3.hpp>

#include <cstdint>

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

// The player's body posture, 26.1's EntityPose. The pose is an explicit state
// resolved once per tick from input and the space overhead — not a raw shift
// derivation — so releasing crouch under a 1.5-high ceiling (a slab/trapdoor)
// keeps the player crouched until it can actually stand, instead of snapping the
// 1.8 body up into the block. Standing/Crouching land now; the rest are reserved
// so the collision-height and pose-solver switches are written once.
enum class Pose : std::uint8_t {
    Standing,
    Crouching,
    // Reserved for later content; no behaviour keys off them yet.
    Swimming,
    FallFlying,
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
    // 26.1 Abilities.DEFAULT_FLYING_SPEED and LocalPlayer.jumpTriggerTime.
    // Vertical creative movement adds flyingSpeed * 3 each tick.
    static constexpr float kCreativeFlyingSpeed = 0.05F;
    static constexpr int kCreativeFlightToggleWindowTicks = 7;
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
        return pose_ == Pose::Crouching ? kSneakingEyeHeight : kEyeHeight;
    }
    [[nodiscard]] glm::vec3 velocity() const { return velocity_; }
    [[nodiscard]] bool onGround() const { return onGround_; }
    [[nodiscard]] bool flying() const { return flying_; }
    [[nodiscard]] bool sprinting() const { return sprinting_; }
    [[nodiscard]] bool inWater() const { return inWater_; }
    // The current body posture. Everything crouch-related (collision height, eye
    // height, the sneak edge-guard, the pose the render snapshot carries) reads
    // this single state instead of re-deriving from the raw shift key.
    [[nodiscard]] Pose pose() const { return pose_; }
    // Kept as the boolean the rest of the game (interaction, snapshot, sounds)
    // already asks for; now it is a view of the pose, not a separate flag.
    [[nodiscard]] bool sneaking() const { return pose_ == Pose::Crouching; }
    // Whether the standing (1.8) body fits at the current feet position — i.e.
    // there is headroom to stand up. Pure query against the world; the pose
    // solver uses it to keep a low-ceilinged player crouched.
    [[nodiscard]] bool canStandUp(const world::World& world) const {
        return !collidesAtHeight(world, position_, kHeight);
    }
    [[nodiscard]] float horizontalSpeed() const { return horizontalSpeed_; }
    [[nodiscard]] float previousHorizontalSpeed() const { return previousHorizontalSpeed_; }
    [[nodiscard]] float strideDistance() const { return strideDistance_; }
    [[nodiscard]] float previousStrideDistance() const { return previousStrideDistance_; }
    // ANIM A1/A2: vanilla WalkAnimationState split into two independent quantities,
    // distinct from the camera bob above.
    //  - walkAnimationSpeed = the gait AMPLITUDE: target = min(4 * d, 1) eased by
    //    0.4/tick, so it saturates to 1.0 (walk 0.86, sprint & creative-fly 1.0 —
    //    no sprint multiplier hack) and decays to 0 when the player stops. This is
    //    what makes the limbs return to rest instead of freezing at the last angle.
    //  - walkAnimationPosition = the phase, `position += speed` each tick.
    // The camera bob (strideDistance_, capped 0.1) stays separate for view bob.
    [[nodiscard]] float walkAnimationSpeed() const { return walkAnimationSpeed_; }
    [[nodiscard]] float previousWalkAnimationSpeed() const {
        return previousWalkAnimationSpeed_;
    }
    [[nodiscard]] float walkAnimationPosition() const { return walkAnimationPosition_; }
    [[nodiscard]] float previousWalkAnimationPosition() const {
        return previousWalkAnimationPosition_;
    }
    // Whether the player's box overlaps a block placed at (x, y, z) whose
    // collision box spans [y+boxBottom, y+boxTop] vertically. The default is a
    // full cube; a slab passes its half box so the player can place a slab into
    // the empty half of the cell it is standing in.
    [[nodiscard]] bool intersectsBlock(int x, int y, int z, float boxBottom = 0.0F,
                                       float boxTop = 1.0F) const;
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
    // collidesAt, but for a body of an arbitrary height rather than the current
    // pose's — so the pose solver can ask "would a 1.8 body fit here?" without
    // first standing up and risking a frame clipped into the ceiling.
    [[nodiscard]] bool collidesAtHeight(const world::World& world, glm::vec3 position,
                                        float height) const;
    // EntityPose resolution (LivingEntity#updatePlayerPose): the player crouches
    // when it wants to (shift, not flying) OR when a standing body would not fit
    // overhead. Called once per tick before the move, so collision height and the
    // sneak edge-guard use the resolved pose.
    void updatePlayerPose(const world::World& world, bool wantsCrouch);
    // Moves one axis and reports whether the move was stopped by a collision,
    // so moveWithCollisions can retry the horizontal move from a step up.
    [[nodiscard]] bool moveAxis(const world::World& world, int axis, float distance);
    void moveWithCollisions(const world::World& world, glm::vec3 distance);
    // When the body is embedded in solid geometry (a door closed onto the
    // player, a teleport into a wall, being buried), the cells it is *already*
    // inside stop colliding until it has moved clear of them — vanilla's
    // collision ignores shapes the entity already overlaps, so a trapped player
    // can walk (or fall) straight out and only the geometry it newly meets stops
    // it. Without this, moveAxis bisects from the overlapping rest position and
    // freezes every axis, trapping the player so they can only break free. The
    // clip window is the cell range the body occupies, recomputed each move; the
    // floor it merely rests on is below that window, so gravity still holds.
    void updateClipWindow(const world::World& world);
    [[nodiscard]] bool cellIsClipped(int x, int y, int z) const {
        return clipActive_ && x >= clipMin_.x && x <= clipMax_.x && y >= clipMin_.y &&
               y <= clipMax_.y && z >= clipMin_.z && z <= clipMax_.z;
    }
    // LivingEntity#maxUpStep: re-attempt a wall-blocked horizontal move from a
    // step up, keeping the move only if the lifted body clears it and lands.
    // Returns whether the step recovered the move, so the caller can restore the
    // horizontal velocity moveAxis zeroed against the wall.
    [[nodiscard]] bool stepUp(const world::World& world, glm::vec3 distance,
                              glm::vec3 beforeHorizontal);
    // Whether a jump clears the one-block rise the player is currently walking
    // into: the body lifted a full block over the blocked forward cell is open.
    [[nodiscard]] bool autoJumpCanClear(const world::World& world, glm::vec3 forward) const;
    [[nodiscard]] glm::vec3 adjustMovementForSneaking(
        const world::World& world,
        glm::vec3 distance) const;
    void updateFieldOfViewMultiplier();
    [[nodiscard]] float collisionHeight() const {
        return pose_ == Pose::Crouching ? kSneakingHeight : kHeight;
    }

    glm::vec3 position_;
    glm::vec3 velocity_{0.0F};
    bool onGround_ = false;
    bool flying_ = false;
    bool sprinting_ = false;
    bool inWater_ = false;
    Pose pose_ = Pose::Standing;
    bool horizontalCollision_ = false;
    // The cell window the body is currently embedded in (see updateClipWindow):
    // collision ignores these cells so a trapped player can move out of them.
    // Inactive whenever the body is not genuinely embedded.
    bool clipActive_ = false;
    glm::ivec3 clipMin_{0};
    glm::ivec3 clipMax_{0};
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
    // ANIM A1/A2: the vanilla WalkAnimationState amplitude + phase (see accessors).
    float walkAnimationSpeed_ = 0.0F;
    float previousWalkAnimationSpeed_ = 0.0F;
    float walkAnimationPosition_ = 0.0F;
    float previousWalkAnimationPosition_ = 0.0F;
};

} // namespace mc::gameplay

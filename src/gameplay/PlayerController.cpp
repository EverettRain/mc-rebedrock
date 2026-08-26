#include "gameplay/PlayerController.hpp"

#include "world/Block.hpp"
#include "world/BlockShape.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace mc::gameplay {
namespace {

constexpr float kCollisionEpsilon = 0.0001F;
constexpr float kHalfWidth = PlayerController::kWidth * 0.5F;
constexpr float kGravity = 0.08F;
constexpr float kWaterGravity = 0.02F;
constexpr float kJumpVelocity = 0.42F;
constexpr float kSprintJumpImpulse = 0.2F;
constexpr float kGroundSlipperiness = 0.6F;
constexpr float kHorizontalAirDrag = 0.91F;
constexpr float kVerticalAirDrag = 0.98F;
constexpr float kWaterDrag = 0.8F;
constexpr float kWaterAcceleration = 0.02F;
constexpr float kWaterLiftAcceleration = 0.04F;
constexpr float kWaterExitVelocity = 0.18F;
constexpr float kFlightVerticalDrag = 0.6F;
// On ground vanilla accelerates by movementSpeed * 0.216 / slipperiness^3, and
// 0.6^3 is 0.216, so the walking tier's acceleration is the attribute itself.
constexpr float kWalkAcceleration = PlayerController::kWalkSpeed;
constexpr float kAirAcceleration = 0.02F;
constexpr float kFlyAcceleration = PlayerController::kCreativeFlyingSpeed;
constexpr float kSprintGroundMultiplier = PlayerController::kSprintSpeedMultiplier;
constexpr float kSprintFlightMultiplier = 2.0F;
constexpr float kSneakingSpeedMultiplier = 0.3F;
constexpr float kFlightVerticalAcceleration = PlayerController::kCreativeFlyingSpeed * 3.0F;
constexpr float kMaximumCollisionStep = 0.45F;
constexpr float kInputScale = 0.98F;
// Entity#maxUpStep's base value, shared with the creature step-up: the body
// rises by up to a full block to clear an obstacle, then drops back to the
// ground. The feet rest one ground-offset above a cell boundary after landing.
constexpr float kStepHeight = 0.6F;
constexpr float kGroundOffset = 0.001F;

[[nodiscard]] int floorDiv(int value, int divisor) {
    int quotient = value / divisor;
    if (value % divisor < 0) {
        --quotient;
    }
    return quotient;
}

[[nodiscard]] bool columnLoaded(const world::World& world, int x, int z) {
    return world.hasChunk({
        floorDiv(x, world::kChunkWidth),
        floorDiv(z, world::kChunkDepth),
    });
}

// The collision shape of the cell at (x, y, z), reading the block through the
// shared world::collisionShape. Out-of-range and unloaded cells stay a solid
// full cube so the player never falls through a chunk seam; above the world is
// empty air.
[[nodiscard]] world::BlockShape blockCollisionShape(const world::World& world, int x, int y,
                                                    int z) {
    if (y < world::kMinY) {
        return {world::ShapeKind::Column, 0.0F, 1.0F, {}};
    }
    if (y >= world::kMaxY) {
        return {world::ShapeKind::Empty, 0.0F, 0.0F, {}};
    }
    if (!columnLoaded(world, x, z)) {
        return {world::ShapeKind::Column, 0.0F, 1.0F, {}};
    }
    return world::collisionShape(world.state(x, y, z));
}

[[nodiscard]] bool pointInWater(const world::World& world, glm::vec3 point) {
    const int x = static_cast<int>(std::floor(point.x));
    const int y = static_cast<int>(std::floor(point.y));
    const int z = static_cast<int>(std::floor(point.z));
    if (!world::isFluid(world.block(x, y, z))) {
        return false;
    }
    const std::uint8_t level = world.fluidLevel(x, y, z);
    const float height = level >= 8U
        ? 1.0F
        : static_cast<float>(8U - level) / 9.0F;
    return point.y < static_cast<float>(y) + height;
}

[[nodiscard]] bool playerTouchesWater(const world::World& world, glm::vec3 position) {
    constexpr std::array<float, 2> sampleHeights{0.001F, 0.9F};
    for (const float sampleHeight : sampleHeights) {
        if (pointInWater(
                world,
                position + glm::vec3{0.0F, sampleHeight, 0.0F})) {
            return true;
        }
    }
    return false;
}

} // namespace

PlayerController::PlayerController(glm::vec3 feetPosition)
    : position_(feetPosition) {}

void PlayerController::setPosition(glm::vec3 feetPosition) {
    position_ = feetPosition;
    velocity_ = glm::vec3{0.0F};
    onGround_ = false;
    horizontalCollision_ = false;
    fallDistance_ = 0.0F;
    jumpingCooldownTicks_ = 0;
    fovMultiplier_ = previousFovMultiplier_ = 1.0F;
    horizontalSpeed_ = previousHorizontalSpeed_ = 0.0F;
    strideDistance_ = previousStrideDistance_ = 0.0F;
    walkAnimationSpeed_ = previousWalkAnimationSpeed_ = 0.0F;
    walkAnimationPosition_ = previousWalkAnimationPosition_ = 0.0F;
}

void PlayerController::resetForRespawn(glm::vec3 feetPosition) {
    setPosition(feetPosition);
    // The fields setPosition leaves alone are exactly the ones a dying body
    // carries: a sprinting flight player respawns grounded, not flying, not
    // sneaking and not mid-water. The first tick re-derives inWater_ from the
    // block under the feet, so forcing it off here is safe.
    flying_ = false;
    sprinting_ = false;
    pose_ = Pose::Standing;
    inWater_ = false;
    jumpedThisTick_ = false;
    flightToggleWindowTicks_ = 0;
    sprintDoubleTapWindowTicks_ = 0;
}

glm::vec3 PlayerController::eyePosition() const {
    return position_ + glm::vec3{0.0F, eyeHeight(), 0.0F};
}

bool PlayerController::intersectsBlock(int x, int y, int z, float boxBottom,
                                      float boxTop) const {
    if (boxTop <= boxBottom) {
        return false;
    }
    const glm::vec3 playerMin{position_.x - kHalfWidth, position_.y, position_.z - kHalfWidth};
    const glm::vec3 playerMax{
        position_.x + kHalfWidth,
        position_.y + collisionHeight(),
        position_.z + kHalfWidth,
    };
    // The would-be block only occupies [y+boxBottom, y+boxTop] vertically, so a
    // slab's half box lets the player stand in the empty half of the same cell.
    const glm::vec3 blockMin{
        static_cast<float>(x), static_cast<float>(y) + boxBottom, static_cast<float>(z)};
    const glm::vec3 blockMax{
        static_cast<float>(x) + 1.0F, static_cast<float>(y) + boxTop, static_cast<float>(z) + 1.0F};
    return playerMin.x + kCollisionEpsilon < blockMax.x &&
           playerMax.x - kCollisionEpsilon > blockMin.x &&
           playerMin.y + kCollisionEpsilon < blockMax.y &&
           playerMax.y - kCollisionEpsilon > blockMin.y &&
           playerMin.z + kCollisionEpsilon < blockMax.z &&
           playerMax.z - kCollisionEpsilon > blockMin.z;
}

bool PlayerController::collidesAt(const world::World& world, glm::vec3 position) const {
    return collidesAtHeight(world, position, collisionHeight());
}

bool PlayerController::collidesAtHeight(const world::World& world, glm::vec3 position,
                                       float height) const {
    const int minX = static_cast<int>(std::floor(position.x - kHalfWidth + kCollisionEpsilon));
    const int maxX = static_cast<int>(std::floor(position.x + kHalfWidth - kCollisionEpsilon));
    const int minY = static_cast<int>(std::floor(position.y + kCollisionEpsilon));
    const int maxY = static_cast<int>(
        std::floor(position.y + height - kCollisionEpsilon));
    const int minZ = static_cast<int>(std::floor(position.z - kHalfWidth + kCollisionEpsilon));
    const int maxZ = static_cast<int>(std::floor(position.z + kHalfWidth - kCollisionEpsilon));

    const float qMinX = position.x - kHalfWidth;
    const float qMaxX = position.x + kHalfWidth;
    const float qMinZ = position.z - kHalfWidth;
    const float qMaxZ = position.z + kHalfWidth;
    const float qMinY = position.y;
    const float qMaxY = position.y + height;
    for (int y = minY; y <= maxY; ++y) {
        for (int z = minZ; z <= maxZ; ++z) {
            for (int x = minX; x <= maxX; ++x) {
                // A cell inside the clip window is geometry the body is already
                // embedded in; ignore it so a trapped player can move out (the
                // block it rests on sits below the window and still collides).
                if (cellIsClipped(x, y, z)) {
                    continue;
                }
                // A partial block only collides over its own boxes: a slab's half
                // box (Column) lets the player stand in the empty half of the
                // cell, and a fence post or stair step (Boxes) fills only part of
                // the footprint. The cell iteration already established the
                // horizontal overlap for a full-footprint Column, so that path
                // stays a plain vertical-span test; Boxes are tested in 3D.
                if (world::shapeOverlaps(blockCollisionShape(world, x, y, z),
                                         static_cast<float>(x), static_cast<float>(y),
                                         static_cast<float>(z), qMinX, qMinY, qMinZ, qMaxX, qMaxY,
                                         qMaxZ)) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool PlayerController::moveAxis(const world::World& world, int axis, float distance) {
    if (std::abs(distance) <= kCollisionEpsilon) {
        return false;
    }
    const int stepCount = std::max(
        1, static_cast<int>(std::ceil(std::abs(distance) / kMaximumCollisionStep)));
    const float step = distance / static_cast<float>(stepCount);

    for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex) {
        glm::vec3 candidate = position_;
        candidate[axis] += step;
        if (!collidesAt(world, candidate)) {
            position_ = candidate;
            continue;
        }

        float safeFraction = 0.0F;
        float blockedFraction = 1.0F;
        for (int iteration = 0; iteration < 12; ++iteration) {
            const float middle = (safeFraction + blockedFraction) * 0.5F;
            candidate = position_;
            candidate[axis] += step * middle;
            if (collidesAt(world, candidate)) {
                blockedFraction = middle;
            } else {
                safeFraction = middle;
            }
        }
        position_[axis] += step * safeFraction;
        if (axis == 1 && distance < 0.0F) {
            onGround_ = true;
        } else if (axis != 1) {
            horizontalCollision_ = true;
        }
        velocity_[axis] = 0.0F;
        return true;
    }
    return false;
}

void PlayerController::updateClipWindow(const world::World& world) {
    // Decide whether the body is genuinely embedded, and if so which cells to let
    // it pass out of this move. clipActive_ must be false while we probe so the
    // probe sees the real geometry, not an already-open window.
    clipActive_ = false;
    // A thin probe at the body's vertical middle is the discriminator: for a
    // player merely standing on (or sunk a little into) a floor the middle is
    // open air, so a normal spawn or a deep terrain seat never trips this — only
    // a block that fills the body's mid-section (a door/wall closed onto it)
    // does. Vertical-only floor penetration is left to the normal Y settling.
    constexpr float kProbeHalf = 0.05F;
    const float midHeight = collisionHeight() * 0.5F;
    if (!collidesAtHeight(world, position_ + glm::vec3{0.0F, midHeight - kProbeHalf, 0.0F},
                          2.0F * kProbeHalf)) {
        return;
    }
    // Embedded: ignore exactly the cells the body currently occupies for the rest
    // of this move. The Y floor uses floor(position_.y) (the feet cell), so the
    // block the player rests *on* — one cell lower — stays solid and gravity is
    // unaffected; only what the body is inside is clipped.
    clipActive_ = true;
    const float height = collisionHeight();
    // Use the same epsilon insets collidesAtHeight uses to pick cells, so the
    // window covers exactly the cells the body registers overlap in — and, in
    // particular, the +eps on the Y floor keeps the cell the player merely rests
    // on (one below the feet) out of the window even when a sub-pixel dip puts
    // the feet fractionally under a cell boundary. Without it a resting player
    // would clip its own floor and sink through.
    clipMin_ = {static_cast<int>(std::floor(position_.x - kHalfWidth + kCollisionEpsilon)),
                static_cast<int>(std::floor(position_.y + kCollisionEpsilon)),
                static_cast<int>(std::floor(position_.z - kHalfWidth + kCollisionEpsilon))};
    clipMax_ = {static_cast<int>(std::floor(position_.x + kHalfWidth - kCollisionEpsilon)),
                static_cast<int>(std::floor(position_.y + height - kCollisionEpsilon)),
                static_cast<int>(std::floor(position_.z + kHalfWidth - kCollisionEpsilon))};
}

void PlayerController::moveWithCollisions(const world::World& world, glm::vec3 distance) {
    onGround_ = false;
    horizontalCollision_ = false;
    // Open a clip window over any geometry the body is already embedded in so
    // this move can carry it out (see updateClipWindow); a free-standing player
    // leaves it inactive and collides exactly as before.
    updateClipWindow(world);
    // Vanilla resolves Y before the horizontal axes, which keeps landing stable.
    // The vertical result is folded into onGround_, so the return is discarded.
    static_cast<void>(moveAxis(world, 1, distance.y));
    const glm::vec3 beforeHorizontal = position_;
    // The horizontal speed carried into the move: moveAxis zeroes the axis it is
    // stopped on, but a step that then recovers the whole move must not lose that
    // speed, or the player re-accelerates from zero every slab and the run
    // stutters. Vanilla only keeps a component zeroed when the move on that axis
    // was truly cut short.
    const glm::vec3 velocityBeforeHorizontal = velocity_;
    // LivingEntity#adjustMovementForCollisions resolves the dominant horizontal
    // axis first (|z| >= |x| → Z, else X): the box clears a corner on its main
    // direction before the other axis is checked, so a diagonal walk around a
    // block edge slides instead of jamming against the corner.
    bool blockedX = false;
    bool blockedZ = false;
    if (std::abs(distance.z) > std::abs(distance.x)) {
        blockedZ = moveAxis(world, 2, distance.z);
        blockedX = moveAxis(world, 0, distance.x);
    } else {
        blockedX = moveAxis(world, 0, distance.x);
        blockedZ = moveAxis(world, 2, distance.z);
    }
    if ((blockedX || blockedZ) && onGround_ &&
        stepUp(world, distance, beforeHorizontal)) {
        // The step recovered the full horizontal move, so restore the speed the
        // wall zeroed — stepping onto a slab keeps a sprint's momentum instead of
        // hitching to a stop and building back up.
        velocity_.x = velocityBeforeHorizontal.x;
        velocity_.z = velocityBeforeHorizontal.z;
    }
    // The clip window only relaxes collision for the duration of this move; every
    // check after it (ground/pose probes, the next tick) sees solid geometry
    // again, so the body re-solidifies the instant it is no longer embedded.
    clipActive_ = false;
}

bool PlayerController::stepUp(const world::World& world, glm::vec3 distance,
                             glm::vec3 beforeHorizontal) {
    // Entity#maxUpStep: when the horizontal move is stopped by a wall while on
    // the ground, retry it from a step up, keeping the move only if the lifted
    // body clears both the lift and the horizontal move and then drops back to
    // the ground. The player keeps vanilla's 0.6 step — enough for stairs and
    // partial blocks but never a full one-block rise, which needs a jump (the
    // creatures lift the full block, the player does not).
    constexpr float kLiftCandidates[] = {kStepHeight};
    for (const float lift : kLiftCandidates) {
        const glm::vec3 lifted = beforeHorizontal + glm::vec3{0.0F, lift, 0.0F};
        if (collidesAt(world, lifted)) {
            continue;
        }
        glm::vec3 target = lifted;
        target.x += distance.x;
        target.z += distance.z;
        if (collidesAt(world, target)) {
            continue;
        }
        // Settle onto the top of the obstacle: drop to the cell boundary if the
        // landing is open, otherwise keep the lifted feet.
        const float dropped = std::floor(target.y) + kGroundOffset >= beforeHorizontal.y
            ? std::floor(target.y) + kGroundOffset
            : target.y;
        target.y = collidesAt(world, {target.x, dropped, target.z}) ? target.y : dropped;
        position_ = target;
        onGround_ = true;
        // The horizontal move was fully realised by stepping up, so it is not a
        // collision: Entity#move reports horizontalCollision only when the
        // desired movement is cut short. moveAxis set the flag when the flat
        // move was blocked; clear it now that the step recovered the whole
        // distance, matching vanilla — stepping onto a slab or from farmland
        // onto a full block keeps a sprint instead of breaking it.
        horizontalCollision_ = false;
        return true;
    }
    return false;
}

bool PlayerController::autoJumpCanClear(const world::World& world, glm::vec3 forward) const {
    // The obstacle the player walks into is a one-block rise: a body lifted a
    // full block and pushed a body-width into the blocked forward cell is open,
    // so a jump clears it. A two-high wall (or a block overhead) keeps the
    // lifted body blocked and no jump helps.
    const glm::vec3 probe =
        position_ + glm::vec3{forward.x * 0.35F, 1.0F, forward.z * 0.35F};
    return !collidesAt(world, probe);
}

glm::vec3 PlayerController::adjustMovementForSneaking(
    const world::World& world,
    glm::vec3 distance) const {
    if (!sneaking() || !onGround_) {
        return distance;
    }
    constexpr float edgeStep = 0.05F;
    const auto reduceTowardZero = [](float value) {
        if (std::abs(value) <= edgeStep) {
            return 0.0F;
        }
        return value > 0.0F ? value - edgeStep : value + edgeStep;
    };
    // The edge guard drops a probe of the body one maxUpStep (0.6) below the feet
    // and keeps the move only while that lifted-down body still rests on ground —
    // matching LivingEntity#maxUpStep, so a crouching player will not walk off a
    // ledge it could have stepped down. (Was a hardcoded -0.5; kStepHeight ties it
    // to the same step constant the wall step-up uses.)
    const auto supported = [&](float x, float z) {
        return collidesAt(
            world,
            position_ + glm::vec3{x, -kStepHeight, z});
    };
    while (std::abs(distance.x) > kCollisionEpsilon &&
           !supported(distance.x, 0.0F)) {
        distance.x = reduceTowardZero(distance.x);
    }
    while (std::abs(distance.z) > kCollisionEpsilon &&
           !supported(0.0F, distance.z)) {
        distance.z = reduceTowardZero(distance.z);
    }
    while (std::abs(distance.x) > kCollisionEpsilon &&
           std::abs(distance.z) > kCollisionEpsilon &&
           !supported(distance.x, distance.z)) {
        distance.x = reduceTowardZero(distance.x);
        distance.z = reduceTowardZero(distance.z);
    }
    return distance;
}

void PlayerController::updatePlayerPose(const world::World& world, bool wantsCrouch) {
    // LivingEntity#updatePlayerPose: the player crouches when it asks to, and is
    // FORCED to stay crouched when a standing (1.8) body would not fit — releasing
    // shift under a slab/trapdoor keeps the crouch instead of clipping the head
    // into the ceiling. The standing fit is tested at the crouch height's feet, so
    // the query is not itself blocked by the very ceiling it is checking for.
    const bool canStand = !collidesAtHeight(world, position_, kHeight);
    pose_ = (wantsCrouch || !canStand) ? Pose::Crouching : Pose::Standing;
}

void PlayerController::tick(const world::World& world, const PlayerInput& input) {
    previousHorizontalSpeed_ = horizontalSpeed_;
    previousStrideDistance_ = strideDistance_;
    previousWalkAnimationSpeed_ = walkAnimationSpeed_;
    previousWalkAnimationPosition_ = walkAnimationPosition_;
    jumpedThisTick_ = false;
    const glm::vec3 positionBeforeMovement = position_;
    // Vanilla LivingEntity#travel samples the friction once, before moving, and
    // reuses that same value for the acceleration and for the end-of-tick drag.
    // A jump therefore still pays the ground friction on its take-off tick; only
    // the following ticks use air drag. Sampling it after the move instead would
    // let a sprint jump keep (0.28 + 0.13 + 0.2) * 0.91 horizontally.
    const bool groundedBeforeMovement = onGround_;
    // Entity.fallDistance resets as soon as the feet touch the ground; a landing
    // tick therefore keeps the accumulated fall for the trample check to read.
    if (groundedBeforeMovement) {
        fallDistance_ = 0.0F;
    }
    if (!input.flightAllowed) {
        flying_ = false;
        flightToggleWindowTicks_ = 0;
    }
    if (flightToggleWindowTicks_ > 0) {
        --flightToggleWindowTicks_;
    }
    if (sprintDoubleTapWindowTicks_ > 0) {
        --sprintDoubleTapWindowTicks_;
    }
    if (jumpingCooldownTicks_ > 0) {
        --jumpingCooldownTicks_;
    }
    if (input.flightAllowed && input.jumpPressed) {
        if (flightToggleWindowTicks_ > 0) {
            flying_ = !flying_;
            flightToggleWindowTicks_ = 0;
            velocity_.y = 0.0F;
            // A double-tap on the ground must still leave it, or the
            // landing-cancel below kills the fresh flight the same tick.
            // Vanilla's second press both toggles flight and jumps
            // (LivingEntity#tickMovement sees jumping while onGround).
            if (flying_ && onGround_) {
                velocity_.y = kJumpVelocity;
                onGround_ = false;
            }
        } else {
            flightToggleWindowTicks_ = kCreativeFlightToggleWindowTicks;
        }
    }
    const int feetX = static_cast<int>(std::floor(position_.x));
    const int feetZ = static_cast<int>(std::floor(position_.z));
    if (!columnLoaded(world, feetX, feetZ)) {
        velocity_ = glm::vec3{0.0F};
        return;
    }
    // The pose is resolved from the shift input and the headroom, after the
    // column guard so the standing-fit query reads loaded blocks. It drives the
    // collision height and eye height, the sneak edge-guard, and the render
    // snapshot's crouch — all through pose_/sneaking(), never the raw key.
    updatePlayerPose(world, input.sneakHeld && !flying_);
    const bool wasInWater = inWater_;
    inWater_ = playerTouchesWater(world, position_);
    if (wasInWater && !inWater_ && input.jumpHeld && velocity_.y > 0.0F) {
        velocity_.y = std::max(velocity_.y, kWaterExitVelocity);
    }

    glm::vec3 forward{input.lookDirection.x, 0.0F, input.lookDirection.z};
    if (glm::dot(forward, forward) < 0.000001F) {
        forward = glm::vec3{0.0F, 0.0F, -1.0F};
    } else {
        forward = glm::normalize(forward);
    }
    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3{0.0F, 1.0F, 0.0F}));
    glm::vec3 movement = forward * input.forward + right * input.strafe;
    if (glm::dot(movement, movement) > 1.0F) {
        movement = glm::normalize(movement);
    }
    movement *= kInputScale;

    if (sneaking()) {
        movement *= kSneakingSpeedMultiplier;
    }

    bool doubleTapSprint = false;
    if (input.forwardPressed) {
        if (sprintDoubleTapWindowTicks_ > 0) {
            doubleTapSprint = true;
            sprintDoubleTapWindowTicks_ = 0;
        } else {
            sprintDoubleTapWindowTicks_ = 7;
        }
    }
    // ClientPlayerEntity#tickMovement's sprint rules. `hasForwardMovement` is
    // the 0.8 threshold on the raw forward input, which is why sneaking (which
    // scales it to 0.3) cancels a sprint on its own. Running into a wall also
    // cancels it, and holding the sprint key restarts it on the next tick.
    const bool forwardMovement = input.forward > 0.8F;
    if (!forwardMovement || !input.sprintAllowed || horizontalCollision_ || inWater_) {
        sprinting_ = false;
    } else if (input.sprintHeld || doubleTapSprint) {
        sprinting_ = true;
    }
    // Bedrock-style auto-jump: when enabled and walking forward into a one-block
    // rise, the player jumps on its own. The collision flag reads the previous
    // tick's move (the move below sets it), which is the same one-tick lag the
    // jump key would have anyway, and it only fires for obstacles a jump can
    // actually clear — never a two-high wall or a missing headroom.
    const bool wantsAutoJump =
        input.autoJump && onGround_ && horizontalCollision_ && input.forward > 0.0F &&
        !sneaking() && !inWater_ && !flying_ && jumpingCooldownTicks_ == 0 &&
        autoJumpCanClear(world, forward);
    if (flying_) {
        const float acceleration = kFlyAcceleration *
            (sprinting_ ? kSprintFlightMultiplier : 1.0F);
        velocity_ += movement * acceleration;
        if (input.jumpHeld != input.descendHeld) {
            velocity_.y += input.jumpHeld
                ? kFlightVerticalAcceleration
                : -kFlightVerticalAcceleration;
        }
    } else {
        const float acceleration = (inWater_
            ? kWaterAcceleration
            : (groundedBeforeMovement ? kWalkAcceleration : kAirAcceleration)) *
            (sprinting_ ? kSprintGroundMultiplier : 1.0F);
        velocity_ += movement * acceleration;
        if (inWater_ && input.jumpHeld) {
            velocity_.y += kWaterLiftAcceleration;
        } else if (inWater_ && sneaking()) {
            velocity_.y -= kWaterLiftAcceleration;
        } else if ((input.jumpHeld || wantsAutoJump) && onGround_ &&
                   jumpingCooldownTicks_ == 0) {
            // LivingEntity#tickMovement jumps off `this.jumping` — the key being
            // held — and then blocks the next ten ticks. A jump arc lasts longer
            // than that, so holding the key bunny-hops continuously, which is
            // how a vanilla sprint actually covers ground (about 7.1 blocks per
            // second rather than the 5.6 of a flat run).
            velocity_.y = kJumpVelocity;
            if (sprinting_) {
                velocity_ += forward * kSprintJumpImpulse;
            }
            onGround_ = false;
            jumpedThisTick_ = true;
            jumpingCooldownTicks_ = 10;
        } else if (!input.jumpHeld && !wantsAutoJump) {
            jumpingCooldownTicks_ = 0;
        }
    }

    velocity_ = adjustMovementForSneaking(world, velocity_);
    moveWithCollisions(world, velocity_);
    // ClientPlayerEntity#tickMovement: landing cancels an active flight. The
    // double-tap that starts one jumps off the ground the same tick, so a
    // freshly toggled flight survives; only a flight that actually touches
    // down is switched off.
    if (onGround_ && flying_) {
        flying_ = false;
    }

    const glm::vec2 horizontalDelta{position_.x - positionBeforeMovement.x,
                                    position_.z - positionBeforeMovement.z};
    const float horizontalDistance = glm::length(horizontalDelta);
    horizontalSpeed_ += horizontalDistance * 0.6F;
    const float targetStride = onGround_ ? std::min(0.1F, horizontalDistance) : 0.0F;
    strideDistance_ += (targetStride - strideDistance_) * 0.4F;
    // ANIM A1/A2: the gait amplitude is the vanilla WalkAnimationState — driven by
    // ACTUAL horizontal displacement (not the "W key"), target = min(4d, 1) eased
    // by 0.4/tick. Walking (~0.216 blk/tick) saturates near 0.86; sprint and
    // creative flight (>=0.25) both hit 1.0, so no sprint multiplier is needed.
    // Stopping decays it to 0 over ~5 ticks, which is what returns the limbs to
    // rest. The phase advances by the amplitude, exactly like `position += speed`.
    const float walkTarget = std::min(4.0F * horizontalDistance, 1.0F);
    walkAnimationSpeed_ += (walkTarget - walkAnimationSpeed_) * 0.4F;
    walkAnimationPosition_ += walkAnimationSpeed_;

    if (!flying_) {
        if (!onGround_ && velocity_.y <= 0.0F &&
            collidesAt(world, position_ - glm::vec3{0.0F, 0.001F, 0.0F})) {
            onGround_ = true;
            velocity_.y = 0.0F;
        }
        // Vanilla moves using the current velocity first, then applies gravity.
        // Applying 0.08 before movement cuts the first jump step from 0.42 to 0.34.
        velocity_.y -= inWater_ ? kWaterGravity : kGravity;
    }

    const float horizontalDrag = inWater_
        ? kWaterDrag
        : groundedBeforeMovement
        ? kHorizontalAirDrag * kGroundSlipperiness
        : kHorizontalAirDrag;
    velocity_.x *= horizontalDrag;
    velocity_.z *= horizontalDrag;
    velocity_.y *= flying_ ? kFlightVerticalDrag
                           : (inWater_ ? kWaterDrag : kVerticalAirDrag);

    // Entity.baseTick: the fall distance grows by the downward motion while
    // airborne, so a landing carries the whole fall for the farmland trample.
    if (!onGround_ && !flying_ && velocity_.y < 0.0F) {
        fallDistance_ -= velocity_.y;
    }

    updateFieldOfViewMultiplier();
}

// AbstractClientPlayerEntity#updateMovementFovMultiplier. The FOV follows the
// movement-speed attribute rather than the measured velocity, so it rises the
// moment a sprint starts and eases back over a few ticks when it ends. Without
// it a vanilla-speed sprint reads as a walk, because the widening view is most
// of what conveys the speed.
void PlayerController::updateFieldOfViewMultiplier() {
    const float movementSpeed =
        kWalkSpeed * (sprinting_ ? kSprintSpeedMultiplier : 1.0F);
    float target = flying_ ? 1.1F : 1.0F;
    target *= (movementSpeed / kWalkSpeed + 1.0F) * 0.5F;
    previousFovMultiplier_ = fovMultiplier_;
    fovMultiplier_ += (target - fovMultiplier_) * 0.5F;
}

} // namespace mc::gameplay

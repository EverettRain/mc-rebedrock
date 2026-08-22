#pragma once

// The frame's player pose, extracted once per frame from the tick-owned action
// timeline and the player's physics endpoints, interpolated with the frame's
// partial tick. This is the animation §13.1 rule made concrete: swing/use and
// the walk stride advance on the server tick, and the intra-frame pose is
// previous/current + partialTicks — so the arm no longer snaps at 20 TPS and
// the same tick produces the same pose at any frame rate.
//
// A plain value object: no references into gameplay, safe to copy across the
// thread boundary, and identical for every consumer (first person, the world
// player, the inventory preview).

#include "gameplay/Inventory.hpp"
#include "gameplay/PlayerActionState.hpp"
#include "gameplay/PlayerController.hpp"
#include "gameplay/PlayerTickSnapshot.hpp"
#include "render/player/ArmPose.hpp"

#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace mc::render::player {

// A single arm swing, interpolated between its tick-quantized endpoints.
struct InterpolatedSwing final {
    bool active = false;
    gameplay::SwingAnimation animation = gameplay::SwingAnimation::Break;
    std::uint64_t sequence = 0U;
    float progress = 0.0F;  // in [0, 1], smooth between ticks
};

// The ongoing item use, interpolated between its tick endpoints.
struct InterpolatedUse final {
    bool active = false;
    gameplay::UseAnimation animation = gameplay::UseAnimation::None;
    float progress = 0.0F;  // elapsed fraction of the use, in [0, 1]
};

// Everything the renderer draws a player from. `feetPosition`, `walkStride`
// (the accumulated phase), and `walkSpeed` (the eased locomotion amplitude) are
// interpolated between tick endpoints.
struct PlayerRenderState final {
    glm::vec3 feetPosition{0.0F};
    float walkStride = 0.0F;
    float walkSpeed = 0.0F;

    // Rotation (degrees), interpolated from the tick endpoints. bodyYaw is the
    // torso facing applied to the world root; headYaw is RELATIVE to the body
    // (the head bone turns this much on top of the body); pitch is the head/eye
    // pitch. Both solvers (third-person body, first-person hand) read these; the
    // camera perspective never enters here (animation §5.2 / §19.4).
    float bodyYawDegrees = 0.0F;
    float headYawDegrees = 0.0F;  // relative to the body
    float pitchDegrees = 0.0F;

    bool sneaking = false;
    bool flying = false;
    bool sprinting = false;

    InterpolatedSwing swing;
    InterpolatedUse use;
    // The held stack's block/item, for the ArmPose and the item render.
    gameplay::ItemStack heldStack{};

    // The arm poses, derived once here so every consumer agrees (Phase 4). The
    // main hand renders on the main arm (right by default); the off arm is Empty
    // until an off-hand slot exists.
    ArmPose rightArmPose = ArmPose::Empty;
    ArmPose leftArmPose = ArmPose::Empty;
};

// Shortest-path angle lerp for degrees, so 179 -> -179 crosses the +180 seam by
// 2 degrees instead of sweeping 358 the wrong way (animation §13.3). Pure.
[[nodiscard]] inline float wrapDegrees(float degrees) {
    float wrapped = std::fmod(degrees + 180.0F, 360.0F);
    if (wrapped < 0.0F) {
        wrapped += 360.0F;
    }
    return wrapped - 180.0F;
}

[[nodiscard]] inline float lerpAngleDegrees(float from, float to, float alpha) {
    return from + wrapDegrees(to - from) * alpha;
}

// Interpolates the tick-owned swing against `partialTicks` in [0, 1). The
// action's previous/current endpoints were captured by the simulation's tick;
// the frame sits between them. `lastSequence` is the renderer's own memory of
// the swing it sampled last frame: a sequence change means a NEW action (a
// restart), and the progress must snap to the new swing's start rather than
// lerp across the boundary — lerping from the previous apex back to 0 reads as
// a visible replay of the arm.
[[nodiscard]] inline InterpolatedSwing interpolateSwing(const gameplay::SwingState& state,
                                                        float partialTicks,
                                                        std::optional<std::uint64_t>& lastSequence) {
    InterpolatedSwing result;
    result.animation = state.animation;
    result.sequence = state.sequence;
    if (!state.active) {
        // The action ended: the finished swing rests at the completion
        // endpoint (progress 1.0 is the clip's rest pose, matching the None
        // pose the renderer falls back to next frame).
        result.active = false;
        result.progress = 1.0F;
        lastSequence.reset();
        return result;
    }
    result.active = true;
    if (!lastSequence.has_value() || *lastSequence != state.sequence) {
        result.progress = state.progress;  // a new swing starts at its own value
    } else {
        result.progress =
            state.previousProgress + (state.progress - state.previousProgress) * partialTicks;
    }
    lastSequence = state.sequence;
    return result;
}

[[nodiscard]] inline InterpolatedUse interpolateUse(const gameplay::ItemUseState& state,
                                                    float partialTicks) {
    InterpolatedUse result;
    result.active = state.active;
    result.animation = state.animation;
    if (!state.active) {
        result.progress = 1.0F;
        return result;
    }
    // previousRemainingTicks is the count before this tick's decrement, so the
    // elapsed fraction rises smoothly across the tick boundary.
    const float previousElapsed = state.durationTicks > 0U
                                      ? 1.0F - static_cast<float>(state.previousRemainingTicks) /
                                                    static_cast<float>(state.durationTicks)
                                      : 0.0F;
    const float currentElapsed = state.durationTicks > 0U
                                     ? 1.0F - static_cast<float>(state.remainingTicks) /
                                                   static_cast<float>(state.durationTicks)
                                     : 0.0F;
    result.progress = previousElapsed + (currentElapsed - previousElapsed) * partialTicks;
    return result;
}

// A full frame's player state from the per-tick snapshot, interpolated with the
// frame's partial tick. `lastSwingSequence` is the renderer's per-frame memory
// of the swing it last sampled, so a restart snaps instead of replaying.
[[nodiscard]] inline PlayerRenderState extractPlayerRenderState(
    const gameplay::PlayerTickSnapshot& snapshot, float partialTicks,
    std::optional<std::uint64_t>& lastSwingSequence) {
    PlayerRenderState state;
    state.feetPosition = snapshot.physicsPrevious +
                         (snapshot.physicsCurrent - snapshot.physicsPrevious) * partialTicks;
    // PlayerController's historical horizontalSpeed is accumulated horizontal
    // travel scaled by 0.6, whereas HumanoidModel's walk position advances by
    // roughly 4 times horizontal travel. Convert the units here before applying
    // the vanilla 0.6662 cadence in the solver; feeding the raw distance phase
    // makes the limbs visibly slide through the world at about one seventh speed.
    constexpr float kControllerTravelScale = 0.6F;
    constexpr float kWalkPositionPerBlock = 4.0F;
    const float distancePhase =
        snapshot.previousSpeed + (snapshot.speed - snapshot.previousSpeed) * partialTicks;
    state.walkStride = distancePhase * (kWalkPositionPerBlock / kControllerTravelScale);
    const float strideAmount =
        snapshot.previousStride + (snapshot.stride - snapshot.previousStride) * partialTicks;
    // Keep PlayerController's 0.1 view-bob cap untouched (the first-person path
    // reads it directly), but normalize the world-model gait to a useful 0..1.
    float locomotionAmount = strideAmount * 8.0F;
    if (snapshot.flying) {
        // The controller intentionally drives the view-bob stride toward zero
        // off ground. Third-person flight still needs a gait, so derive its
        // amount from this tick's horizontal travel. The accumulated distance
        // phase above already continues in flight and keeps the cycle aligned.
        const float deltaX = snapshot.physicsCurrent.x - snapshot.physicsPrevious.x;
        const float deltaZ = snapshot.physicsCurrent.z - snapshot.physicsPrevious.z;
        const float flyingDistance = std::sqrt(deltaX * deltaX + deltaZ * deltaZ);
        locomotionAmount = std::max(locomotionAmount, flyingDistance * 4.0F);
    }
    state.walkSpeed = std::clamp(locomotionAmount, 0.0F, 1.0F);
    state.sneaking = snapshot.sneaking;
    state.flying = snapshot.flying;
    state.sprinting = snapshot.sprinting;

    // Rotation: wrapped angle lerp so the seam at +/-180 never sweeps the long
    // way. The head yaw stored in the snapshot is absolute; the render state
    // wants it relative to the body, so the body yaw is subtracted after both are
    // interpolated (each on its own shortest path).
    const float bodyYaw =
        lerpAngleDegrees(snapshot.previousBodyYawDegrees, snapshot.bodyYawDegrees, partialTicks);
    const float headYaw =
        lerpAngleDegrees(snapshot.previousHeadYawDegrees, snapshot.headYawDegrees, partialTicks);
    state.bodyYawDegrees = bodyYaw;
    state.headYawDegrees = wrapDegrees(headYaw - bodyYaw);
    state.pitchDegrees =
        snapshot.previousPitchDegrees +
        (snapshot.pitchDegrees - snapshot.previousPitchDegrees) * partialTicks;

    state.swing = interpolateSwing(snapshot.swing, partialTicks, lastSwingSequence);
    state.use = interpolateUse(snapshot.use, partialTicks);
    state.heldStack = snapshot.heldStack;

    // Arm poses derived once (Phase 4). The main hand is the right arm by
    // default; the use hand is the main hand today. The off arm has no slot yet.
    const bool usingMain = state.use.active && snapshot.use.hand == gameplay::InteractionHand::Main;
    state.rightArmPose = deriveArmPose(state.heldStack, usingMain, state.use.animation);
    state.leftArmPose = ArmPose::Empty;
    return state;
}

} // namespace mc::render::player

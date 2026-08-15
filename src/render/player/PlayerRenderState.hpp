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

#include <glm/vec3.hpp>

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

// Everything the renderer draws a player from. `feetPosition` and `walkStride`
// are interpolated between the physics endpoints; the look angles are passed in
// (the camera is the look source until Phase 3 moves it into player state).
struct PlayerRenderState final {
    glm::vec3 feetPosition{0.0F};
    float walkStride = 0.0F;
    float walkSpeed = 0.0F;
    bool sneaking = false;
    bool flying = false;
    bool sprinting = false;

    InterpolatedSwing swing;
    InterpolatedUse use;
    // The held stack's block/item, for the ArmPose and the item render.
    gameplay::ItemStack heldStack{};
};

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
    state.walkStride =
        snapshot.previousStride + (snapshot.stride - snapshot.previousStride) * partialTicks;
    state.walkSpeed = snapshot.previousSpeed + (snapshot.speed - snapshot.previousSpeed) * partialTicks;
    state.sneaking = snapshot.sneaking;
    state.flying = snapshot.flying;
    state.sprinting = snapshot.sprinting;
    state.swing = interpolateSwing(snapshot.swing, partialTicks, lastSwingSequence);
    state.use = interpolateUse(snapshot.use, partialTicks);
    state.heldStack = snapshot.heldStack;
    return state;
}

} // namespace mc::render::player

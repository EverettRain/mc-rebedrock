#pragma once

// The first-person hand pose solver (analysis §9). The first person keeps its
// own camera-space matrix construction (firstPersonArm/Item/EatTransform) — it
// must NOT reuse the third-person world bones (§9.1 / §19.2). What PX-2 removes
// is the independent action clock: the renderer used to hand-decide whether the
// hand is breaking/using/eating and how far along. This solver derives that
// (action + progress) purely from the same unified PlayerRenderState the
// third-person solver reads, so both views show the same attack/use progress
// and F5 never restarts anything (§20).
//
// Pure: no trigger(), no update(deltaSeconds). Given a render state it returns
// the ModelAction and the clip progress in [0, 1]; the renderer samples its
// ModelAnimationSystem with these and builds the camera-space transform as
// before. Vulkan-free and headless-testable.

#include "animation/ModelAnimationSystem.hpp"
#include "render/player/PlayerRenderState.hpp"

namespace mc::animation {

// The first-person action to play this frame and how far along its clip is. A
// swing (attack/mine/place) maps to Break or Use; an active eat/drink to Eat;
// otherwise None (the rest hand).
struct FirstPersonHandInput final {
    ModelAction action = ModelAction::None;
    float progress = 0.0F;  // clip progress in [0, 1]
};

// Derives the first-person hand input from the unified render state. Use takes
// precedence over a swing (eating while the arm would otherwise idle-swing),
// mirroring the vanilla priority where the use pose owns the arm. The swing arc
// distinction (Break vs Use) comes straight from the tick-owned SwingAnimation,
// so a mining swing and a placement swing keep their different first-person arcs
// without the renderer re-deciding.
[[nodiscard]] inline FirstPersonHandInput
solveFirstPersonHand(const render::player::PlayerRenderState& state) {
    FirstPersonHandInput input;
    if (state.use.active) {
        switch (state.use.animation) {
            case gameplay::UseAnimation::Eat:
            case gameplay::UseAnimation::Drink:
                input.action = ModelAction::Eat;
                input.progress = state.use.progress;
                return input;
            default:
                // Other uses (block/bow/...) fall back to the generic Use pose
                // until their content-specific first-person clips land (Phase 6).
                input.action = ModelAction::Use;
                input.progress = state.use.progress;
                return input;
        }
    }
    if (state.swing.active) {
        input.action = state.swing.animation == gameplay::SwingAnimation::Use ? ModelAction::Use
                                                                              : ModelAction::Break;
        input.progress = state.swing.progress;
        return input;
    }
    return input;  // None, progress 0
}

}  // namespace mc::animation

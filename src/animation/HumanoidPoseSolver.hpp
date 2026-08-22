#pragma once

// The deterministic third-person humanoid pose solver (analysis §8). Given an
// already-extracted PlayerRenderState and the player skeleton, it produces the
// frame's SkeletonPose (per-bone rotation deltas relative to rest) plus the two
// hand sockets, following 26.1's HumanoidModel.setupAnim ordering:
//
//   rest -> look -> locomotion -> arm poses -> attack -> crouch -> idle bob
//
// It is a PURE FUNCTION: it starts from rest every call, reads no previous-frame
// bone matrices, and accumulates no elapsed time (the cosmetic idle bob is a
// function of an explicit ageInTicks argument, never a stored clock). The same
// PlayerRenderState therefore yields the same pose at any frame rate and in any
// F5 mode — the world player and the inventory preview both call this and get
// identical local bone rotations (analysis §20).
//
// Vulkan-free: it only touches SkeletalModel/SkeletonPose (animation) and the
// PlayerRenderState value object, so it is headless-testable.

#include "animation/Animator.hpp"
#include "animation/SkeletalModel.hpp"
#include "render/player/PlayerRenderState.hpp"

#include <glm/mat4x4.hpp>

namespace mc::animation {

// The bones the solver drives, resolved once against a model so solve() does no
// string lookups. -1 for any bone the geometry lacks (the solver skips it).
struct HumanoidBoneBindings final {
    int body = -1;
    int head = -1;
    int rightArm = -1;
    int leftArm = -1;
    int rightLeg = -1;
    int leftLeg = -1;

    [[nodiscard]] static HumanoidBoneBindings bind(const SkeletalModel& model);
};

// The solved frame: the skeleton deltas plus the two hand sockets (model-space
// matrices where a held item attaches — analysis §10.2). The sockets follow the
// animated arms so the item-in-hand layer needs no second pose solve.
struct PlayerPoseFrame final {
    SkeletonPose skeleton;
    glm::mat4 rightHandSocket{1.0F};
    glm::mat4 leftHandSocket{1.0F};
};

// Solves the third-person humanoid pose from the render state. `ageInTicks`
// drives only the cosmetic idle bob (a sine of age); pass the same value for a
// given tick+alpha to keep the result deterministic. `model` must outlive the
// returned frame (the SkeletonPose holds a pointer to it, as SkeletonPose does).
[[nodiscard]] PlayerPoseFrame solveHumanoidPose(const SkeletalModel& model,
                                                const HumanoidBoneBindings& bones,
                                                const render::player::PlayerRenderState& state,
                                                float ageInTicks);

}  // namespace mc::animation

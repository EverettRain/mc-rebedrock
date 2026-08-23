#pragma once

// The player's action and physics state, published once per simulation tick
// under the world write lock, so the render thread reads a single coherent
// snapshot instead of live gameplay objects the tick may be mid-mutation on.
// The renderer interpolates between the previous/current endpoints with its own
// per-frame alpha (animation §13.1: intra-frame pose = previous/current +
// partialTicks).
//
// Gameplay owns the type (the simulation publishes it); render/ consumes it.

#include "gameplay/GameMode.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/PlayerActionState.hpp"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>

namespace mc::gameplay {

struct PlayerTickSnapshot final {
    // The resident bytes this snapshot holds. All fields are inline POD (no
    // heap), so the struct size is the whole cost. The N-Mem budget gate pins
    // a per-tick ceiling on it.
    [[nodiscard]] std::size_t residentBytes() const { return sizeof(*this); }

    [[nodiscard]] friend bool operator==(const PlayerTickSnapshot&, const PlayerTickSnapshot&) =
        default;

    // The world tick this snapshot was taken at. The renderer uses it to know
    // when the snapshot advanced (a new tick between frames).
    std::uint64_t serverTick = 0U;

    // The swing/use timelines, with their previous/current progress endpoints.
    SwingState swing;
    ItemUseState use;

    // Physics interpolation endpoints (feet position).
    glm::vec3 physicsPrevious{0.0F};
    glm::vec3 physicsCurrent{0.0F};

    // Walk animation endpoints. The inherited controller names are historical:
    // `speed` is accumulated horizontal travel (the view-bob phase, scaled by
    // 0.6) and `stride` is the eased movement amplitude (which returns to zero
    // after stopping). PlayerRenderState converts both into HumanoidModel units.
    float previousStride = 0.0F;
    float stride = 0.0F;
    float previousSpeed = 0.0F;
    float speed = 0.0F;
    // ANIM A1/A2: the vanilla WalkAnimationState — the gait amplitude
    // (`walkAmount`, min(4d,1) eased, saturates to 1.0, decays to 0 on stop) and
    // the phase (`walkPosition`, += amplitude). These are the ANIM drive
    // quantities the third-person clips read (walk_amount / walk_position),
    // distinct from the camera-bob `stride`. The renderer interpolates each pair.
    float previousWalkAmount = 0.0F;
    float walkAmount = 0.0F;
    float previousWalkPosition = 0.0F;
    float walkPosition = 0.0F;

    // Rotation endpoints (degrees), owned by the tick rather than derived in the
    // renderer from the camera each frame (animation §7.1: worldBodyYaw must not
    // be a frame-driven renderer float). bodyYaw is the torso facing; headYaw is
    // absolute here (the extractor makes it relative to the body); pitch is the
    // head/eye pitch. The renderer wrapped-lerps them by partialTicks, so the
    // same tick yields the same facing at any frame rate and F5 does not touch
    // them. The simulation advances body yaw toward the look each tick with the
    // vanilla head/body clamp.
    float previousBodyYawDegrees = 0.0F;
    float bodyYawDegrees = 0.0F;
    float previousHeadYawDegrees = 0.0F;
    float headYawDegrees = 0.0F;
    float previousPitchDegrees = 0.0F;
    float pitchDegrees = 0.0F;

    bool sneaking = false;
    bool flying = false;
    bool sprinting = false;
    bool inWater = false;
    bool onGround = false;

    // The movement FOV multiplier endpoints (the camera's sprinting/flight
    // widening), interpolated by the renderer.
    float previousFieldOfViewMultiplier = 1.0F;
    float fieldOfViewMultiplier = 1.0F;

    // The selected inventory stack, for the held-item render and the arm pose.
    ItemStack heldStack{};

    // The cell the player is mining and when it started, so the crack overlay
    // can interpolate progress. Published from PlayerInteraction each tick — the
    // renderer must not read the live dig state on its own thread.
    struct MiningDigState final {
        bool active = false;
        glm::ivec3 target{};
        std::uint64_t startedTick = 0U;
        [[nodiscard]] friend bool operator==(const MiningDigState&, const MiningDigState&) =
            default;
    };
    MiningDigState digging;

    // The HUD-read vitals and mode, so the HUD snapshot is built from the tick
    // snapshot too instead of reaching into live gameplay objects.
    float health = 0.0F;
    int foodLevel = 0;
    int airTicks = 0;
    int ticksSinceDamage = 1000;
    GameMode gameMode = GameMode::Survival;
    bool eating = false;
    std::size_t selectedHotbarSlot = 0U;
    // XP-0: the two HUD-relevant experience fields (the bar's fill fraction
    // and the level number drawn above it). totalExperience/enchantmentSeed
    // are not HUD-visible, so — like the container slots this snapshot
    // deliberately omits — they do not ride the per-tick wire.
    int experienceLevel = 0;
    float experienceProgress = 0.0F;
};

} // namespace mc::gameplay

#pragma once

#include "animation/AnimationClip.hpp"
#include "animation/BoneMask.hpp"
#include "animation/Molang.hpp"
#include "animation/SkeletalModel.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace mc::animation {

// How a layer combines with the pose accumulated by earlier layers.
//
//  * Additive — the layer's sampled value is scaled by its weight and *added* to
//    the bone (rotations/positions sum; scale composes multiplicatively toward
//    1). This is how Bedrock stacks a walk cycle, an idle sway and a look, and
//    is the original (pre-ANIM-2) behaviour.
//  * Override — the bone is *lerped toward* the layer's value by its weight:
//    `target = lerp(target, value, weight)`. weight 1 replaces the bone
//    outright (an item-hold pose that must not read the walk's arm swing);
//    weight 0.5 blends halfway; weight 0 leaves it unchanged. Combined with an
//    ANIM-1 mask this cleanly replaces just the masked bones.
enum class BlendMode : std::uint8_t { Additive, Override };

// A weight ramp between two values over a fixed duration — the reusable
// crossfade primitive that replaces the hand-written `approach()` eases
// scattered across the animators. It carries no clip; callers advance it each
// frame and feed `value()` as a layer weight (or any blend factor).
//
// Deterministic and side-effect free: purely a function of elapsed time, so the
// same inputs always give the same weight. The optional ease smooths the ramp
// (smoothstep) without changing its endpoints or duration.
class Transition final {
  public:
    Transition() = default;
    Transition(float from, float to, float durationSeconds, bool eased = false)
        : from_(from), to_(to),
          duration_(durationSeconds > 0.0F ? durationSeconds : 0.0F), eased_(eased) {}

    // Advances the ramp by `deltaSeconds` (clamped at 0) and returns the new
    // value. A zero/negative duration snaps straight to `to`.
    float advance(float deltaSeconds);

    // Retarget the ramp toward a new destination from wherever it is now,
    // restarting the clock. Used when a state flips mid-transition.
    void retarget(float to, float durationSeconds, bool eased = false);

    [[nodiscard]] float value() const;
    [[nodiscard]] bool finished() const { return elapsed_ >= duration_; }
    [[nodiscard]] float target() const { return to_; }

  private:
    float from_ = 0.0F;
    float to_ = 0.0F;
    float duration_ = 0.0F;
    float elapsed_ = 0.0F;
    bool eased_ = false;
};

// The animated transform of a single bone, relative to its rest pose.
struct BonePose final {
    glm::vec3 rotation{0.0F}; // additional rotation in degrees
    glm::vec3 position{0.0F}; // model-unit offset
    glm::vec3 scale{1.0F};    // multiplier around the pivot
};

// The evaluated pose of a whole skeleton for one frame. Holds the per-bone
// animation deltas and can resolve them into hierarchical model-space matrices
// the renderer applies to each cube.
//
// Rotation convention matches Minecraft's bone stack: rotations are applied in
// Z, then Y, then X order about the bone pivot, angles in degrees. Model space
// is Bedrock's: 16 units per block, Y up.
class SkeletonPose final {
  public:
    SkeletonPose() = default;
    explicit SkeletonPose(const SkeletalModel& model);

    [[nodiscard]] const SkeletalModel* model() const { return model_; }
    [[nodiscard]] std::size_t boneCount() const { return bones_.size(); }
    [[nodiscard]] BonePose& bone(std::size_t index) { return bones_[index]; }
    [[nodiscard]] const BonePose& bone(std::size_t index) const { return bones_[index]; }

    // Local (parent-relative) transform of a bone, including its rest rotation.
    [[nodiscard]] glm::mat4 localMatrix(int boneIndex) const;

    // Model-space transform of a bone with the full parent chain applied.
    [[nodiscard]] glm::mat4 worldMatrix(int boneIndex) const;

    // All world matrices, indexed by bone. Convenient for a whole-model draw.
    [[nodiscard]] std::vector<glm::mat4> worldMatrices() const;

  private:
    const SkeletalModel* model_ = nullptr;
    std::vector<BonePose> bones_;
};

// Blends any number of weighted animation layers over a skeleton and exposes a
// Molang context for the game to feed per-frame inputs (anim_time, speeds, …).
// This single runtime drives blocks, the player, NPCs and mobs identically.
class Animator final {
  public:
    void setModel(const SkeletalModel* model);
    [[nodiscard]] const SkeletalModel* model() const { return model_; }

    [[nodiscard]] MolangContext& context() { return context_; }
    [[nodiscard]] const MolangContext& context() const { return context_; }

    // Clears all queued layers for the next frame (keeps the model/context).
    void clearLayers();

    // Queues a clip to blend this frame. `localTime` is the already-wrapped clip
    // time (see AnimationClip::localTime). `weight` scales its contribution;
    // additive layers stack, so a walk clip at weight 1 plus a look override at
    // weight 1 compose the way Bedrock layered animations do.
    //
    // `mask`, when non-null, restricts this layer to the bones it selects: bones
    // outside the mask are left untouched by this layer (Bedrock avatar-mask /
    // upper-vs-lower-body separation). The pointer must outlive `evaluate()`; a
    // null mask keeps the whole-skeleton path byte-for-byte as before.
    //
    // `mode` chooses additive stacking (default; the pre-ANIM-2 behaviour) or an
    // override that lerps the bone toward the layer value by its weight. Evaluate
    // applies all additive layers first, then the override layers in the order
    // they were added, matching Bedrock's layered-animation composition.
    void addLayer(const AnimationClip& clip, float localTime, float weight = 1.0F,
                  const BoneMask* mask = nullptr, BlendMode mode = BlendMode::Additive);

    // Convenience for the common single-clip case: sets anim_time and adds it.
    void playSingle(const AnimationClip& clip, float elapsedSeconds, float weight = 1.0F);

    // Evaluates all queued layers into a pose over the bound model.
    [[nodiscard]] SkeletonPose evaluate() const;

  private:
    struct Layer final {
        const AnimationClip* clip = nullptr;
        float localTime = 0.0F;
        float weight = 1.0F;
        const BoneMask* mask = nullptr;             // null = whole skeleton
        BlendMode mode = BlendMode::Additive;
    };

    const SkeletalModel* model_ = nullptr;
    MolangContext context_;
    std::vector<Layer> layers_;
};

} // namespace mc::animation

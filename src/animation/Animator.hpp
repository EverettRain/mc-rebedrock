#pragma once

#include "animation/AnimationClip.hpp"
#include "animation/Molang.hpp"
#include "animation/SkeletalModel.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <vector>

namespace mc::animation {

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
    void addLayer(const AnimationClip& clip, float localTime, float weight = 1.0F);

    // Convenience for the common single-clip case: sets anim_time and adds it.
    void playSingle(const AnimationClip& clip, float elapsedSeconds, float weight = 1.0F);

    // Evaluates all queued layers into a pose over the bound model.
    [[nodiscard]] SkeletonPose evaluate() const;

  private:
    struct Layer final {
        const AnimationClip* clip = nullptr;
        float localTime = 0.0F;
        float weight = 1.0F;
    };

    const SkeletalModel* model_ = nullptr;
    MolangContext context_;
    std::vector<Layer> layers_;
};

} // namespace mc::animation

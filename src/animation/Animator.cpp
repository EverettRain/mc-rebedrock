#include "animation/Animator.hpp"

#include <glm/ext/matrix_transform.hpp>

#include <cmath>

namespace mc::animation {

SkeletonPose::SkeletonPose(const SkeletalModel& model)
    : model_(&model), bones_(model.boneCount()) {}

glm::mat4 SkeletonPose::localMatrix(int boneIndex) const {
    const ModelBone& bone = model_->bones()[static_cast<std::size_t>(boneIndex)];
    const BonePose& pose = bones_[static_cast<std::size_t>(boneIndex)];

    // Pivot plus the animated positional offset defines where the bone rotates.
    const glm::vec3 pivot = bone.pivot;
    glm::mat4 matrix{1.0F};
    matrix = glm::translate(matrix, pivot + pose.position);
    matrix = matrix * rotationMatrix(bone.rotation + pose.rotation);
    matrix = glm::scale(matrix, pose.scale);
    matrix = glm::translate(matrix, -pivot);
    return matrix;
}

glm::mat4 SkeletonPose::worldMatrix(int boneIndex) const {
    const int parent = model_->bones()[static_cast<std::size_t>(boneIndex)].parent;
    const glm::mat4 local = localMatrix(boneIndex);
    if (parent < 0) {
        return local;
    }
    return worldMatrix(parent) * local;
}

std::vector<glm::mat4> SkeletonPose::worldMatrices() const {
    std::vector<glm::mat4> matrices(bones_.size());
    for (std::size_t i = 0U; i < bones_.size(); ++i) {
        matrices[i] = worldMatrix(static_cast<int>(i));
    }
    return matrices;
}

void Animator::setModel(const SkeletalModel* model) {
    model_ = model;
    layers_.clear();
}

void Animator::clearLayers() { layers_.clear(); }

void Animator::addLayer(const AnimationClip& clip, float localTime, float weight,
                        const BoneMask* mask, BlendMode mode) {
    if (weight <= 0.0F) {
        return;
    }
    layers_.push_back({&clip, localTime, weight, mask, mode});
}

void Animator::playSingle(const AnimationClip& clip, float elapsedSeconds, float weight) {
    clearLayers();
    const float local = clip.localTime(elapsedSeconds);
    context_.setQuery("anim_time", local);
    context_.setQuery("life_time", elapsedSeconds);
    addLayer(clip, local, weight);
}

SkeletonPose Animator::evaluate() const {
    SkeletonPose pose;
    if (model_ == nullptr) {
        return pose;
    }
    pose = SkeletonPose{*model_};

    // Copy the context once, then overwrite anim_time per layer in place. The
    // previous per-layer copy cloned both unordered_maps every layer and
    // dominated the evaluation cost.
    MolangContext frameContext = context_;

    // Applies one layer's channels onto the pose, respecting its mask and mode.
    const auto applyLayer = [&](const Layer& layer) {
        // Expose this layer's clip time so channel expressions can reference it.
        frameContext.setQuery("anim_time", layer.localTime);
        const float w = layer.weight;

        for (const auto& [boneName, boneAnimation] : layer.clip->bones()) {
            const int index = model_->findBone(boneName);
            if (index < 0) {
                continue; // clip targets a bone this geometry does not have
            }
            // A masked layer only writes the bones it selects; bones outside the
            // mask keep whatever earlier layers left. A null mask writes every
            // bone (the pre-mask path), so the whole-skeleton case is unchanged.
            if (layer.mask != nullptr && !layer.mask->test(index)) {
                continue;
            }
            BonePose& target = pose.bone(static_cast<std::size_t>(index));

            if (!boneAnimation.rotation.empty()) {
                const glm::vec3 value =
                    boneAnimation.rotation.sample(layer.localTime, frameContext, glm::vec3{0.0F});
                // Additive: scaled add. Override: lerp the bone toward the value.
                target.rotation = (layer.mode == BlendMode::Override)
                                      ? target.rotation + (value - target.rotation) * w
                                      : target.rotation + value * w;
            }
            if (!boneAnimation.position.empty()) {
                const glm::vec3 value =
                    boneAnimation.position.sample(layer.localTime, frameContext, glm::vec3{0.0F});
                target.position = (layer.mode == BlendMode::Override)
                                      ? target.position + (value - target.position) * w
                                      : target.position + value * w;
            }
            if (!boneAnimation.scale.empty()) {
                const glm::vec3 value =
                    boneAnimation.scale.sample(layer.localTime, frameContext, glm::vec3{1.0F});
                target.scale = (layer.mode == BlendMode::Override)
                                   // Override: lerp the scale toward the value.
                                   ? target.scale + (value - target.scale) * w
                                   // Additive: compose multiplicatively toward 1.
                                   : target.scale * (glm::vec3{1.0F} + (value - glm::vec3{1.0F}) * w);
            }
        }
    };

    // Bedrock composition order: sum every additive layer first, then apply the
    // override layers in the order they were queued (each lerps against the pose
    // the earlier layers built). Two passes keep this order independent of how
    // additive and override layers happen to be interleaved by the caller.
    for (const Layer& layer : layers_) {
        if (layer.clip != nullptr && layer.mode == BlendMode::Additive) {
            applyLayer(layer);
        }
    }
    for (const Layer& layer : layers_) {
        if (layer.clip != nullptr && layer.mode == BlendMode::Override) {
            applyLayer(layer);
        }
    }
    return pose;
}

float Transition::value() const {
    if (duration_ <= 0.0F) {
        return to_;
    }
    float t = elapsed_ / duration_;
    if (t <= 0.0F) {
        return from_;
    }
    if (t >= 1.0F) {
        return to_;
    }
    if (eased_) {
        t = t * t * (3.0F - 2.0F * t); // smoothstep
    }
    return from_ + (to_ - from_) * t;
}

float Transition::advance(float deltaSeconds) {
    elapsed_ += std::max(deltaSeconds, 0.0F);
    if (elapsed_ > duration_) {
        elapsed_ = duration_;
    }
    return value();
}

void Transition::retarget(float to, float durationSeconds, bool eased) {
    from_ = value();
    to_ = to;
    duration_ = durationSeconds > 0.0F ? durationSeconds : 0.0F;
    elapsed_ = 0.0F;
    eased_ = eased;
}

} // namespace mc::animation

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

void Animator::addLayer(const AnimationClip& clip, float localTime, float weight) {
    if (weight <= 0.0F) {
        return;
    }
    layers_.push_back({&clip, localTime, weight});
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

    for (const Layer& layer : layers_) {
        if (layer.clip == nullptr) {
            continue;
        }
        // Expose this layer's clip time so channel expressions can reference it.
        frameContext.setQuery("anim_time", layer.localTime);

        for (const auto& [boneName, boneAnimation] : layer.clip->bones()) {
            const int index = model_->findBone(boneName);
            if (index < 0) {
                continue; // clip targets a bone this geometry does not have
            }
            BonePose& target = pose.bone(static_cast<std::size_t>(index));

            if (!boneAnimation.rotation.empty()) {
                target.rotation +=
                    boneAnimation.rotation.sample(layer.localTime, frameContext, glm::vec3{0.0F}) *
                    layer.weight;
            }
            if (!boneAnimation.position.empty()) {
                target.position +=
                    boneAnimation.position.sample(layer.localTime, frameContext, glm::vec3{0.0F}) *
                    layer.weight;
            }
            if (!boneAnimation.scale.empty()) {
                const glm::vec3 scale =
                    boneAnimation.scale.sample(layer.localTime, frameContext, glm::vec3{1.0F});
                // Compose scale multiplicatively, blended toward 1 by weight.
                target.scale *= glm::vec3{1.0F} + (scale - glm::vec3{1.0F}) * layer.weight;
            }
        }
    }
    return pose;
}

} // namespace mc::animation

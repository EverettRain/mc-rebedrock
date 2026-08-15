#include "animation/ModelAnimationSystem.hpp"

#include "animation/Molang.hpp"
#include "core/Json.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <fstream>
#include <glm/ext/matrix_transform.hpp>
#include <sstream>

namespace mc::animation {
namespace {

constexpr const char* kBreakClipName = "animation.held_item.break";
constexpr const char* kUseClipName = "animation.held_item.use";
constexpr const char* kEatClipName = "animation.held_item.eat";

// Built-in first-person swing clips. Rotations are authored in degrees and
// swing progress is the clip's normalised time. These mirror
// resources/animation/held_item.animation.json so the swing works with no
// external assets and unit tests stay hermetic.
//
// The eat clip is Molang rather than keyframes because vanilla's
// HeldItemRenderer#applyEatOrDrinkTransformation is a closed-form function of
// the remaining use time, and the curves are far easier to read as that
// function than as sampled keys. With `q.anim_time` in seconds from the start of
// the 32-tick (1.6 s) meal, vanilla's `f = itemUseTimeLeft - tickDelta + 1`
// is `32 - anim_time * 20` ticks and its `g = f / maxUseTime` is
// `1 - anim_time / 1.6`, so:
//   * `h = 1 - g^27` snaps from 0 to ~1 over the first four ticks — the lift
//     toward the mouth — and drives translate(h*0.6, h*-0.5, 0) plus the
//     Y 90° / Z 10° / X 30° tilt that turns the food to face the camera;
//   * once `g < 0.8` (after ~7 ticks) `abs(cos(f/4 * pi)) * 0.1` adds the
//     four-tick chewing bob. `math.cos` takes degrees here, so `f/4 * pi`
//     radians is `f * 45` degrees.
constexpr const char* kBuiltinClips = R"JSON({
  "format_version": "1.8.0",
  "animations": {
    "animation.held_item.break": {
      "loop": false,
      "animation_length": 0.3,
      "bones": {
        "item": {
          "position": {"0.0": [0,0,0], "0.15": [-0.11,-0.18,0.05], "0.3": [0,0,0]},
          "rotation": {"0.0": [0,0,0], "0.15": [0,-54.4296,0], "0.3": [0,0,0]},
          "scale":    {"0.0": [1,1,1], "0.15": [0.94,0.94,0.94], "0.3": [1,1,1]}
        }
      }
    },
    "animation.held_item.use": {
      "loop": false,
      "animation_length": 0.3,
      "bones": {
        "item": {
          "position": {"0.0": [0,0,0], "0.15": [-0.04,-0.11,-0.03], "0.3": [0,0,0]},
          "rotation": {"0.0": [0,0,0], "0.15": [0,-21.7714,0], "0.3": [0,0,0]},
          "scale":    {"0.0": [1,1,1], "0.15": [0.97,0.97,0.97], "0.3": [1,1,1]}
        }
      }
    },
    "animation.held_item.eat": {
      "loop": false,
      "animation_length": 1.6,
      "bones": {
        "item": {
          "position": [
            "(1 - math.pow(1 - q.anim_time / 1.6, 27)) * 0.6",
            "(1 - math.pow(1 - q.anim_time / 1.6, 27)) * -0.5 + (1 - q.anim_time / 1.6 < 0.8 ? math.abs(math.cos((32 - q.anim_time * 20) * 45)) * 0.1 : 0)",
            0
          ],
          "rotation": [
            "(1 - math.pow(1 - q.anim_time / 1.6, 27)) * 30",
            "(1 - math.pow(1 - q.anim_time / 1.6, 27)) * 90",
            "(1 - math.pow(1 - q.anim_time / 1.6, 27)) * 10"
          ],
          "scale": [1,1,1]
        }
      }
    }
  }
})JSON";

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

} // namespace

glm::mat4 firstPersonArmTransform(const ModelPose& pose) {
    constexpr float pi = 3.14159265358979323846F;
    const float swing = std::clamp(pose.swingProgress, 0.0F, 1.0F);
    const float rootSwing = std::sqrt(swing);
    const float x = -0.3F * std::sin(rootSwing * pi);
    const float y = 0.4F * std::sin(rootSwing * 2.0F * pi);
    const float z = -0.4F * std::sin(swing * pi);

    glm::mat4 transform{1.0F};
    transform = glm::translate(transform, {x + 0.64000005F, y - 0.6F, z - 0.72F});
    transform = glm::rotate(transform, glm::radians(45.0F), {0.0F, 1.0F, 0.0F});
    transform =
        glm::rotate(transform, glm::radians(std::sin(rootSwing * pi) * 70.0F), {0.0F, 1.0F, 0.0F});
    transform = glm::rotate(transform, glm::radians(std::sin(swing * swing * pi) * -20.0F),
                            {0.0F, 0.0F, 1.0F});
    transform = glm::translate(transform, {-1.0F, 3.6F, 3.5F});
    transform = glm::rotate(transform, glm::radians(120.0F), {0.0F, 0.0F, 1.0F});
    transform = glm::rotate(transform, glm::radians(200.0F), {1.0F, 0.0F, 0.0F});
    transform = glm::rotate(transform, glm::radians(-135.0F), {0.0F, 1.0F, 0.0F});
    transform = glm::translate(transform, {5.6F, 0.0F, 0.0F});
    // Right arm ModelPart pivot plus its 4x12x4 cuboid centre, in pixels / 16.
    return glm::translate(transform, {-0.375F, 0.375F, 0.0F});
}

glm::mat4 firstPersonItemTransform(const ModelPose& pose, bool cubeModel) {
    constexpr float pi = 3.14159265358979323846F;
    const float swing = std::clamp(pose.swingProgress, 0.0F, 1.0F);
    const float rootSwing = std::sqrt(swing);

    glm::mat4 transform{1.0F};
    transform = glm::translate(transform, {-0.4F * std::sin(rootSwing * pi),
                                           0.2F * std::sin(rootSwing * 2.0F * pi),
                                           -0.2F * std::sin(swing * pi)});
    transform = glm::translate(transform, {0.56F, -0.52F, -0.72F});
    transform = glm::rotate(transform, glm::radians(45.0F + std::sin(swing * swing * pi) * -20.0F),
                            {0.0F, 1.0F, 0.0F});
    transform =
        glm::rotate(transform, glm::radians(std::sin(rootSwing * pi) * -20.0F), {0.0F, 0.0F, 1.0F});
    transform =
        glm::rotate(transform, glm::radians(std::sin(rootSwing * pi) * -80.0F), {1.0F, 0.0F, 0.0F});
    transform = glm::rotate(transform, glm::radians(-45.0F), {0.0F, 1.0F, 0.0F});

    if (cubeModel) {
        transform = glm::rotate(transform, glm::radians(45.0F), {0.0F, 1.0F, 0.0F});
        transform = glm::scale(transform, {0.4F, 0.4F, 0.4F});
    } else {
        transform = glm::translate(transform, {1.13F / 16.0F, 3.2F / 16.0F, 1.13F / 16.0F});
        transform = glm::rotate(transform, glm::radians(-90.0F), {0.0F, 1.0F, 0.0F});
        transform = glm::rotate(transform, glm::radians(25.0F), {0.0F, 0.0F, 1.0F});
        transform = glm::scale(transform, {0.68F, 0.68F, 0.68F});
    }
    return transform;
}

// The held item during eating. HeldItemRenderer#renderFirstPersonItem takes the
// UseAction.EAT branch: it runs applyEatOrDrinkTransformation *first*, then
// applyEquipOffset, and skips the swing translate/applySwingOffset entirely. So
// the clip's lift and tilt are camera-space and wrap the resting hand pose —
// applying them the other way round (inside the item's own space, after the
// -45° swing rotation, the display transform and its 0.68 scale) sent the food
// off in an arbitrary direction instead of up to the mouth.
glm::mat4 firstPersonEatTransform(const ModelPose& pose, bool cubeModel) {
    glm::mat4 transform{1.0F};
    transform = glm::translate(transform, pose.translation);
    transform = glm::rotate(transform, glm::radians(pose.rotationDegrees.y), {0.0F, 1.0F, 0.0F});
    transform = glm::rotate(transform, glm::radians(pose.rotationDegrees.z), {0.0F, 0.0F, 1.0F});
    transform = glm::rotate(transform, glm::radians(pose.rotationDegrees.x), {1.0F, 0.0F, 0.0F});
    // A swing-free pose reduces firstPersonItemTransform to exactly what vanilla
    // runs after the eat transformation: applyEquipOffset (its 45°/-45° pair
    // cancels at rest) followed by the first-person display transform.
    transform = transform * firstPersonItemTransform(ModelPose{}, cubeModel);
    return glm::scale(transform, glm::vec3{pose.scale});
}

ModelAnimationSystem::ModelAnimationSystem() {
    library_ = AnimationLibrary::parse(kBuiltinClips);
}

void ModelAnimationSystem::load(const std::filesystem::path& animationDirectory) {
    const auto path = animationDirectory / "held_item.animation.json";
    try {
        const std::string text = readFile(path);
        if (text.empty()) {
            return;
        }
        AnimationLibrary loaded;
        loaded.loadDocument(core::Json::parse(text));
        // Only adopt the file if it actually provides the clips we need.
        if (loaded.find(kBreakClipName) != nullptr && loaded.find(kUseClipName) != nullptr &&
            loaded.find(kEatClipName) != nullptr) {
            library_ = std::move(loaded);
        }
    } catch (const std::exception&) {
        // Keep the built-in clips on any read/parse failure.
    }
}

const AnimationClip* ModelAnimationSystem::clipFor(ModelAction action) const {
    switch (action) {
        case ModelAction::Break:
            return library_.find(kBreakClipName);
        case ModelAction::Use:
            return library_.find(kUseClipName);
        case ModelAction::Eat:
            return library_.find(kEatClipName);
        case ModelAction::None:
            break;
    }
    return nullptr;
}

void ModelAnimationSystem::setAction(ModelAction action, float progress) {
    action_ = action;
    if (action == ModelAction::None) {
        pose_ = {};
        return;
    }
    sample(std::clamp(progress, 0.0F, 1.0F));
}

void ModelAnimationSystem::sample(float progress) {
    const AnimationClip* clip = clipFor(action_);
    if (clip == nullptr) {
        pose_ = {};
        return;
    }
    const BoneAnimation* item = clip->findBone("item");
    if (item == nullptr) {
        pose_ = {};
        return;
    }
    const float local = clip->localTime(progress * clip->length());
    MolangContext context;
    context.setQuery("anim_time", local);

    pose_.translation = item->position.sample(local, context, glm::vec3{0.0F});
    pose_.rotationDegrees = item->rotation.sample(local, context, glm::vec3{0.0F});
    pose_.scale = item->scale.sample(local, context, glm::vec3{1.0F}).x;
    pose_.swingProgress = progress;
}

} // namespace mc::animation

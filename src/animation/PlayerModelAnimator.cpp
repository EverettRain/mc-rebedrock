#include "animation/PlayerModelAnimator.hpp"

#include "animation/Molang.hpp"
#include "core/Json.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <sstream>

namespace mc::animation {
namespace {

constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;

// Look sensitivity: the normalised cursor offset maps to these head angles (in
// degrees) so the preview keeps roughly the previous procedural feel while the
// motion is now authored data.
constexpr float kLookYawDegrees = 55.0F;
constexpr float kLookPitchDegrees = 27.5F;

// Built-in preview skeleton: classic 8:12:12 Steve proportions.
constexpr const char* kBuiltinGeometry = R"({
  "format_version": "1.12.0",
  "minecraft:geometry": [
    { "description": {"identifier": "geometry.player.preview", "texture_width": 64, "texture_height": 64},
      "bones": [
        {"name": "body", "pivot": [0, 24, 0], "cubes": [{"origin": [-4,12,-2], "size": [8,12,4], "uv": [16,16]}]},
        {"name": "head", "parent": "body", "pivot": [0, 24, 0], "cubes": [{"origin": [-4,24,-4], "size": [8,8,8], "uv": [0,0]}]},
        {"name": "rightArm", "parent": "body", "pivot": [-5, 22, 0], "cubes": [{"origin": [-8,12,-2], "size": [4,12,4], "uv": [40,16]}]},
        {"name": "leftArm", "parent": "body", "pivot": [5, 22, 0], "cubes": [{"origin": [4,12,-2], "size": [4,12,4], "uv": [32,48]}]},
        {"name": "rightLeg", "pivot": [-1.9, 12, 0], "cubes": [{"origin": [-3.9,0,-2], "size": [4,12,4], "uv": [0,16]}]},
        {"name": "leftLeg", "pivot": [1.9, 12, 0], "cubes": [{"origin": [-0.1,0,-2], "size": [4,12,4], "uv": [16,48]}]}
      ]
    }
  ]
})";

constexpr const char* kBuiltinAnimations = R"({
  "format_version": "1.8.0",
  "animations": {
    "animation.player.walk": {
      "loop": true, "animation_length": 1.0,
      "bones": {
        "rightLeg": {"rotation": ["math.cos(query.anim_time * 360) * 40 * variable.walk_amount", 0, 0]},
        "leftLeg":  {"rotation": ["math.cos(query.anim_time * 360 + 180) * 40 * variable.walk_amount", 0, 0]},
        "rightArm": {"rotation": ["math.cos(query.anim_time * 360 + 180) * 40 * variable.walk_amount", 0, 0]},
        "leftArm":  {"rotation": ["math.cos(query.anim_time * 360) * 40 * variable.walk_amount", 0, 0]}
      }
    },
    "animation.player.idle": {
      "loop": true, "animation_length": 3.0,
      "bones": {
        "rightArm": {"rotation": [0, 0, "math.cos(query.anim_time * 120) * 2 + 3"]},
        "leftArm":  {"rotation": [0, 0, "math.cos(query.anim_time * 120) * -2 - 3"]},
        "body":     {"position": [0, "math.cos(query.anim_time * 120) * 0.1", 0]}
      }
    },
    "animation.player.look": {
      "loop": true, "animation_length": 1.0,
      "bones": {
        // The body turns with the look at half the head's amplitude
        // (body_look_amount = 0.5 in the preview, 0 for the world player),
        // matching vanilla EntityRenderer#drawEntity's bodyYaw = f*20 vs
        // yaw = f*40. Because the head is a child of the body in the geometry,
        // the animator composes the two rotations; the preview pose then just
        // reads the resulting bone rotations instead of rotating parts by hand.
        "body": {"rotation": [0, "variable.look_yaw * variable.body_look_amount", 0]},
        "head": {"rotation": ["variable.look_pitch", "variable.look_yaw", 0]}
      }
    },
    "animation.player.sneak": {
      "loop": true, "animation_length": 1.0,
      "bones": {
        "body": {"rotation": [28, 0, 0], "position": [0, -1.405, 5.634]},
        "head": {"rotation": [-16, 0, 0]}
      }
    }
  }
})";

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

} // namespace

PlayerModelAnimator::PlayerModelAnimator() {
    model_ = SkeletalModel::parse(kBuiltinGeometry);
    library_ = AnimationLibrary::parse(kBuiltinAnimations);
    animator_.setModel(&model_);
    rebindBones();
}

void PlayerModelAnimator::rebindBones() {
    animator_.setModel(&model_);
    bodyBone_ = model_.findBone("body");
    headBone_ = model_.findBone("head");
    rightArmBone_ = model_.findBone("rightArm");
    leftArmBone_ = model_.findBone("leftArm");
    rightLegBone_ = model_.findBone("rightLeg");
    leftLegBone_ = model_.findBone("leftLeg");
}

void PlayerModelAnimator::load(const std::filesystem::path& animationDirectory) {
    try {
        SkeletalModel model = SkeletalModel::loadGeometry(
            core::Json::parse(readFile(animationDirectory / "player.geo.json")));
        AnimationLibrary library;
        library.loadDocument(core::Json::parse(readFile(animationDirectory / "player.animation.json")));
        if (library.find("animation.player.walk") == nullptr ||
            library.find("animation.player.look") == nullptr) {
            return; // incomplete override, keep built-ins
        }
        model_ = std::move(model);
        library_ = std::move(library);
        rebindBones();
    } catch (const std::exception&) {
        // Keep the built-in preview assets on any read/parse failure.
    }
}

void PlayerModelAnimator::setCursorLook(float normalizedX, float normalizedY) {
    lookX_ = std::clamp(normalizedX, -1.0F, 1.0F);
    lookY_ = std::clamp(normalizedY, -1.0F, 1.0F);
}

void PlayerModelAnimator::update(float deltaSeconds, bool walking, bool sneaking) {
    const float dt = std::max(deltaSeconds, 0.0F);
    elapsed_ += dt;

    // Ease the blend weights toward their targets so starting/stopping a state
    // (walk, sneak) fades over a few frames instead of snapping. rate*dt is the
    // fraction of the remaining gap closed this frame.
    const auto approach = [dt](float current, float target, float rate) {
        return current + (target - current) * std::min(1.0F, rate * dt);
    };
    walkAmount_ = approach(walkAmount_, walking ? 1.0F : 0.0F, 10.0F);
    sneakAmount_ = approach(sneakAmount_, sneaking ? 1.0F : 0.0F, 9.0F);

    animator_.context().setVariable("walk_amount", walkAmount_);
    animator_.context().setVariable("look_yaw", lookX_ * kLookYawDegrees);
    animator_.context().setVariable("look_pitch", lookY_ * kLookPitchDegrees);
    // The look clip turns the body bone with the look at half the head's yaw
    // amplitude when the preview enabled it; the world player keeps it off so
    // its own "head leads, body follows" yaw is not doubled.
    animator_.context().setVariable("body_look_amount", bodyFollowsLook_ ? 0.5F : 0.0F);

    animator_.clearLayers();
    if (const AnimationClip* walk = library_.find("animation.player.walk")) {
        animator_.addLayer(*walk, walk->localTime(elapsed_), 1.0F);
    }
    if (const AnimationClip* idle = library_.find("animation.player.idle")) {
        animator_.addLayer(*idle, idle->localTime(elapsed_), 1.0F - walkAmount_);
    }
    if (const AnimationClip* sneak = library_.find("animation.player.sneak")) {
        animator_.addLayer(*sneak, sneak->localTime(elapsed_), sneakAmount_);
    }
    if (const AnimationClip* look = library_.find("animation.player.look")) {
        animator_.addLayer(*look, 0.0F, 1.0F);
    }

    skeletonPose_ = animator_.evaluate();
    const SkeletonPose& pose = skeletonPose_;
    const auto rotation = [&](int bone) -> glm::vec3 {
        return bone >= 0 ? pose.bone(static_cast<std::size_t>(bone)).rotation : glm::vec3{0.0F};
    };

    // The look clip already rotated the body bone (at body_look_amount), so the
    // preview pose just projects the animation-library result: bodyYaw is the
    // body bone's yaw, and headYaw is the head's yaw relative to it (the preview
    // renderer applies bodyYaw to the body/arms/legs and bodyYaw + headYaw to
    // the head, keeping the head at its full look amplitude).
    const float bodyYawDeg = rotation(bodyBone_).y;
    const float headYawDeg = rotation(headBone_).y;
    pose_.bodyYaw = bodyYawDeg * kDegToRad;
    pose_.headYaw = (headYawDeg - bodyYawDeg) * kDegToRad;
    pose_.headPitch = rotation(headBone_).x * kDegToRad;
    pose_.rightArmPitch = rotation(rightArmBone_).x * kDegToRad;
    pose_.leftArmPitch = rotation(leftArmBone_).x * kDegToRad;
    pose_.rightLegPitch = rotation(rightLegBone_).x * kDegToRad;
    pose_.leftLegPitch = rotation(leftLegBone_).x * kDegToRad;

    const float bodyBobUnits =
        bodyBone_ >= 0 ? pose.bone(static_cast<std::size_t>(bodyBone_)).position.y : 0.0F;
    pose_.idleBob = bodyBobUnits * 0.18F;
}

} // namespace mc::animation

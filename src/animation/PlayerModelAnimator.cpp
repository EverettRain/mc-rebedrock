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
        // B1/B2 (26.1 §3): cos(walk_position * 0.6662 (+PI)) * A * walk_amount,
        // arm A = 57.3 deg (1.0 rad), leg A = 80.2 deg (1.4 rad = arm * 1.4).
        // Diagonal sync: rightArm<->leftLeg, leftArm<->rightLeg. walk_position is
        // the vanilla phase accumulator (position += speed); walk_amount the eased
        // amplitude. NOTE: Molang math.cos takes DEGREES, so the vanilla radian
        // frequency 0.6662 is expressed as 0.6662*180/pi = 38.166 deg/unit, and
        // the +PI anti-phase as +180 deg.
        "rightLeg": {"rotation": ["math.cos(variable.walk_position * 38.166) * 80.2 * variable.walk_amount", 0, 0]},
        "leftLeg":  {"rotation": ["math.cos(variable.walk_position * 38.166 + 180) * 80.2 * variable.walk_amount", 0, 0]},
        "rightArm": {"rotation": ["math.cos(variable.walk_position * 38.166 + 180) * 57.3 * variable.walk_amount", 0, 0]},
        "leftArm":  {"rotation": ["math.cos(variable.walk_position * 38.166) * 57.3 * variable.walk_amount", 0, 0]}
      }
    },
    "animation.player.idle": {
      "loop": true, "animation_length": 3.0,
      "bones": {
        // A3/B3 (26.1 §4): the ONLY idle motion is AnimationUtils.bobModelPart on
        // the two arms — Z sways outward-only cos(age*0.09)*2.86 + 2.86 (mirrored
        // right +, left -), X front/back sin(age*0.067)*2.86 (reversed per arm).
        // The body/head/legs stay still — NO body Y bob. age = ticks. NOTE: Molang
        // trig is in DEGREES, so the radian rates become 0.09*180/pi = 5.1566 and
        // 0.067*180/pi = 3.8388 deg/tick.
        "rightArm": {"rotation": ["math.sin(variable.idle_age * 3.8388) * 2.86", 0, "math.cos(variable.idle_age * 5.1566) * 2.86 + 2.86"]},
        "leftArm":  {"rotation": ["math.sin(variable.idle_age * 3.8388) * -2.86", 0, "math.cos(variable.idle_age * 5.1566) * -2.86 - 2.86"]}
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
        // B4 (26.1 §9): body leans +28.65 deg (0.5 rad) on X. head is a child of
        // body here, so it inherits that lean; to keep the gaze level it fully
        // compensates with -28.65 deg (the previous -16 under-compensated, tilting
        // the head down). The look clip then adds the view pitch on top.
        "body": {"rotation": [28.65, 0, 0], "position": [0, -1.405, 5.634]},
        "head": {"rotation": [-28.65, 0, 0]}
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
    controllers_ = builtinPlayerControllers();
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

    // ANIM-1 named masks derived from the (possibly overridden) skeleton, so the
    // layered player animation splits legs / arms / head cleanly.
    masks_ = buildBoneGroups(model_);

    // The item-hold override pose: an authored clip that pitches both arms
    // forward. Applied as an ANIM-2 override masked to the arms so it replaces the
    // walk swing rather than summing with it.
    itemHoldClip_ = AnimationClip{};
    itemHoldClip_.setLength(0.0F);
    itemHoldClip_.setLoop(false);

    // The ANIM-3 locomotion controller drives idle/walk/sneak; reset it onto the
    // (rebound) player skeleton.
    if (const AnimationController* loco = controllers_.find("controller.player.locomotion")) {
        controllerInstance_.bind(*loco);
    }
}

void PlayerModelAnimator::setItemHold(bool holding, float pitchDegrees) {
    holdingItem_ = holding;
    itemHoldPitch_ = pitchDegrees;
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
    // Preview path: it has no world displacement, so it synthesizes the drive
    // quantities. The walk amplitude is full in the walk state (the controller
    // crossfade owns the idle<->walk state blend); the phase advances from the
    // clock at the ~2.16 rad/s a real walk accumulates (0.6662 * 0.216 blk/tick *
    // 20 tick/s); the idle age is the elapsed clock in ticks.
    elapsed_ += dt;
    const float walkAmount = walking ? 1.0F : 0.0F;
    walkPosition_ += dt * 20.0F * 0.216F;
    evaluatePose(dt, walkAmount, walkPosition_, elapsed_ * 20.0F, sneaking);
}

void PlayerModelAnimator::updateWorldPlayer(float deltaSeconds, float walkAmount,
                                            float walkPosition, float ageInTicks, bool sneaking) {
    // World-player path: feed the AUTHORITATIVE vanilla WalkAnimationState the
    // controller published (walkAnimationSpeed / walkAnimationPosition) and the
    // real render age, so the same controller stack drives idle/walk/sneak from
    // true movement (idle -> no swing, stopping decays to rest) instead of a
    // synthesized clock. Look + item-hold are set through setCursorLook /
    // setItemHold before this call; body_look_amount stays 0 (the renderer applies
    // the world body yaw at the model root).
    elapsed_ += std::max(deltaSeconds, 0.0F);
    evaluatePose(std::max(deltaSeconds, 0.0F), walkAmount, walkPosition, ageInTicks, sneaking);
}

void PlayerModelAnimator::evaluatePose(float dt, float walkAmount, float walkPosition,
                                       float idleAgeTicks, bool sneaking) {
    walking_ = walkAmount > 0.1F;
    sneaking_ = sneaking;
    walkAmount_ = walkAmount;

    MolangContext& ctx = animator_.context();
    ctx.setVariable("walk_amount", walkAmount_);
    // B2: the walk clip is phase-driven by walk_position (vanilla `position`).
    ctx.setVariable("walk_position", walkPosition);
    // A3: the idle bob is driven by age in ticks.
    ctx.setVariable("idle_age", idleAgeTicks);
    ctx.setVariable("sneaking", sneaking ? 1.0F : 0.0F);
    ctx.setVariable("look_yaw", lookX_ * kLookYawDegrees);
    ctx.setVariable("look_pitch", lookY_ * kLookPitchDegrees);
    // The look clip turns the body bone with the look at half the head's yaw
    // amplitude when the preview enabled it; the world player keeps it off so
    // its own "head leads, body follows" yaw is not doubled.
    ctx.setVariable("body_look_amount", bodyFollowsLook_ ? 0.5F : 0.0F);
    ctx.setQuery("anim_time", elapsed_);

    // ANIM-3: advance the state machine (idle/walk/sneak) and crossfade, then let
    // it queue the active state's locomotion/sneak clips onto the animator.
    controllerInstance_.update(dt, ctx);
    animator_.clearLayers();
    const MaskResolver resolveMask = [this](std::string_view name) -> const BoneMask* {
        if (name == "lower_body") return &masks_.lowerBody;
        if (name == "upper_body") return &masks_.upperBody;
        if (name == "arms") return &masks_.arms;
        if (name == "head") return &masks_.head;
        return nullptr;
    };
    controllerInstance_.apply(animator_, library_, resolveMask, ctx);

    // ANIM-1 head mask: the look drives the head (and, in the preview, the body at
    // half amplitude) independently of locomotion and sneak. Kept as its own layer
    // so head tracking is always live regardless of the locomotion state.
    if (const AnimationClip* look = library_.find("animation.player.look")) {
        // body_look_amount routes the body yaw inside the clip; mask to head+body
        // union via the head/torso groups so it never disturbs arms or legs.
        animator_.addLayer(*look, 0.0F, 1.0F, &masks_.upperBody);
    }

    // ANIM-2 override: an item-hold pose masked to the arms REPLACES the arm swing
    // rather than summing with it — walking while holding an item no longer bends
    // the held pose by the gait. This is the separation fix (upper vs lower body).
    if (holdingItem_) {
        BoneAnimation& rightArm = itemHoldClip_.bone("rightArm");
        BoneAnimation& leftArm = itemHoldClip_.bone("leftArm");
        rightArm.rotation = AnimationChannel{};
        leftArm.rotation = AnimationChannel{};
        rightArm.rotation.addLinear(0.0F, glm::vec3{itemHoldPitch_, 0.0F, 0.0F});
        leftArm.rotation.addLinear(0.0F, glm::vec3{itemHoldPitch_, 0.0F, 0.0F});
        animator_.addLayer(itemHoldClip_, 0.0F, 1.0F, &masks_.arms, BlendMode::Override);
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

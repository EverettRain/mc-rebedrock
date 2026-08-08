#include "animation/AnimationAssets.hpp"
#include "animation/Animator.hpp"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <glm/geometric.hpp>

using namespace mc::animation;

#ifndef MC_REBEDROCK_ANIMATION_DIR
#error "MC_REBEDROCK_ANIMATION_DIR must point at resources/animation"
#endif

namespace {
const std::filesystem::path kDir{MC_REBEDROCK_ANIMATION_DIR};
}

int main() {
    // The player, a block (chest) and a mob (quadruped) all load through the
    // identical geometry + animation pipeline.
    const AnimatedModel player = loadAnimatedModel(
        kDir / "player.geo.json", {kDir / "player.animation.json"});
    assert(player.model.boneCount() == 7U);
    assert(player.animations.find("animation.player.walk") != nullptr);
    assert(player.animations.find("animation.player.look") != nullptr);

    // Blend a looping walk with a procedural look override, exactly as the game
    // would each frame.
    Animator animator;
    animator.setModel(&player.model);
    animator.context().setVariable("walk_amount", 1.0F);
    animator.context().setVariable("look_yaw", 20.0F);
    animator.context().setVariable("look_pitch", -10.0F);
    animator.clearLayers();
    animator.addLayer(*player.animations.find("animation.player.walk"), 0.0F, 1.0F);
    animator.addLayer(*player.animations.find("animation.player.look"), 0.0F, 1.0F);
    const SkeletonPose pose = animator.evaluate();
    const int head = player.model.findBone("head");
    const int rightLeg = player.model.findBone("rightLeg");
    assert(std::abs(pose.bone(static_cast<std::size_t>(head)).rotation.y - 20.0F) < 1e-3F);
    assert(std::abs(pose.bone(static_cast<std::size_t>(head)).rotation.x + 10.0F) < 1e-3F);
    assert(std::abs(pose.bone(static_cast<std::size_t>(rightLeg)).rotation.x - 40.0F) < 1e-3F);

    // Block animation: the chest lid is driven entirely by a data variable, and
    // the knob rides the lid through the bone hierarchy.
    const AnimatedModel chest = loadAnimatedModel(
        kDir / "chest.geo.json", {kDir / "chest.animation.json"});
    const AnimationClip* lidClip = chest.animations.find("animation.chest.lid");
    assert(lidClip != nullptr);
    const int knob = chest.model.findBone("knob");
    const auto knobAt = [&](float angle) {
        Animator chestAnimator;
        chestAnimator.setModel(&chest.model);
        chestAnimator.context().setVariable("lid_angle", angle);
        chestAnimator.clearLayers();
        chestAnimator.addLayer(*lidClip, 0.0F, 1.0F);
        return chestAnimator.evaluate().worldMatrix(knob);
    };
    const glm::vec4 knobPoint{0.0F, 10.0F, -8.0F, 1.0F};
    assert(glm::length(glm::vec3(knobAt(0.0F) * knobPoint - knobAt(1.0F) * knobPoint)) > 1.0F);

    // Mob animation: a quadruped walk cycle over the same runtime.
    const AnimatedModel quadruped = loadAnimatedModel(
        kDir / "quadruped.geo.json", {kDir / "quadruped.animation.json"});
    assert(quadruped.model.boneCount() == 6U);
    Animator mob;
    mob.setModel(&quadruped.model);
    mob.context().setVariable("walk_amount", 1.0F);
    mob.playSingle(*quadruped.animations.find("animation.quadruped.walk"), 0.5F);
    const int frontRight = quadruped.model.findBone("legFrontRight");
    // cos(180) * 30 = -30 degrees at t = 0.5.
    assert(std::abs(mob.evaluate().bone(static_cast<std::size_t>(frontRight)).rotation.x + 30.0F) <
           1e-3F);

    // Cow: the 1.16.1 CowEntityModel port. Nine bones — the quadruped's six,
    // plus the udder and the two horns the cow model adds as children of the
    // torso and head — and the walk clip swings the legs at the vanilla
    // setAngles cadence (0.6662 frequency, 1.4 rad = 80.2 degrees amplitude)
    // with the front-right anti-phase to the front-left — the same diagonal
    // gait 1.16.1 applies.
    const AnimatedModel cow = loadAnimatedModel(
        kDir / "cow.geo.json", {kDir / "cow.animation.json"});
    assert(cow.model.boneCount() == 9U);
    assert(cow.animations.find("animation.cow.walk") != nullptr);
    assert(cow.animations.find("animation.cow.idle") != nullptr);
    Animator cowMob;
    cowMob.setModel(&cow.model);
    cowMob.context().setVariable("walk_amount", 1.0F);
    cowMob.clearLayers();
    cowMob.addLayer(*cow.animations.find("animation.cow.walk"), 0.5F);
    const int cowFrontRight = cow.model.findBone("legFrontRight");
    const int cowFrontLeft = cow.model.findBone("legFrontLeft");
    const int cowBackRight = cow.model.findBone("legBackRight");
    const int cowBackLeft = cow.model.findBone("legBackLeft");
    assert(cowFrontRight >= 0 && cowFrontLeft >= 0 && cowBackRight >= 0 && cowBackLeft >= 0);
    const SkeletonPose cowPose = cowMob.evaluate();
    // At t = 0.5: cos(0.5 * 360 * 0.6662) = cos(119.916 deg) = -0.4986, so the
    // front-left and back-right legs swing back ~40 degrees while the opposite
    // diagonal (+180) swings forward ~40 degrees. The two diagonal pairs move
    // together exactly as 1.16.1's setAngles pairs them.
    assert(std::abs(cowPose.bone(static_cast<std::size_t>(cowFrontRight)).rotation.x - 40.0F) <
           0.1F);
    assert(std::abs(cowPose.bone(static_cast<std::size_t>(cowFrontLeft)).rotation.x + 40.0F) <
           0.1F);
    assert(std::abs(cowPose.bone(static_cast<std::size_t>(cowBackRight)).rotation.x + 40.0F) <
           0.1F);
    assert(std::abs(cowPose.bone(static_cast<std::size_t>(cowBackLeft)).rotation.x - 40.0F) <
           0.1F);

    // Zombie: the biped box-UV mob. Its left limbs mirror the right-hand skin
    // regions (the classic Java zombie skin only draws the right limbs), and
    // the walk clip swings the legs with the arms held forward.
    const AnimatedModel zombie = loadAnimatedModel(
        kDir / "zombie.geo.json", {kDir / "zombie.animation.json"});
    assert(zombie.model.boneCount() == 6U);
    assert(zombie.animations.find("animation.zombie.walk") != nullptr);
    assert(zombie.animations.find("animation.zombie.idle") != nullptr);
    const int zombieRightArm = zombie.model.findBone("rightArm");
    const int zombieLeftArm = zombie.model.findBone("leftArm");
    const int zombieRightLeg = zombie.model.findBone("rightLeg");
    const int zombieLeftLeg = zombie.model.findBone("leftLeg");
    assert(zombieRightArm >= 0 && zombieLeftArm >= 0 && zombieRightLeg >= 0 && zombieLeftLeg >= 0);
    assert(!zombie.model.bones()[static_cast<std::size_t>(zombieRightArm)].cubes[0].mirror);
    assert(zombie.model.bones()[static_cast<std::size_t>(zombieLeftArm)].cubes[0].mirror);
    assert(!zombie.model.bones()[static_cast<std::size_t>(zombieRightLeg)].cubes[0].mirror);
    assert(zombie.model.bones()[static_cast<std::size_t>(zombieLeftLeg)].cubes[0].mirror);
    Animator zombieMob;
    zombieMob.setModel(&zombie.model);
    zombieMob.context().setVariable("walk_amount", 1.0F);
    // The in-game renderer blends walk + idle every frame, and the Animator
    // *adds* each layer's rotation to the bone. A pose duplicated in both clips
    // would therefore double (90+90 = straight up, a surrender pose), so the
    // arms live in the idle clip only and the walk swings the legs. Blend both
    // exactly as drawWorldEntities does and check the totals.
    zombieMob.clearLayers();
    zombieMob.addLayer(*zombie.animations.find("animation.zombie.walk"), 0.5F);
    zombieMob.addLayer(*zombie.animations.find("animation.zombie.idle"), 0.0F);
    const SkeletonPose zombiePose = zombieMob.evaluate();
    // cos(180) * 40 = -40 degrees at t = 0.5 (legs come from the walk clip only).
    assert(std::abs(zombiePose.bone(static_cast<std::size_t>(zombieRightLeg)).rotation.x + 40.0F) <
           1e-3F);
    // The blended pose holds the arms straight out front at exactly 90 degrees.
    assert(std::abs(zombiePose.bone(static_cast<std::size_t>(zombieRightArm)).rotation.x - 90.0F) <
           1e-3F);
    assert(std::abs(zombiePose.bone(static_cast<std::size_t>(zombieLeftArm)).rotation.x - 90.0F) <
           1e-3F);

    return 0;
}

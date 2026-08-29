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

    // Cow: the Java 26.1 normal adult CowModel port. Its 64x64 skin has a
    // separate muzzle below the old 64x32 region, while the horns and udder are
    // cubes on the head and body model parts. The walk clip swings the legs at
    // the vanilla cadence with the front-right anti-phase to the front-left.
    const AnimatedModel cow = loadAnimatedModel(
        kDir / "cow.geo.json", {kDir / "cow.animation.json"});
    assert(cow.model.textureWidth() == 64);
    assert(cow.model.textureHeight() == 64);
    assert(cow.model.boneCount() == 6U);
    assert(cow.animations.find("animation.cow.walk") != nullptr);
    assert(cow.animations.find("animation.cow.idle") != nullptr);
    const int cowHead = cow.model.findBone("head");
    const int cowBody = cow.model.findBone("body");
    assert(cowHead >= 0 && cowBody >= 0);
    const auto& cowHeadCubes = cow.model.bones()[static_cast<std::size_t>(cowHead)].cubes;
    const auto& cowBodyCubes = cow.model.bones()[static_cast<std::size_t>(cowBody)].cubes;
    assert(cowHeadCubes.size() == 4U);
    assert(cowBodyCubes.size() == 2U);
    assert(glm::length(cowHeadCubes[1].origin - glm::vec3{-3.0F, 16.0F, -15.0F}) < 1e-3F);
    assert(glm::length(cowHeadCubes[1].size - glm::vec3{6.0F, 3.0F, 1.0F}) < 1e-3F);
    assert(glm::length(cowHeadCubes[1].uv - glm::vec2{1.0F, 33.0F}) < 1e-3F);
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
    assert(std::abs(cow.model.bones()[static_cast<std::size_t>(cowFrontRight)].pivot.z + 5.0F) <
           1e-3F);
    assert(!cow.model.bones()[static_cast<std::size_t>(cowFrontRight)].cubes[0].mirror);
    assert(cow.model.bones()[static_cast<std::size_t>(cowFrontLeft)].cubes[0].mirror);
    assert(!cow.model.bones()[static_cast<std::size_t>(cowBackRight)].cubes[0].mirror);
    assert(cow.model.bones()[static_cast<std::size_t>(cowBackLeft)].cubes[0].mirror);
    const SkeletonPose cowPose = cowMob.evaluate();
    // At t = 0.5: cos(0.5 * 360 * 0.6662) = cos(119.916 deg) = -0.4986, so the
    // front-left and back-right legs swing back ~40 degrees while the opposite
    // diagonal (+180) swings forward ~40 degrees. The two diagonal pairs move
    // together exactly as 26.1's QuadrupedModel pairs them.
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
    // RN-1 (#15): body/head/2 arms/2 legs plus the head-child hat overlay.
    assert(zombie.model.boneCount() == 7U);
    assert(zombie.animations.find("animation.zombie.walk") != nullptr);
    assert(zombie.animations.find("animation.zombie.idle") != nullptr);
    // The hat is a head child with +0.5 inflate (26.1 HumanoidModel hat layer).
    const int zombieHat = zombie.model.findBone("hat");
    const int zombieHead = zombie.model.findBone("head");
    assert(zombieHat >= 0 && zombieHead >= 0);
    assert(zombie.model.bones()[static_cast<std::size_t>(zombieHat)].parent == zombieHead);
    assert(std::abs(zombie.model.bones()[static_cast<std::size_t>(zombieHat)].cubes[0].inflate -
                    0.5F) < 1e-3F);
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

    // RN-1 (#3): Sheep rebuilt as JE 26.1's two-layer model — a base
    // SheepModel (legSize=12, right legs mirror) plus a SheepFurModel fleece
    // overlay (head inflate 0.6, body "wool" inflate 1.75, legs inflate 0.5).
    // The fur bones are children of their base counterparts so the fleece rides
    // the animated body/legs. Structure + inflate asserted here; proportions/
    // texture/look are 待 mac (not verifiable headless).
    const AnimatedModel sheep = loadAnimatedModel(
        kDir / "sheep.geo.json", {kDir / "sheep.animation.json"});
    // base: head, body, 4 legs (6) + fur: woolHead, wool, 4 wool legs (6) = 12.
    assert(sheep.model.boneCount() == 12U);
    assert(sheep.animations.find("animation.sheep.walk") != nullptr);
    assert(sheep.animations.find("animation.sheep.idle") != nullptr);
    const int sheepWool = sheep.model.findBone("wool");
    const int sheepBody = sheep.model.findBone("body");
    const int sheepHead = sheep.model.findBone("head");
    assert(sheepWool >= 0 && sheepBody >= 0 && sheepHead >= 0);
    // The wool (fur body) bone is parented to the body, so the fleece overlay
    // rides the sheep's torso — moving the body moves the wool with it. It
    // carries the 26.1 SheepFurModel body inflate (1.75), the fleece's thickness.
    assert(sheep.model.bones()[static_cast<std::size_t>(sheepWool)].parent == sheepBody);
    assert(std::abs(sheep.model.bones()[static_cast<std::size_t>(sheepWool)].cubes[0].inflate -
                    1.75F) < 1e-3F);
    // The fur head/leg overlays exist and hang off their base bones.
    const int sheepWoolHead = sheep.model.findBone("woolHead");
    const int sheepWoolFrontRight = sheep.model.findBone("woolLegFrontRight");
    assert(sheepWoolHead >= 0 && sheepWoolFrontRight >= 0);
    assert(sheep.model.bones()[static_cast<std::size_t>(sheepWoolHead)].parent == sheepHead);
    assert(std::abs(sheep.model.bones()[static_cast<std::size_t>(sheepWoolHead)].cubes[0].inflate -
                    0.6F) < 1e-3F);
    const int sheepFrontRight = sheep.model.findBone("legFrontRight");
    const int sheepFrontLeft = sheep.model.findBone("legFrontLeft");
    const int sheepBackRight = sheep.model.findBone("legBackRight");
    const int sheepBackLeft = sheep.model.findBone("legBackLeft");
    assert(sheepFrontRight >= 0 && sheepFrontLeft >= 0 && sheepBackRight >= 0 &&
           sheepBackLeft >= 0);
    // 26.1 SheepModel uses mirrorRightLeg=true, mirrorLeftLeg=false — the RIGHT
    // legs mirror (opposite the cow), the fur legs never mirror.
    assert(sheep.model.bones()[static_cast<std::size_t>(sheepFrontRight)].cubes[0].mirror);
    assert(!sheep.model.bones()[static_cast<std::size_t>(sheepFrontLeft)].cubes[0].mirror);
    assert(!sheep.model.bones()[static_cast<std::size_t>(sheepWoolFrontRight)].cubes[0].mirror);
    // Base legs are legSize=12 (4x12x4), the 26.1 proportion (not vanilla's 10).
    assert(std::abs(sheep.model.bones()[static_cast<std::size_t>(sheepFrontRight)].cubes[0].size.y -
                    12.0F) < 1e-3F);
    Animator sheepMob;
    sheepMob.setModel(&sheep.model);
    sheepMob.context().setVariable("walk_amount", 1.0F);
    sheepMob.clearLayers();
    sheepMob.addLayer(*sheep.animations.find("animation.sheep.walk"), 0.5F);
    const SkeletonPose sheepPose = sheepMob.evaluate();
    // cos(180) * 30 = -30 degrees at t = 0.5, the quadruped walk cadence this
    // geometry's legs were sized for.
    assert(std::abs(sheepPose.bone(static_cast<std::size_t>(sheepFrontRight)).rotation.x + 30.0F) <
           1e-3F);
    assert(std::abs(sheepPose.bone(static_cast<std::size_t>(sheepFrontLeft)).rotation.x - 30.0F) <
           1e-3F);

    // AR-A1: Chicken. A small biped: body/head/two legs like the zombie's
    // lower half, plus two wing bones (a bird has none of the zombie's arms).
    // Structure only — see the sheep note above for what "待 mac" covers.
    const AnimatedModel chicken = loadAnimatedModel(
        kDir / "chicken.geo.json", {kDir / "chicken.animation.json"});
    // RN-1 (#2): 26.1 AdultChickenModel — head + beak + redThing (head children)
    // + body + 2 legs + 2 wings = 8 bones.
    assert(chicken.model.boneCount() == 8U);
    assert(chicken.animations.find("animation.chicken.walk") != nullptr);
    assert(chicken.animations.find("animation.chicken.idle") != nullptr);
    const int chickenBeak = chicken.model.findBone("beak");
    const int chickenRedThing = chicken.model.findBone("redThing");
    const int chickenHead = chicken.model.findBone("head");
    assert(chickenBeak >= 0 && chickenRedThing >= 0 && chickenHead >= 0);
    assert(chicken.model.bones()[static_cast<std::size_t>(chickenBeak)].parent == chickenHead);
    const int chickenRightLeg = chicken.model.findBone("rightLeg");
    const int chickenLeftLeg = chicken.model.findBone("leftLeg");
    const int chickenRightWing = chicken.model.findBone("rightWing");
    const int chickenLeftWing = chicken.model.findBone("leftWing");
    assert(chickenRightLeg >= 0 && chickenLeftLeg >= 0 && chickenRightWing >= 0 &&
           chickenLeftWing >= 0);
    // 26.1 chicken legs share one un-mirrored builder (uv 26,0); neither mirrors.
    assert(std::abs(chicken.model.bones()[static_cast<std::size_t>(chickenRightLeg)].cubes[0].uv.x -
                    26.0F) < 1e-3F);
    assert(!chicken.model.bones()[static_cast<std::size_t>(chickenRightLeg)].cubes[0].mirror);
    assert(!chicken.model.bones()[static_cast<std::size_t>(chickenLeftLeg)].cubes[0].mirror);
    assert(!chicken.model.bones()[static_cast<std::size_t>(chickenLeftWing)].cubes[0].mirror);
    Animator chickenMob;
    chickenMob.setModel(&chicken.model);
    chickenMob.context().setVariable("walk_amount", 1.0F);
    chickenMob.clearLayers();
    chickenMob.addLayer(*chicken.animations.find("animation.chicken.walk"), 0.3F);
    const SkeletonPose chickenPose = chickenMob.evaluate();
    // At t = 0.3 of a 0.6s clip: cos(360 * 0.3 / 0.6) = cos(180) = -1, so
    // -25 degrees on the right leg and +25 on the left (anti-phase).
    assert(std::abs(chickenPose.bone(static_cast<std::size_t>(chickenRightLeg)).rotation.x +
                    25.0F) < 1e-3F);
    assert(std::abs(chickenPose.bone(static_cast<std::size_t>(chickenLeftLeg)).rotation.x -
                    25.0F) < 1e-3F);

    return 0;
}

#include "animation/AnimationClip.hpp"
#include "animation/Animator.hpp"
#include "animation/SkeletalModel.hpp"

#include <cassert>
#include <cmath>
#include <glm/geometric.hpp>

using namespace mc::animation;

namespace {

constexpr const char* kGeometry = R"({
  "format_version": "1.12.0",
  "minecraft:geometry": [
    { "description": {"identifier":"geometry.test","texture_width":64,"texture_height":64},
      "bones": [
        {"name":"body","pivot":[0,18,0],"cubes":[{"origin":[-4,12,-2],"size":[8,12,4],"uv":[16,16]}]},
        {"name":"head","parent":"body","pivot":[0,24,0],"cubes":[{"origin":[-4,24,-4],"size":[8,8,8],"uv":[0,0]}]}
      ]
    }
  ]
})";

constexpr const char* kAnimation = R"({
  "format_version":"1.8.0",
  "animations": {
    "animation.test.walk": {
      "loop": true,
      "animation_length": 1.0,
      "bones": {
        "body": { "rotation": [0, "math.sin(query.anim_time * 360) * 10", 0] },
        "head": { "rotation": {"0.0":[0,0,0], "0.5":[45,0,0], "1.0":[0,0,0]} }
      }
    }
  }
})";

} // namespace

int main() {
    const SkeletalModel model = SkeletalModel::parse(kGeometry);
    assert(model.boneCount() == 2U);
    assert(model.textureWidth() == 64);
    const int body = model.findBone("body");
    const int head = model.findBone("head");
    assert(body >= 0 && head >= 0);
    assert(model.bones()[static_cast<std::size_t>(head)].parent == body);

    const AnimationLibrary library = AnimationLibrary::parse(kAnimation);
    const AnimationClip* walk = library.find("animation.test.walk");
    assert(walk != nullptr);
    assert(walk->loops());
    assert(std::abs(walk->length() - 1.0F) < 1e-5F);
    assert(std::abs(walk->localTime(1.25F) - 0.25F) < 1e-5F); // loop wrap

    Animator animator;
    animator.setModel(&model);
    animator.playSingle(*walk, 0.25F);
    const SkeletonPose pose = animator.evaluate();

    // Molang channel: body yaw = sin(90) * 10 = 10 degrees.
    assert(std::abs(pose.bone(static_cast<std::size_t>(body)).rotation.y - 10.0F) < 1e-3F);
    // Keyframed channel: head pitch lerps 0 -> 45 across the first half, sampled
    // at t=0.25 (halfway) = 22.5 degrees.
    assert(std::abs(pose.bone(static_cast<std::size_t>(head)).rotation.x - 22.5F) < 1e-3F);

    // Hierarchy: rotating the body moves the child head's world position.
    Animator rest;
    rest.setModel(&model);
    rest.playSingle(*walk, 0.0F);
    const glm::mat4 headRest = rest.evaluate().worldMatrix(head);
    const glm::mat4 headNow = pose.worldMatrix(head);
    const glm::vec4 corner{4.0F, 24.0F, 4.0F, 1.0F};
    assert(glm::length(glm::vec3(headRest * corner - headNow * corner)) > 0.1F);

    // A clip that targets a bone the geometry lacks is ignored, not fatal.
    const AnimationLibrary orphan = AnimationLibrary::parse(R"({
      "animations": {"a": {"animation_length": 1.0, "bones": {"ghost": {"rotation": [90,0,0]}}}}
    })");
    Animator orphanAnimator;
    orphanAnimator.setModel(&model);
    orphanAnimator.playSingle(*orphan.find("a"), 0.5F);
    const SkeletonPose orphanPose = orphanAnimator.evaluate();
    assert(std::abs(orphanPose.bone(static_cast<std::size_t>(body)).rotation.x) < 1e-6F);

    return 0;
}

#include "animation/ModelAnimationSystem.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <glm/mat4x4.hpp>

int main() {
    mc::animation::ModelAnimationSystem animation;
    assert(!animation.active());

    // The pose is a pure function of (action, progress): halfway through the
    // break clip the arm is at its swing peak.
    animation.setAction(mc::animation::ModelAction::Break, 0.5F);
    assert(animation.active());
    assert(animation.pose().translation.y < -0.10F);
    assert(animation.pose().rotationDegrees.y < -30.0F);
    assert(animation.pose().swingProgress >= 0.5F);
    const glm::mat4 swingingArm = mc::animation::firstPersonArmTransform(animation.pose());
    const glm::mat4 restingArm = mc::animation::firstPersonArmTransform({});
    assert(std::abs(swingingArm[3].x - restingArm[3].x) > 0.01F ||
           std::abs(swingingArm[3].y - restingArm[3].y) > 0.01F);

    // The clip is non-looping: at progress 1 the swing is back at rest.
    animation.setAction(mc::animation::ModelAction::Break, 1.0F);
    assert(std::abs(animation.pose().scale - 1.0F) < 0.0001F);
    assert(std::abs(animation.pose().translation.x) < 0.0001F);
    assert(std::abs(animation.pose().rotationDegrees.y) < 0.0001F);

    // The Use clip is the smaller placement arc.
    animation.setAction(mc::animation::ModelAction::Use, 0.5F);
    assert(animation.pose().translation.y < -0.05F);
    const glm::mat4 blockItem = mc::animation::firstPersonItemTransform(animation.pose(), true);
    const glm::mat4 flatItem = mc::animation::firstPersonItemTransform(animation.pose(), false);
    assert(std::abs(blockItem[0][0] - flatItem[0][0]) > 0.01F);

    // Progress at 0 is the resting pose, at 1 the clip's end.
    animation.setAction(mc::animation::ModelAction::Use, 0.0F);
    assert(animation.pose().swingProgress == 0.0F);
    animation.setAction(mc::animation::ModelAction::Use, 1.0F);
    assert(animation.pose().swingProgress == 1.0F);

    animation.setAction(mc::animation::ModelAction::None, 0.0F);
    assert(!animation.active());
    assert(std::abs(animation.pose().scale - 1.0F) < 0.0001F);

    // Eating reproduces HeldItemRenderer#applyEatOrDrinkTransformation. At the
    // start of the meal (progress 0) the food sits in the hand.
    animation.setAction(mc::animation::ModelAction::Eat, 0.0F);
    assert(std::abs(animation.pose().translation.x) < 0.02F);
    assert(std::abs(animation.pose().rotationDegrees.y) < 2.0F);

    // Four ticks into the 32-tick meal (progress 0.125, anim_time 0.2 s) the
    // lift is essentially complete: right 0.6, down 0.5, and the Y 90 / Z 10 /
    // X 30 tilt that turns the food toward the camera.
    animation.setAction(mc::animation::ModelAction::Eat, 0.125F);
    {
        const auto& lifted = animation.pose();
        assert(lifted.translation.x > 0.55F);
        assert(lifted.translation.y < -0.4F);
        assert(lifted.rotationDegrees.y > 85.0F);
        assert(std::abs(lifted.rotationDegrees.z - 10.0F) < 1.0F);
        assert(std::abs(lifted.rotationDegrees.x - 30.0F) < 2.0F);
    }

    // The progress is bounded by the caller: at the end of the meal the food is
    // at the mouth, held there because the caller keeps sampling progress 1
    // while the meal is active.
    animation.setAction(mc::animation::ModelAction::Eat, 1.0F);
    assert(animation.active());
    assert(animation.pose().translation.x > 0.55F);
    assert(animation.pose().rotationDegrees.y > 85.0F);

    // The composed transform is vanilla's stack: translate(0.6, -0.5 + bob, 0),
    // then Y 90 / Z 10 / X 30, then applyEquipOffset(0.56, -0.52, -0.72). At
    // progress 1 the bob term is gone, which puts the item origin here.
    const glm::mat4 eating = mc::animation::firstPersonEatTransform(animation.pose(), true);
    assert(std::abs(eating[3].x - -0.2835F) < 0.002F);
    assert(std::abs(eating[3].y - -0.3917F) < 0.002F);
    assert(std::abs(eating[3].z - -0.5672F) < 0.002F);
    // Which is up and across from the resting hand: the food ends up in front of
    // the face rather than out at the edge of the screen where it is held.
    const glm::mat4 resting = mc::animation::firstPersonItemTransform({}, true);
    assert(resting[3].x - eating[3].x > 0.5F);
    assert(eating[3].y > resting[3].y);

    animation.setAction(mc::animation::ModelAction::None, 0.0F);
    assert(!animation.active());
    return 0;
}

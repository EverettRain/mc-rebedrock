#include "animation/ModelAnimationSystem.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <glm/mat4x4.hpp>

int main() {
    mc::animation::ModelAnimationSystem animation;
    assert(!animation.active());
    animation.trigger(mc::animation::ModelAction::Break);
    animation.update(0.15F);
    assert(animation.active());
    assert(animation.pose().translation.y < -0.10F);
    assert(animation.pose().rotationDegrees.y < -30.0F);
    assert(animation.pose().swingProgress >= 0.5F);
    const glm::mat4 swingingArm = mc::animation::firstPersonArmTransform(animation.pose());
    const glm::mat4 restingArm = mc::animation::firstPersonArmTransform({});
    assert(std::abs(swingingArm[3].x - restingArm[3].x) > 0.01F ||
           std::abs(swingingArm[3].y - restingArm[3].y) > 0.01F);
    animation.update(0.16F);
    assert(!animation.active());
    assert(std::abs(animation.pose().scale - 1.0F) < 0.0001F);

    animation.trigger(mc::animation::ModelAction::Use);
    animation.update(0.15F);
    assert(animation.pose().translation.y < -0.05F);
    const glm::mat4 blockItem = mc::animation::firstPersonItemTransform(animation.pose(), true);
    const glm::mat4 flatItem = mc::animation::firstPersonItemTransform(animation.pose(), false);
    assert(std::abs(blockItem[0][0] - flatItem[0][0]) > 0.01F);
    animation.trigger(mc::animation::ModelAction::None);
    assert(!animation.active());

    // LivingEntity#swing: retriggering the same action before the arc is half
    // done keeps the current swing, so a caller that triggers every frame during
    // an ongoing dig gets the vanilla rhythm instead of a frozen arm.
    animation.trigger(mc::animation::ModelAction::Break);
    animation.update(0.1F);
    animation.trigger(mc::animation::ModelAction::Break);
    assert(animation.pose().swingProgress > 0.3F);
    // Past halfway the swing restarts, which is what produces the repeat cadence.
    animation.update(0.06F);
    animation.trigger(mc::animation::ModelAction::Break);
    animation.update(0.01F);
    assert(animation.pose().swingProgress < 0.2F);
    // A different action always takes over immediately.
    animation.update(0.1F);
    animation.trigger(mc::animation::ModelAction::Use);
    animation.update(0.01F);
    assert(animation.pose().swingProgress < 0.2F);

    // Driving the swing every frame for several clip lengths keeps it running
    // rather than letting it lapse back to the resting pose.
    bool sawRestart = false;
    float previousProgress = animation.pose().swingProgress;
    for (int frame = 0; frame < 120; ++frame) {
        animation.trigger(mc::animation::ModelAction::Use);
        animation.update(1.0F / 60.0F);
        assert(animation.active());
        const float progress = animation.pose().swingProgress;
        sawRestart = sawRestart || progress < previousProgress;
        previousProgress = progress;
    }
    assert(sawRestart);

    // Eating reproduces HeldItemRenderer#applyEatOrDrinkTransformation.
    animation.trigger(mc::animation::ModelAction::None);
    animation.trigger(mc::animation::ModelAction::Eat);
    animation.update(0.001F);
    // `h = 1 - g^27` is still ~0 at the start, so the food sits in the hand.
    assert(std::abs(animation.pose().translation.x) < 0.02F);
    assert(std::abs(animation.pose().rotationDegrees.y) < 2.0F);

    // Four ticks in, the lift is essentially complete: right 0.6, down 0.5, and
    // the Y 90 / Z 10 / X 30 tilt that turns the food toward the camera.
    animation.update(0.2F - 0.001F);
    {
        const auto& lifted = animation.pose();
        assert(lifted.translation.x > 0.55F);
        assert(lifted.translation.y < -0.4F);
        assert(lifted.rotationDegrees.y > 85.0F);
        assert(std::abs(lifted.rotationDegrees.z - 10.0F) < 1.0F);
        assert(std::abs(lifted.rotationDegrees.x - 30.0F) < 2.0F);
    }

    // Past seven ticks the four-tick chewing bob rides on top of the lift, so
    // the height keeps changing in both directions instead of holding still.
    float minimumY = 1.0F;
    float maximumY = -1.0F;
    for (int frame = 0; frame < 40; ++frame) {
        animation.update(0.01F);
        minimumY = std::min(minimumY, animation.pose().translation.y);
        maximumY = std::max(maximumY, animation.pose().translation.y);
    }
    assert(maximumY - minimumY > 0.05F);

    // The eat clip is held by gameplay, not by its own length: running past 1.6 s
    // keeps the food at the mouth rather than dropping it back to the hand.
    for (int frame = 0; frame < 60; ++frame) {
        animation.update(0.02F);
    }
    assert(animation.active());
    assert(animation.pose().translation.x > 0.55F);
    assert(animation.pose().rotationDegrees.y > 85.0F);

    // The composed transform is vanilla's stack: translate(0.6, -0.5 + bob, 0),
    // then Y 90 / Z 10 / X 30, then applyEquipOffset(0.56, -0.52, -0.72). At this
    // point the bob is at its peak (+0.1), which puts the item origin here.
    const glm::mat4 eating = mc::animation::firstPersonEatTransform(animation.pose(), true);
    assert(std::abs(eating[3].x - -0.2835F) < 0.002F);
    assert(std::abs(eating[3].y - -0.3917F) < 0.002F);
    assert(std::abs(eating[3].z - -0.5672F) < 0.002F);
    // Which is up and across from the resting hand: the food ends up in front of
    // the face rather than out at the edge of the screen where it is held.
    const glm::mat4 resting = mc::animation::firstPersonItemTransform({}, true);
    assert(resting[3].x - eating[3].x > 0.5F);
    assert(eating[3].y > resting[3].y);

    animation.trigger(mc::animation::ModelAction::None);
    assert(!animation.active());
    return 0;
}

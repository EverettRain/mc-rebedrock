// ANIM-4: the player's animation is now layered through the ANIM-1 masks,
// ANIM-2 override/crossfade and the ANIM-3 locomotion controller. These headless
// assertions pin the separation the layering guarantees (walking + holding an
// item, sneaking + looking) and that the state weights come from the controller/
// Transition rather than hand-eased C++ weights. The actual on-screen look stays
// a mac visual check.

#include "animation/PlayerModelAnimator.hpp"
#include "animation/SkeletalModel.hpp"

#include <cassert>
#include <cmath>
#include <string>

using namespace mc::animation;

namespace {

float legPitch(const PlayerModelAnimator& a) { return a.pose().rightLegPitch; }
float rightArmPitch(const PlayerModelAnimator& a) { return a.pose().rightArmPitch; }

// Settle an animator into a steady walking / sneaking state by ticking a while.
void settle(PlayerModelAnimator& a, bool walking, bool sneaking) {
    for (int i = 0; i < 40; ++i) {
        a.update(0.05F, walking, sneaking);
    }
}

} // namespace

int main() {
    // ---- Controller drives locomotion state (idle -> walk -> idle) -----------
    {
        PlayerModelAnimator a;
        a.update(0.05F, /*walking=*/false);
        assert(a.locomotionState() == "idle");
        settle(a, /*walking=*/true, false);
        assert(a.locomotionState() == "walk");
        // Walking swings the legs (locomotion clip on the lower body).
        assert(std::abs(legPitch(a)) > 1e-3F);
        settle(a, /*walking=*/false, false);
        assert(a.locomotionState() == "idle");
    }

    // ---- Crossfade is a ramp, not a snap (ANIM-2 Transition, no hand ease) ----
    {
        PlayerModelAnimator a;
        settle(a, /*walking=*/false, false); // idle
        assert(!a.locomotionTransitioning());
        // Entering walk starts a crossfade that stays in flight across several
        // short frames (walk blend_transition ~0.1s), proving the state blend
        // eases over time instead of snapping to full on the first frame.
        a.update(0.02F, /*walking=*/true, false);
        assert(a.locomotionState() == "walk");
        assert(a.locomotionTransitioning()); // still ramping, not snapped
        // It settles once enough time passes.
        settle(a, /*walking=*/true, false);
        assert(!a.locomotionTransitioning());
        assert(std::abs(legPitch(a)) > 1e-3F);
    }

    // ---- Separation (★): walking + item-hold override -----------------------
    // The legs keep striding while the arms take the item pose. Crucially the arm
    // must equal the item pose, NOT the item pose plus the walk arm swing.
    {
        const float holdPitch = -55.0F;

        PlayerModelAnimator walkingHold;
        walkingHold.setItemHold(true, holdPitch);
        settle(walkingHold, /*walking=*/true, false);

        // Legs still swing (lower body untouched by the arm override).
        assert(std::abs(legPitch(walkingHold)) > 1e-3F);

        // The arm equals the held pose regardless of the gait: an override masked
        // to the arms replaces the swing instead of summing with it. Compare to a
        // still player holding the same item -- the arm pitch must match.
        PlayerModelAnimator stillHold;
        stillHold.setItemHold(true, holdPitch);
        settle(stillHold, /*walking=*/false, false);

        const float radHold = holdPitch * 3.14159265358979F / 180.0F;
        assert(std::abs(rightArmPitch(walkingHold) - radHold) < 1e-3F);
        assert(std::abs(rightArmPitch(stillHold) - radHold) < 1e-3F);
        // And walking with the item did not bend the held pose by the swing.
        assert(std::abs(rightArmPitch(walkingHold) - rightArmPitch(stillHold)) < 1e-3F);
    }

    // Without the item hold, walking DOES swing the arm (regression guard: the
    // override only applies when holding).
    {
        PlayerModelAnimator a;
        settle(a, /*walking=*/true, false);
        assert(std::abs(rightArmPitch(a)) > 1e-3F);
    }

    // ---- Separation (★): sneaking + head-look independence -------------------
    // The sneak state leans the torso; the head-look drives the head. Changing
    // the look must move the head without disturbing the sneaking torso lean.
    {
        PlayerModelAnimator lookLeft;
        lookLeft.setCursorLook(-1.0F, 0.0F);
        settle(lookLeft, /*walking=*/false, /*sneaking=*/true);
        assert(lookLeft.locomotionState() == "sneak");
        const float bodyYawLeft = lookLeft.pose().bodyYaw;
        const float headYawLeft = lookLeft.pose().headYaw;

        PlayerModelAnimator lookRight;
        lookRight.setCursorLook(1.0F, 0.0F);
        settle(lookRight, /*walking=*/false, /*sneaking=*/true);
        const float headYawRight = lookRight.pose().headYaw;
        const float bodyYawRight = lookRight.pose().bodyYaw;

        // The head follows the look (opposite signs for opposite look).
        assert(headYawLeft * headYawRight < 0.0F);
        // The sneaking torso lean (bodyYaw, driven by the sneak state, not look)
        // is unaffected by the look direction: body_look_amount is 0 for the
        // world player by default, so the body yaw stays put regardless of look.
        assert(std::abs(bodyYawLeft - bodyYawRight) < 1e-4F);
    }

    // ---- head-look does not disturb the legs (head mask) ---------------------
    {
        PlayerModelAnimator noLook;
        noLook.setCursorLook(0.0F, 0.0F);
        settle(noLook, /*walking=*/true, false);
        const float legNoLook = legPitch(noLook);

        PlayerModelAnimator look;
        look.setCursorLook(1.0F, 1.0F);
        settle(look, /*walking=*/true, false);
        // The leg swing phase is time-driven; both settled at the same cadence, so
        // the head look must not have shifted the legs.
        assert(std::abs(legPitch(look) - legNoLook) < 1e-3F);
    }

    return 0;
}

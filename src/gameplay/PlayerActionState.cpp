#include "gameplay/PlayerActionState.hpp"

#include <algorithm>
#include <cstdint>

namespace mc::gameplay {

void PlayerActionState::swingHand(InteractionHand hand, SwingAnimation animation,
                                  std::uint32_t durationTicks) {
    // LivingEntity#swing only restarts the arc once it is past halfway, which
    // is the vanilla cadence for a held dig — a mid-swing retrigger keeps the
    // arm moving instead of snapping back to the start.
    if (swing.active && swing.progress < 0.5F) {
        return;
    }
    swing = SwingState{};
    swing.hand = hand;
    swing.animation = animation;
    swing.sequence = ++swingSequence_;
    swing.startedTick = serverTick_;
    swing.durationTicks = durationTicks > 0U ? durationTicks : 6U;
    swing.active = true;
    swing.previousProgress = 0.0F;
    swing.progress = 0.0F;
}

bool PlayerActionState::startUsing(InteractionHand hand, UseAnimation animation,
                                   std::uint32_t durationTicks) {
    if (use.active) {
        return false;
    }
    use = ItemUseState{};
    use.hand = hand;
    use.animation = animation;
    use.startedTick = serverTick_;
    use.durationTicks = durationTicks;
    use.remainingTicks = durationTicks;
    use.previousRemainingTicks = durationTicks;
    use.active = durationTicks > 0U;
    return use.active;
}

void PlayerActionState::stopUsing() {
    use = ItemUseState{};
}

void PlayerActionState::tick() {
    ++serverTick_;
    if (swing.active) {
        ++swing.elapsedTicks;
        swing.previousProgress = swing.progress;
        swing.progress = swing.durationTicks > 0U
                             ? std::min(1.0F, static_cast<float>(swing.elapsedTicks) /
                                                  static_cast<float>(swing.durationTicks))
                             : 1.0F;
        if (swing.elapsedTicks >= swing.durationTicks) {
            swing.active = false;
        }
    }
    if (use.active) {
        use.previousRemainingTicks = use.remainingTicks;
        if (use.remainingTicks > 0U) {
            --use.remainingTicks;
        }
        if (use.remainingTicks == 0U) {
            use.active = false;
        }
    }
}

} // namespace mc::gameplay

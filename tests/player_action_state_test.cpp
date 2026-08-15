// The PlayerActionState timeline is the N1 foundation: a swing and an item use
// advance once per tick, so the same operation consumes the same ticks at any
// frame rate. These tests pin the swing-restart rule (halfway), the sequence
// counter, and the use countdown.

#include "gameplay/PlayerActionState.hpp"

#include <cassert>
#include <cstdint>

using namespace mc;

int main() {
    // A swing completes in exactly durationTicks ticks, one tick per call.
    {
        gameplay::PlayerActionState state;
        state.swingHand(gameplay::InteractionHand::Main, gameplay::SwingAnimation::Break, 6U);
        assert(state.swing.active);
        assert(state.swing.sequence == 1U);
        for (int tick = 0; tick < 6; ++tick) {
            assert(state.swing.active);  // still swinging through tick 5
            state.tick();
        }
        assert(!state.swing.active);
        assert(state.swing.progress == 1.0F);
        // progress is tick-driven: six ticks, exactly six steps, independent of
        // how many frames each tick spanned.
        assert(state.swing.elapsedTicks == 6U);
    }

    // A retrigger before the arc is halfway does not restart it (the vanilla
    // cadence for a held dig), and the sequence is unchanged.
    {
        gameplay::PlayerActionState state;
        state.swingHand(gameplay::InteractionHand::Main, gameplay::SwingAnimation::Break, 6U);
        const auto sequence = state.swing.sequence;
        state.tick();  // progress 1/6
        state.swingHand(gameplay::InteractionHand::Main, gameplay::SwingAnimation::Break, 6U);
        assert(state.swing.sequence == sequence);  // not restarted
        state.tick();                               // 2/6
        state.tick();                               // 3/6 == halfway
        assert(state.swing.progress >= 0.5F);
        state.swingHand(gameplay::InteractionHand::Main, gameplay::SwingAnimation::Break, 6U);
        assert(state.swing.sequence == sequence + 1U);  // restarted past halfway
        assert(state.swing.elapsedTicks == 0U);
        assert(state.swing.progress == 0.0F);
    }

    // The sequence distinguishes two consecutive completed swings.
    {
        gameplay::PlayerActionState state;
        state.swingHand(gameplay::InteractionHand::Main, gameplay::SwingAnimation::Break, 6U);
        const auto first = state.swing.sequence;
        for (int tick = 0; tick < 6; ++tick) {
            state.tick();
        }
        assert(!state.swing.active);
        state.swingHand(gameplay::InteractionHand::Main, gameplay::SwingAnimation::Break, 6U);
        assert(state.swing.sequence == first + 1U);
    }

    // An item use counts down in whole ticks and finishes itself; a second
    // start while active is refused, and stopUsing cancels.
    {
        gameplay::PlayerActionState state;
        assert(state.startUsing(gameplay::InteractionHand::Main, gameplay::UseAnimation::Eat, 32U));
        // An already-active use does not restart.
        assert(!state.startUsing(gameplay::InteractionHand::Main, gameplay::UseAnimation::Eat,
                                 32U));
        assert(state.use.remainingTicks == 32U);
        for (int tick = 0; tick < 31; ++tick) {
            state.tick();
        }
        assert(state.use.active);
        assert(state.use.remainingTicks == 1U);
        state.tick();
        assert(!state.use.active);  // finished on the 32nd tick
        assert(state.use.remainingTicks == 0U);
    }
    {
        gameplay::PlayerActionState state;
        assert(state.startUsing(gameplay::InteractionHand::Main, gameplay::UseAnimation::Eat, 32U));
        state.stopUsing();
        assert(!state.use.active);
        assert(state.use.remainingTicks == 0U);
        // stopUsing frees the slot: a fresh use can start.
        assert(state.startUsing(gameplay::InteractionHand::Main, gameplay::UseAnimation::Eat, 8U));
        assert(state.use.remainingTicks == 8U);
    }

    // The server tick advances once per tick() call.
    {
        gameplay::PlayerActionState state;
        assert(state.serverTick() == 0U);
        state.tick();
        assert(state.serverTick() == 1U);
    }

    return 0;
}

// N3a's extractor: the frame's player pose interpolates the per-tick player
// snapshot against the frame's partial tick. Within one swing the progress
// rises smoothly across tick boundaries; a restart (sequence change) snaps
// instead of replaying the arm back from the apex; and the same endpoints give
// the same pose at any frame rate.

#include "render/player/PlayerRenderState.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <optional>

using namespace mc;

int main() {
    // A swing at its tick endpoint, interpolated halfway into the next frame,
    // sits between the previous and current progress.
    {
        gameplay::SwingState swing;
        swing.active = true;
        swing.animation = gameplay::SwingAnimation::Break;
        swing.sequence = 7U;
        swing.durationTicks = 6U;
        swing.elapsedTicks = 2U;
        swing.previousProgress = 2.0F / 6.0F;
        swing.progress = 3.0F / 6.0F;
        std::optional<std::uint64_t> last{7U};
        const auto frame = render::player::interpolateSwing(swing, 0.5F, last);
        assert(frame.active);
        assert(frame.sequence == 7U);
        assert(std::abs(frame.progress - 2.5F / 6.0F) < 0.0001F);
    }

    // Cross-tick continuity: the same sequence sampled at alpha 0.95 (late in
    // one tick) and alpha 0.05 (just after the next tick committed) must not
    // rewind. The swing advances each tick, so the display only rises.
    {
        gameplay::SwingState firstTick;
        firstTick.active = true;
        firstTick.sequence = 7U;
        firstTick.durationTicks = 6U;
        firstTick.previousProgress = 2.0F / 6.0F;
        firstTick.progress = 3.0F / 6.0F;
        std::optional<std::uint64_t> last;
        const float lateFrame =
            render::player::interpolateSwing(firstTick, 0.95F, last).progress;

        // The next tick advances the same swing (same sequence).
        gameplay::SwingState secondTick = firstTick;
        secondTick.previousProgress = firstTick.progress;
        secondTick.progress = 4.0F / 6.0F;
        secondTick.elapsedTicks = 3U;
        const float earlyNextFrame =
            render::player::interpolateSwing(secondTick, 0.05F, last).progress;
        assert(lateFrame > 2.0F / 6.0F);
        assert(earlyNextFrame > lateFrame);  // no rewind
    }

    // A sequence change is a NEW action: snap to the new swing's start, never
    // lerp from the previous apex back to 0 (that would replay the arm).
    {
        gameplay::SwingState before;
        before.active = true;
        before.sequence = 7U;
        before.previousProgress = 2.0F / 6.0F;
        before.progress = 3.0F / 6.0F;
        std::optional<std::uint64_t> last{7U};
        // Consume the old swing so last tracks sequence 7 (the interpolated pose
        // itself is irrelevant here — the call advances `last`).
        static_cast<void>(render::player::interpolateSwing(before, 0.5F, last));

        gameplay::SwingState restarted;
        restarted.active = true;
        restarted.sequence = 8U;  // the restart bumped the sequence
        restarted.previousProgress = 0.0F;
        restarted.progress = 0.0F;
        const auto frame = render::player::interpolateSwing(restarted, 0.5F, last);
        assert(frame.sequence == 8U);
        // Snapped to the new swing's value (0), not lerped from 0.5.
        assert(std::abs(frame.progress - 0.0F) < 0.0001F);
        assert(last.has_value() && *last == 8U);
    }

    // An ended swing yields the completion endpoint (rest).
    {
        gameplay::SwingState done;
        done.active = false;
        std::optional<std::uint64_t> last{7U};
        const auto frame = render::player::interpolateSwing(done, 0.5F, last);
        assert(!frame.active);
        assert(frame.progress == 1.0F);
        assert(!last.has_value());
    }

    // The use countdown's elapsed fraction rises smoothly across the boundary.
    {
        gameplay::ItemUseState use;
        use.active = true;
        use.animation = gameplay::UseAnimation::Eat;
        use.durationTicks = 32U;
        use.remainingTicks = 30U;
        use.previousRemainingTicks = 31U;
        const auto frame = render::player::interpolateUse(use, 0.5F);
        assert(frame.active);
        assert(std::abs(frame.progress - 1.5F / 32.0F) < 0.0001F);
    }

    // ANIM A1/A2: the extractor reads the vanilla WalkAnimationState directly —
    // walkStride is the interpolated phase (walkPosition), walkSpeed the
    // interpolated amplitude (walkAmount). No bob-derived hack, no sprint scaling.
    {
        gameplay::PlayerTickSnapshot snapshot;
        snapshot.physicsPrevious = glm::vec3{1.0F, 2.0F, 3.0F};
        snapshot.physicsCurrent = glm::vec3{5.0F, 6.0F, 7.0F};
        snapshot.previousWalkPosition = 4.0F;
        snapshot.walkPosition = 8.0F;
        snapshot.previousWalkAmount = 0.30F;
        snapshot.walkAmount = 0.50F;
        snapshot.sneaking = true;
        std::optional<std::uint64_t> last;
        const auto state = render::player::extractPlayerRenderState(snapshot, 0.5F, last);
        assert(std::abs(state.feetPosition.x - 3.0F) < 0.0001F);
        assert(std::abs(state.walkStride - 6.0F) < 0.0001F);   // phase lerp: 4->8 @0.5
        assert(std::abs(state.walkSpeed - 0.40F) < 0.0001F);   // amount lerp: 0.3->0.5 @0.5
        assert(state.sneaking);
    }

    // The amplitude is clamped to [0, 1] (a saturated snapshot never over-swings).
    {
        gameplay::PlayerTickSnapshot snapshot;
        snapshot.previousWalkAmount = 1.0F;
        snapshot.walkAmount = 1.0F;
        std::optional<std::uint64_t> last;
        const auto state = render::player::extractPlayerRenderState(snapshot, 0.5F, last);
        assert(std::abs(state.walkSpeed - 1.0F) < 0.0001F);
    }

    // FPS-independence: the same tick endpoints + partialTicks give the same
    // pose no matter how many frames sat between ticks (no frame-time input).
    {
        gameplay::SwingState swing;
        swing.active = true;
        swing.sequence = 7U;
        swing.previousProgress = 2.0F / 6.0F;
        swing.progress = 3.0F / 6.0F;
        std::optional<std::uint64_t> a{7U}, b{7U};
        assert(std::abs(render::player::interpolateSwing(swing, 0.5F, a).progress -
                        render::player::interpolateSwing(swing, 0.5F, b).progress) < 0.0001F);
    }

    return 0;
}

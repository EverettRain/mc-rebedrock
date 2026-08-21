#include "gameplay/RedstoneTorch.hpp"

#include "world/Block.hpp"
#include "world/BlockState.hpp"

#include <cstdio>
#include <cstdlib>

// W-4 slice 2: the redstone torch's timing decisions and its deterministic
// burnout, pinned against RedstoneTorchBlock. These are the "肉眼难察" parts —
// the 2gt inversion trigger and the 8-off-toggles-in-60gt burnout — verified as
// pure decisions, ahead of the scheduler/harness plumbing that drives them.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "redstone_torch_test line %d failed: %s\n", line, expression);
        std::abort();
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using namespace mc::gameplay;
using mc::world::Block;
using mc::world::BlockPos;
using mc::world::BlockState;

const BlockState kLit = BlockState{Block::RedstoneTorch}.withLit(true);
const BlockState kUnlit = BlockState{Block::RedstoneTorch}.withLit(false);

// --- neighborChanged: schedule a toggle exactly when LIT already matches the
//     input signal (the "needs to flip" states), and never twice. ---
void testScheduleDecision() {
    // Lit and powered -> should go out: schedule.
    REQUIRE(redstone::torchShouldScheduleToggle(kLit, true, false));
    // Unlit and unpowered -> should relight: schedule.
    REQUIRE(redstone::torchShouldScheduleToggle(kUnlit, false, false));
    // Lit and unpowered -> already correct: do nothing.
    REQUIRE(!redstone::torchShouldScheduleToggle(kLit, false, false));
    // Unlit and powered -> already correct: do nothing.
    REQUIRE(!redstone::torchShouldScheduleToggle(kUnlit, true, false));
    // Dedup: a tick is already pending for this cell, so re-notifying is a no-op.
    REQUIRE(!redstone::torchShouldScheduleToggle(kLit, true, true));
}

// --- tick: apply the inversion. This is what produces the torch-inverter's
//     false@t=2 / true@t=7 once the scheduler delivers the tick 2gt later. ---
void testTickDecision() {
    redstone::TorchBurnoutTracker tracker;
    const BlockPos pos{0, 1, 0};

    // Lit + powered -> goes out.
    auto off = redstone::torchTick(kLit, true, pos, 2, tracker);
    REQUIRE(off.changed);
    REQUIRE(!off.newState.lit());
    REQUIRE(!off.burnedOut); // first off-toggle, far from the burnout threshold

    // Unlit + unpowered -> relights (fresh tracker: not too frequent).
    redstone::TorchBurnoutTracker fresh;
    auto on = redstone::torchTick(kUnlit, false, pos, 7, fresh);
    REQUIRE(on.changed);
    REQUIRE(on.newState.lit());

    // Lit + unpowered and Unlit + powered: no change (nothing to apply).
    REQUIRE(!redstone::torchTick(kLit, false, pos, 3, fresh).changed);
    REQUIRE(!redstone::torchTick(kUnlit, true, pos, 3, fresh).changed);
}

// --- burnout: a 1-torch feedback loop flips off every 4gt (off events at
//     t=2,6,10,...). Only off transitions count. The 8th (t=30) trips burnout;
//     the earlier seven do not. After the window ages out (t=190) the torch may
//     relight. RedstoneTorchBlock.tick:90 / torch-inverter.md scenario B. ---
void testBurnout() {
    redstone::TorchBurnoutTracker tracker;
    const BlockPos pos{0, 1, 0};

    bool burnedOut = false;
    int offEvents = 0;
    for (std::int64_t t = 2; t <= 30; t += 4) {
        tracker.prune(t);
        const auto result = redstone::torchTick(kLit, true, pos, t, tracker);
        REQUIRE(result.changed); // every off event flips it out
        ++offEvents;
        if (offEvents < 8) {
            REQUIRE(!result.burnedOut); // events 1..7 are under the threshold
        } else {
            burnedOut = result.burnedOut; // the 8th, at t=30
        }
    }
    REQUIRE(offEvents == 8);
    REQUIRE(burnedOut); // 8 off-toggles in the 60gt window -> burned out at t=30

    // While burned out, an unpowered unlit torch refuses to relight: the eight
    // toggles are still inside the window.
    tracker.prune(32);
    REQUIRE(tracker.isTooFrequent(pos));
    REQUIRE(!redstone::torchTick(kUnlit, false, pos, 32, tracker).changed);

    // At t=190 (30 + RESTART_DELAY 160) every toggle is >60gt old and pruned, so
    // the torch relights and the oscillation restarts.
    tracker.prune(190);
    REQUIRE(tracker.size() == 0U);
    REQUIRE(!tracker.isTooFrequent(pos));
    const auto restart = redstone::torchTick(kUnlit, false, pos, 190, tracker);
    REQUIRE(restart.changed);
    REQUIRE(restart.newState.lit());
}

} // namespace

int main() {
    testScheduleDecision();
    testTickDecision();
    testBurnout();
    return 0;
}

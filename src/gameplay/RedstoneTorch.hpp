#pragma once

// The redstone torch's timing and burnout logic (W-4), source-derived from Java
// 26.1's RedstoneTorchBlock. Kept as pure decisions over (state, input signal,
// game time, toggle history) so the exact 2gt inversion and the deterministic
// burnout can be pinned in isolation — the scheduled-tick/block-update plumbing
// that feeds them (WorldSimulation drain + the neighbour updater) is a separate
// slice, but the correctness that is "肉眼难察" lives here and is testable here.
//
// Determinism: nothing below consults a random source. Burnout is a pure count
// over a fixed 60-gametick window — the one behaviour that "looks random" but is
// not — so a replay produces the identical torch state every time.

#include "world/BlockPos.hpp"
#include "world/BlockState.hpp"

#include <cstdint>
#include <vector>

namespace mc::gameplay::redstone {

// Constants, RedstoneTorchBlock:31-34.
inline constexpr int kTorchToggleDelay = 2;         // gt: input change -> apply after 2gt (1 redstone-tick)
inline constexpr std::int64_t kRecentToggleWindow = 60; // gt: burnout sliding window
inline constexpr int kMaxRecentToggles = 8;         // >=8 off-toggles in the window -> burnout
inline constexpr int kTorchRestartDelay = 160;      // gt: re-check delay after burning out

// The recent off-toggles, the burnout counter's state. Java keeps a
// WeakHashMap<Level, List<Toggle>>; this is the same list, packed: one entry per
// off-toggle as (packed pos, game time). Small and pruned every tick, so a plain
// vector is cheaper than a map. Shared across all torches in a world.
class TorchBurnoutTracker final {
  public:
    // Drop every recorded toggle older than the 60gt window. Java does this at
    // the top of RedstoneTorchBlock.tick before counting.
    void prune(std::int64_t gameTime) {
        std::size_t kept = 0;
        for (const Toggle& toggle : toggles_) {
            if (gameTime - toggle.when <= kRecentToggleWindow) {
                toggles_[kept++] = toggle;
            }
        }
        toggles_.resize(kept);
    }

    // isToggledTooFrequently(add=true): record this off-toggle, then report
    // whether the torch has now flipped off >=8 times inside the window. Only the
    // off transition records, RedstoneTorchBlock.tick:90.
    [[nodiscard]] bool recordOffAndCheck(world::BlockPos pos, std::int64_t gameTime) {
        toggles_.push_back({world::packBlockPos(pos), gameTime});
        return countFor(world::packBlockPos(pos)) >= kMaxRecentToggles;
    }

    // isToggledTooFrequently(add=false): the relight guard — count without
    // recording, RedstoneTorchBlock.tick:95.
    [[nodiscard]] bool isTooFrequent(world::BlockPos pos) const {
        return countFor(world::packBlockPos(pos)) >= kMaxRecentToggles;
    }

    [[nodiscard]] std::size_t size() const { return toggles_.size(); }
    void clear() { toggles_.clear(); }

  private:
    struct Toggle final {
        std::int64_t pos = 0;
        std::int64_t when = 0;
    };

    [[nodiscard]] int countFor(std::int64_t packedPos) const {
        int count = 0;
        for (const Toggle& toggle : toggles_) {
            if (toggle.pos == packedPos) {
                ++count;
            }
        }
        return count;
    }

    std::vector<Toggle> toggles_;
};

// neighborChanged: whether the torch should schedule a toggle tick. The input
// changed, and the torch's current LIT already equals its input's signal — so it
// is in the "needs to flip" state (LIT while powered, or unlit while unpowered).
// `alreadyScheduled` is the UNIQUE_TICK_HASH dedup guard (willTickThisTick):
// re-notifying before the pending tick fires must not queue a second one.
// RedstoneTorchBlock.neighborChanged:104-105.
[[nodiscard]] inline bool torchShouldScheduleToggle(world::BlockState torchState,
                                                    bool hasNeighborSignal, bool alreadyScheduled) {
    return torchState.lit() == hasNeighborSignal && !alreadyScheduled;
}

// The outcome of a torch's scheduled tick: whether the LIT state changed, the new
// state if so, and whether the off-toggle burned the torch out (which the caller
// turns into a smoke event and a +160gt re-check tick).
struct TorchTickResult final {
    bool changed = false;
    world::BlockState newState{};
    bool burnedOut = false;
};

// tick: apply the delayed inversion, RedstoneTorchBlock.tick:80-97. `tracker`
// must already have been pruned for `gameTime`. A lit torch that is powered goes
// out (and records a burnout toggle); an unlit torch that is unpowered relights,
// unless it has been toggling too fast.
[[nodiscard]] inline TorchTickResult torchTick(world::BlockState state, bool hasNeighborSignal,
                                               world::BlockPos pos, std::int64_t gameTime,
                                               TorchBurnoutTracker& tracker) {
    TorchTickResult result;
    if (state.lit()) {
        if (hasNeighborSignal) {
            result.changed = true;
            result.newState = state.withLit(false);
            result.burnedOut = tracker.recordOffAndCheck(pos, gameTime);
        }
    } else if (!hasNeighborSignal && !tracker.isTooFrequent(pos)) {
        result.changed = true;
        result.newState = state.withLit(true);
    }
    return result;
}

} // namespace mc::gameplay::redstone

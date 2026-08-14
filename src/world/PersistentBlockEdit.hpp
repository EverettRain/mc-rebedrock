#pragma once

#include "world/Block.hpp"
#include "world/BlockState.hpp"

namespace mc::world {

// One cell a player or the simulation changed away from what generation would
// produce. The world is a seed plus this list, so these are the records the
// save file is mostly made of.
//
// It carries the whole state rather than a block plus a fixed set of loose
// fields. The loose form could only name the properties it had columns for —
// block, fluid level, orientation, lit — which meant a crop's age travelled
// disguised as a direction, and a fifth property would have needed both a new
// field here and a new column on disk. Now the state is opaque and the save
// layer writes property *names*, so the two can gain properties independently.
struct PersistentBlockEdit final {
    int x = 0;
    int y = 0;
    int z = 0;
    BlockState state{};

    [[nodiscard]] bool operator==(const PersistentBlockEdit&) const = default;
};

} // namespace mc::world

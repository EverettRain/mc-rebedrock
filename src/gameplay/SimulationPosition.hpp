#pragma once

// The block coordinate the simulation schedules work against.
//
// Split out of WorldSimulation.hpp so ChunkTickScheduler can name it without
// including the simulation that owns the scheduler — the include cycle is the
// only reason this is its own header.

#include <cstddef>
#include <functional>

namespace mc::gameplay {

struct SimulationPosition final {
    int x = 0;
    int y = 0;
    int z = 0;

    [[nodiscard]] bool operator==(const SimulationPosition&) const = default;
};

struct SimulationPositionHash final {
    [[nodiscard]] std::size_t operator()(const SimulationPosition& position) const noexcept;
};

} // namespace mc::gameplay

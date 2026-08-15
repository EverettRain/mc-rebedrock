#pragma once

// The render-visible world state, published once per simulation tick under the
// world write lock (the same pattern as PlayerTickSnapshot): the weather, the
// time of day, the named clocks and the game rules the renderer's sky/rain/HUD
// read. The render thread samples this snapshot once per frame instead of
// reaching into live gameplay systems mid-tick.
//
// N3b covers the scalar world state; the block-entity mirror (chests, furnaces)
// and the block deltas ride in their own channels.

#include "gameplay/ChestSystem.hpp"
#include "gameplay/WeatherSystem.hpp"
#include "world/WorldClock.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace mc::gameplay {

struct WorldSnapshot final {
    std::uint64_t serverTick = 0U;

    // The smoothed weather gradients the sky, rain and thunder render from. The
    // previous/current endpoints let the renderer reproduce the per-frame
    // interpolation rainGradientAt(alpha) would give without the live system.
    float previousRainGradient = 0.0F;
    float rainGradient = 0.0F;
    float previousThunderGradient = 0.0F;
    float thunderGradient = 0.0F;
    bool raining = false;
    bool thundering = false;

    // The overworld time-of-day in ticks [0, 24000), for the sun, moon and the
    // day/night sky.
    double dayTimeTicks = 0.0;

    // The named clocks, so a frozen sun (doDaylightCycle=false) renders still
    // and the day count (moon phase) is preserved.
    std::array<world::ClockState, world::kClockCount> clocks{};

    // The game rules the renderer reads (daylight, weather cycles).
    bool doDaylightCycle = true;
    bool doWeatherCycle = true;

    // The chest block entities' render state (position + lid hinge), so the
    // world renderer draws the lid without reaching into the live chest system.
    // Contents are deliberately not copied: the container screen reads those
    // from the open chest directly.
    struct ChestRenderState final {
        ChestPosition position{};
        float previousLidAngle = 0.0F;
        float lidAngle = 0.0F;
    };
    std::vector<ChestRenderState> chests;
};

} // namespace mc::gameplay

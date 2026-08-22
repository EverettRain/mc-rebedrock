#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mc::world {

// The named clocks a world runs. 26.1 keeps these in a registry
// (Registries.WORLD_CLOCK, WorldClocks.OVERWORLD/THE_END); a fixed enum is the
// C++ equivalent while no data pack drives them. The point of naming them at
// all is that a clock can be paused or re-rated on its own, so freezing the sun
// can never freeze anything else — the single gameTimeSeconds it replaces froze
// mining, use cooldowns, chat and the cursor along with the sun.
enum class ClockId : std::uint8_t {
    Overworld,
    // The Nether and the End each run their own clock too (26.1
    // WorldClocks.OVERWORLD / THE_END plus the Nether). Their DimensionType
    // carries a fixed time, so DIM2's tick will hold these clocks at that value
    // instead of advancing the day — the enum only names them so a paused sun in
    // one dimension can never freeze another.
    Nether,
    End,
    Count,
};

// One clock per dimension: the clock ids line up index-for-index with
// DimensionId (see world/Dimension.hpp), so `level(dim)` and its clock share the
// same dense index. This equality is what lets a per-dimension tick reach its
// clock by a subscript rather than a lookup.
static_assert(static_cast<std::uint8_t>(ClockId::Overworld) == 0U);
static_assert(static_cast<std::uint8_t>(ClockId::Nether) == 1U);
static_assert(static_cast<std::uint8_t>(ClockId::End) == 2U);

inline constexpr std::size_t kClockCount = static_cast<std::size_t>(ClockId::Count);

// One clock's whole state, mirroring 26.1's ClockState record. `rate` scales the
// clock against the server tick (0.5 runs the sun at half speed without
// touching anything else), and `partialTick` carries the fraction a non-integer
// rate leaves behind so no time is lost across ticks.
struct ClockState final {
    std::uint64_t totalTicks = 0U;
    float partialTick = 0.0F;
    float rate = 1.0F;
    bool paused = false;

    [[nodiscard]] bool operator==(const ClockState&) const = default;
};

// A named point on a clock's period, mirroring 26.1's ClockTimeMarker registry
// (ClockTimeMarkers.DAY/NOON/NIGHT/MIDNIGHT). `/time set day` resolves through
// one of these, and — this is the whole point — always moves *forward* to the
// next occurrence. A marker can never wind a clock backwards, which is what
// used to strand `nextUseSeconds` in the future and freeze interaction.
enum class ClockTimeMarker : std::uint8_t {
    Day,
    Noon,
    Night,
    Midnight,
};

// Where each marker sits inside the 24000-tick day (Timelines.OVERWORLD_DAY:
// day 1000, noon 6000, night 13000, midnight 18000).
[[nodiscard]] std::uint64_t timeMarkerTicks(ClockTimeMarker marker);

class ClockManager final {
  public:
    // One server tick of every clock. Each clock advances by its own rate and
    // skips entirely while paused; the server tick that drives this never
    // stops, so gameplay timing is unaffected by any of it.
    void tick();

    [[nodiscard]] std::uint64_t totalTicks(ClockId clock) const {
        return state(clock).totalTicks;
    }
    // The fraction of a tick already elapsed, for render interpolation.
    [[nodiscard]] float partialTick(ClockId clock) const { return state(clock).partialTick; }

    [[nodiscard]] const ClockState& state(ClockId clock) const {
        return clocks_[static_cast<std::size_t>(clock)];
    }
    void setState(ClockId clock, const ClockState& value) {
        clocks_[static_cast<std::size_t>(clock)] = value;
    }

    // `/time set <n>`: land exactly on a tick, dropping the partial so the jump
    // is not smeared across the next tick.
    void setTotalTicks(ClockId clock, std::uint64_t ticks);
    // `/time add <n>`: never lets a clock go below zero.
    void addTicks(ClockId clock, std::int64_t ticks);
    // `/time set day|noon|night|midnight`: moves forward to the next occurrence
    // of the marker, so the clock is monotonic across the whole command set.
    void moveToTimeMarker(ClockId clock, ClockTimeMarker marker);

    void setPaused(ClockId clock, bool paused);
    void setRate(ClockId clock, float rate);

  private:
    [[nodiscard]] ClockState& mutableState(ClockId clock) {
        return clocks_[static_cast<std::size_t>(clock)];
    }

    std::array<ClockState, kClockCount> clocks_{};
};

} // namespace mc::world

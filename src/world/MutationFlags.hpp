#pragma once

#include <cstdint>
#include <type_traits>

namespace mc::world {

// What a caller asks WorldMutationService to do besides writing the cell.
//
// The set mirrors 26.1's Block.UPDATE_* bits rather than 1.16.1's, and the
// difference matters: 1.16.1 had a "do not update light" bit (32), which 26.1
// reclaimed for UPDATE_SUPPRESS_DROPS. Light and mesh invalidation are no
// longer a caller's decision at all — they are derived from whether the old and
// new state actually differ. Leaving them out is the point: every "forgot to
// relight after editing a block" bug becomes unrepresentable.
enum class MutationFlags : std::uint16_t {
    None = 0U,

    // Neighbours react: sand falls, fluids flow, torches and crops pop off,
    // leaves start decaying. Comparators re-read their signal.
    NotifyNeighbors = 1U << 0U,
    // Clients are told, and the section is queued for a redraw.
    NotifyClients = 1U << 1U,
    // Run the neighbour shape updates in this call instead of deferring them.
    Immediate = 1U << 3U,
    // The caller already knows the shape is unchanged, so the recursive
    // updateNeighbourShapes pass can be skipped.
    KnownShape = 1U << 4U,
    // Replace the block without dropping its contents or its item.
    SuppressDrops = 1U << 5U,
    // A piston moved this block; block entities travel rather than break.
    MovedByPiston = 1U << 6U,
    // Do not create, keep or destroy a block entity for this write. World
    // generation uses it; nothing in a running world should.
    SkipBlockEntity = 1U << 8U,
    // Skip the new state's onPlaced callback.
    SkipOnPlace = 1U << 9U,

    // The ordinary edit a player, a fluid or a random tick makes.
    All = NotifyNeighbors | NotifyClients,
    // World generation: the chunk is not in the world yet, so there is nobody
    // to notify, no block entity to attach and no placement callback to run.
    Generation = KnownShape | SkipOnPlace | SkipBlockEntity,
    // Every side effect off, for bulk edits that repair their own consequences.
    SkipAllSideEffects = KnownShape | SuppressDrops | SkipBlockEntity | SkipOnPlace,
};

// Level.UPDATE_LIMIT: how deep the recursive neighbour-shape pass may go before
// it gives up. Vanilla's guard against a self-feeding update chain.
inline constexpr int kDefaultUpdateLimit = 512;

[[nodiscard]] constexpr MutationFlags operator|(MutationFlags left, MutationFlags right) {
    using Underlying = std::underlying_type_t<MutationFlags>;
    return static_cast<MutationFlags>(static_cast<Underlying>(left) |
                                      static_cast<Underlying>(right));
}

[[nodiscard]] constexpr MutationFlags operator&(MutationFlags left, MutationFlags right) {
    using Underlying = std::underlying_type_t<MutationFlags>;
    return static_cast<MutationFlags>(static_cast<Underlying>(left) &
                                      static_cast<Underlying>(right));
}

[[nodiscard]] constexpr MutationFlags operator~(MutationFlags value) {
    using Underlying = std::underlying_type_t<MutationFlags>;
    return static_cast<MutationFlags>(~static_cast<Underlying>(value));
}

constexpr MutationFlags& operator|=(MutationFlags& left, MutationFlags right) {
    left = left | right;
    return left;
}

constexpr MutationFlags& operator&=(MutationFlags& left, MutationFlags right) {
    left = left & right;
    return left;
}

// Whether every bit in `wanted` is set. Named rather than spelled out at call
// sites so the "did the caller ask for this?" test reads the same everywhere.
[[nodiscard]] constexpr bool hasFlag(MutationFlags value, MutationFlags wanted) {
    return (value & wanted) == wanted;
}

// Why a cell changed. Sound, particles, statistics and the future event stream
// all branch on this instead of guessing from the call site, so the same edit
// made by a player, a fluid, gravity, an explosion or a command produces one
// consistent set of consequences.
enum class MutationCause : std::uint8_t {
    PlayerBreak,
    PlayerPlace,
    Fluid,
    Gravity,
    RandomTick,
    ScheduledTick,
    Explosion,
    Command,
    Generation,
};

} // namespace mc::world

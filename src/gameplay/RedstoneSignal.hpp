#pragma once

// The redstone signal-query model (W-4), source-derived from Java 26.1's
// SignalGetter/RedstoneTorchBlock/RedstoneWallTorchBlock/PoweredBlock. This is
// the foundation every component's driving logic reads: "how much power does a
// block emit toward a side" and the world-level aggregation that turns those
// per-block answers into "is this cell powered".
//
// It is a by-BlockId query, not a virtual dispatch — the DOD shape the roadmap
// asks for (isSignalSource pre-filter + a small switch that R1 folds into the
// behaviour table). Two power kinds, exactly as Java:
//
//   * **weak** (getSignal): powers adjacent redstone dust/components.
//   * **strong / direct** (getDirectSignal): also powers a conductor, which then
//     re-emits weak power to *its* neighbours (getDirectSignalTo). This is why a
//     lever on a block powers dust two cells away but a block of redstone — which
//     is weak-only — does not.
//
// Timing (torch toggle/burnout, repeater delay) is NOT here; this file is the
// pure, side-effect-free signal answer the component tick and the wire evaluator
// both consult.

#include "world/Block.hpp"
#include "world/BlockPos.hpp"
#include "world/BlockState.hpp"
#include "world/World.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace mc::gameplay::redstone {

// The six faces, in Java's Direction sense. Order is irrelevant to every query
// below (they all reduce by max or or), so it is chosen for readability.
enum class Direction : std::uint8_t { Down, Up, North, South, West, East };

inline constexpr std::array<world::BlockPos, 6> kDirectionOffsets{{
    {0, -1, 0}, // Down
    {0, 1, 0},  // Up
    {0, 0, -1}, // North (-Z)
    {0, 0, 1},  // South (+Z)
    {-1, 0, 0}, // West (-X)
    {1, 0, 0},  // East (+X)
}};

inline constexpr std::array<Direction, 6> kAllDirections{{
    Direction::Down, Direction::Up, Direction::North,
    Direction::South, Direction::West, Direction::East,
}};

[[nodiscard]] constexpr world::BlockPos relative(world::BlockPos pos, Direction dir) {
    const world::BlockPos offset = kDirectionOffsets[static_cast<std::size_t>(dir)];
    return {pos.x + offset.x, pos.y + offset.y, pos.z + offset.z};
}

// A wall torch's FACING as a Direction (it is always horizontal).
[[nodiscard]] constexpr Direction facingOf(world::BlockState state) {
    switch (state.orientation()) {
    case world::BlockOrientation::North:
        return Direction::North;
    case world::BlockOrientation::East:
        return Direction::East;
    case world::BlockOrientation::South:
        return Direction::South;
    case world::BlockOrientation::West:
        return Direction::West;
    case world::BlockOrientation::Up:
        return Direction::Up;
    case world::BlockOrientation::Down:
        return Direction::Down;
    }
    return Direction::North;
}

// Whether the block ever emits a redstone signal — the pre-filter that lets the
// aggregation skip the overwhelming majority of blocks (PoweredBlock/torches now;
// lever/button/etc. join as they land).
[[nodiscard]] constexpr bool isSignalSource(world::Block block) {
    return block == world::Block::RedstoneBlock || block == world::Block::RedstoneTorch ||
           block == world::Block::RedstoneWallTorch || block == world::Block::Lever;
}

// Weak power this block emits toward `dir` (RedstoneTorchBlock.getSignal etc.).
[[nodiscard]] constexpr int getSignal(world::BlockState state, Direction dir) {
    switch (state.block()) {
    case world::Block::RedstoneBlock:
        return 15; // PoweredBlock: every side, always
    case world::Block::RedstoneTorch:
        // LIT && direction != UP ? 15 : 0
        return state.lit() && dir != Direction::Up ? 15 : 0;
    case world::Block::RedstoneWallTorch:
        // LIT && FACING != direction ? 15 : 0 (never toward the wall it faces)
        return state.lit() && facingOf(state) != dir ? 15 : 0;
    case world::Block::Lever:
        // LeverBlock.getSignal: POWERED ? 15 : 0, every side weakly.
        return state.powered() ? 15 : 0;
    default:
        return 0;
    }
}

// Strong/direct power this block emits toward `dir`. A torch strongly powers only
// its DOWN face (RedstoneTorchBlock.getDirectSignal); redstone_block is weak-only
// (PoweredBlock does not override getDirectSignal, so it inherits 0).
[[nodiscard]] constexpr int getDirectSignal(world::BlockState state, Direction dir) {
    switch (state.block()) {
    case world::Block::RedstoneTorch:
    case world::Block::RedstoneWallTorch:
        return dir == Direction::Down ? getSignal(state, Direction::Down) : 0;
    case world::Block::Lever:
        // LeverBlock.getDirectSignal: strongly powers only the block it hangs on
        // (getConnectedDirection), which FACING records here.
        return state.powered() && facingOf(state) == dir ? 15 : 0;
    default:
        return 0;
    }
}

// A lever's connected direction (LeverBlock.getConnectedDirection): FACING,
// which points *away* from the block it hangs on and is the side it strongly
// powers. Its opposite is the "front" that points at the mount — the block whose
// neighbours the lever also notifies when toggled (LeverBlock.updateNeighbours),
// which is how a torch standing on that mount learns the input changed.
[[nodiscard]] constexpr Direction leverConnectedDirection(world::BlockState state) {
    return facingOf(state);
}

[[nodiscard]] constexpr Direction opposite(Direction dir) {
    switch (dir) {
    case Direction::Down:
        return Direction::Up;
    case Direction::Up:
        return Direction::Down;
    case Direction::North:
        return Direction::South;
    case Direction::South:
        return Direction::North;
    case Direction::West:
        return Direction::East;
    case Direction::East:
        return Direction::West;
    }
    return dir;
}

// The mount a lever hangs on: one cell along the front (opposite its connected
// direction). This is the block whose neighbours LeverBlock.updateNeighbours
// notifies in addition to the lever's own.
[[nodiscard]] constexpr world::BlockPos leverMountPos(world::BlockState state,
                                                      world::BlockPos leverPos) {
    return relative(leverPos, opposite(leverConnectedDirection(state)));
}

// Whether a block re-emits the strong power it receives — a full solid cube.
// (A finer opaque/sturdy test is a later refinement; Stone, the conductor the
// components stand on, is a full cube and every non-cube component is not.)
[[nodiscard]] inline bool isRedstoneConductor(world::BlockState state) {
    return state.isFullCubeState();
}

// The strongest direct signal fed into `pos` from its six neighbours
// (SignalGetter.getDirectSignalTo): each neighbour is asked for its direct signal
// in the direction from `pos` to it.
[[nodiscard]] inline int getDirectSignalTo(const world::World& world, world::BlockPos pos) {
    int best = 0;
    for (const Direction dir : kAllDirections) {
        const world::BlockPos neighbor = relative(pos, dir);
        best = std::max(best, getDirectSignal(world.state(neighbor.x, neighbor.y, neighbor.z), dir));
        if (best >= 15) {
            return 15;
        }
    }
    return best;
}

// The signal `pos` presents on its `dir` face (SignalGetter.getSignal): the
// block's own weak emission, raised to the direct signal it receives when it is a
// conductor (the re-emission that carries strong power one cell further).
[[nodiscard]] inline int getSignal(const world::World& world, world::BlockPos pos, Direction dir) {
    const world::BlockState state = world.state(pos.x, pos.y, pos.z);
    const int weak = getSignal(state, dir);
    return isRedstoneConductor(state) ? std::max(weak, getDirectSignalTo(world, pos)) : weak;
}

[[nodiscard]] inline bool hasSignal(const world::World& world, world::BlockPos pos, Direction dir) {
    return getSignal(world, pos, dir) > 0;
}

// The strongest signal reaching `pos` from any neighbour (SignalGetter
// .getBestNeighborSignal) — what a redstone wire reads to set its own power.
[[nodiscard]] inline int getBestNeighborSignal(const world::World& world, world::BlockPos pos) {
    int best = 0;
    for (const Direction dir : kAllDirections) {
        const world::BlockPos neighbor = relative(pos, dir);
        best = std::max(best, getSignal(world, neighbor, dir));
        if (best >= 15) {
            return 15;
        }
    }
    return best;
}

// A redstone torch's input: whether the block it is mounted on (directly below)
// is powered, read on its DOWN face — RedstoneTorchBlock.hasNeighborSignal =
// level.hasSignal(pos.below(), DOWN).
[[nodiscard]] inline bool torchHasNeighborSignal(const world::World& world,
                                                 world::BlockPos torchPos) {
    return hasSignal(world, {torchPos.x, torchPos.y - 1, torchPos.z}, Direction::Down);
}

} // namespace mc::gameplay::redstone

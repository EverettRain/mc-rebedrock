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
           block == world::Block::RedstoneWallTorch || block == world::Block::Lever ||
           block == world::Block::Repeater || block == world::Block::Comparator ||
           block == world::Block::RedstoneWire || block == world::Block::Observer ||
           block == world::Block::StoneButton;
}

// Whether a block is a diode (repeater or comparator) — DiodeBlock.isDiode.
[[nodiscard]] constexpr bool isDiode(world::Block block) {
    return block == world::Block::Repeater || block == world::Block::Comparator;
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
    case world::Block::StoneButton:
        // LeverBlock/ButtonBlock.getSignal: POWERED ? 15 : 0, every side weakly.
        return state.powered() ? 15 : 0;
    case world::Block::Repeater:
        // DiodeBlock.getSignal: output 15 only out of its FACING side when on.
        return state.powered() && facingOf(state) == dir ? 15 : 0;
    case world::Block::Comparator:
        // Same, but the output is its analog value rather than a flat 15.
        return state.powered() && facingOf(state) == dir ? state.analogSignal() : 0;
    case world::Block::RedstoneWire:
        // Wire weakly powers every side except DOWN with its POWER (the
        // connection-directional refinement lands with a later slice).
        return dir != Direction::Down ? state.analogSignal() : 0;
    case world::Block::Observer:
        // ObserverBlock.getSignal: 15 out its FACING side (its back, which the
        // pulse faces) while POWERED.
        return state.powered() && facingOf(state) == dir ? 15 : 0;
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
    case world::Block::StoneButton:
        // LeverBlock/ButtonBlock.getDirectSignal: strongly powers only the block
        // it hangs on (getConnectedDirection), which FACING records here.
        return state.powered() && facingOf(state) == dir ? 15 : 0;
    case world::Block::Repeater:
    case world::Block::Comparator:
        // DiodeBlock.getDirectSignal == getSignal (strong out the FACING side).
        return getSignal(state, dir);
    case world::Block::RedstoneWire:
        // RedStoneWireBlock.getDirectSignal: strongly powers only the block below.
        return dir == Direction::Down ? state.analogSignal() : 0;
    case world::Block::Observer:
        // ObserverBlock.getDirectSignal == getSignal.
        return getSignal(state, dir);
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

// Horizontal clockwise / counter-clockwise, viewed from above (Direction
// .getClockWise / getCounterClockWise). Non-horizontal directions are returned
// unchanged; a diode's FACING is always horizontal.
[[nodiscard]] constexpr Direction clockWise(Direction dir) {
    switch (dir) {
    case Direction::North:
        return Direction::East;
    case Direction::East:
        return Direction::South;
    case Direction::South:
        return Direction::West;
    case Direction::West:
        return Direction::North;
    default:
        return dir;
    }
}
[[nodiscard]] constexpr Direction counterClockWise(Direction dir) {
    return clockWise(clockWise(clockWise(dir)));
}

// SignalGetter.getControlInputSignal: what a diode reads from one side. With
// onlyDiodes (a repeater's side/lock input) only a diode's direct signal counts;
// otherwise (a comparator's side input) redstone_block/wire/any source count.
[[nodiscard]] inline int getControlInputSignal(const world::World& world, world::BlockPos pos,
                                               Direction dir, bool onlyDiodes) {
    const world::BlockState state = world.state(pos.x, pos.y, pos.z);
    if (onlyDiodes) {
        return isDiode(state.block()) ? getDirectSignal(state, dir) : 0;
    }
    if (state.block() == world::Block::RedstoneBlock) {
        return 15;
    }
    if (state.block() == world::Block::RedstoneWire) {
        return state.analogSignal();
    }
    return isSignalSource(state.block()) ? getDirectSignal(state, dir) : 0;
}

// DiodeBlock.getInputSignal: the signal at the block one step along FACING (the
// diode's input side). >=15 short-circuits; a wire there would also contribute
// its POWER (added when wire lands).
[[nodiscard]] inline int diodeInputSignal(const world::World& world, world::BlockPos pos,
                                          world::BlockState state) {
    const Direction facing = facingOf(state);
    const world::BlockPos target = relative(pos, facing);
    int input = getSignal(world, target, facing);
    if (input >= 15) {
        return input;
    }
    const world::BlockState targetState = world.state(target.x, target.y, target.z);
    if (targetState.block() == world::Block::RedstoneWire) {
        input = std::max(input, targetState.analogSignal());
    }
    return input;
}

// DiodeBlock.getAlternateSignal: the strongest control input from the two
// perpendicular sides. `onlyDiodes` is true for a repeater (its lock input).
[[nodiscard]] inline int diodeAlternateSignal(const world::World& world, world::BlockPos pos,
                                              world::BlockState state, bool onlyDiodes) {
    const Direction facing = facingOf(state);
    const Direction cw = clockWise(facing);
    const Direction ccw = counterClockWise(facing);
    return std::max(getControlInputSignal(world, relative(pos, cw), cw, onlyDiodes),
                    getControlInputSignal(world, relative(pos, ccw), ccw, onlyDiodes));
}

// RepeaterBlock.isLocked: a repeater is locked while a powered diode points into
// either of its sides (getAlternateSignal > 0, diodes only).
[[nodiscard]] inline bool repeaterIsLocked(const world::World& world, world::BlockPos pos,
                                           world::BlockState state) {
    return diodeAlternateSignal(world, pos, state, /*onlyDiodes=*/true) > 0;
}

// DiodeBlock.shouldTurnOn for a repeater: its input side carries a signal.
[[nodiscard]] inline bool repeaterShouldTurnOn(const world::World& world, world::BlockPos pos,
                                               world::BlockState state) {
    return diodeInputSignal(world, pos, state) > 0;
}

// DiodeBlock.shouldPrioritize: the cell behind the output holds a diode not
// facing back at this one (diode-facing-diode), which schedules at EXTREMELY_HIGH.
[[nodiscard]] inline bool diodeShouldPrioritize(const world::World& world, world::BlockPos pos,
                                                world::BlockState state) {
    const Direction behind = opposite(facingOf(state));
    const world::BlockPos oppositePos = relative(pos, behind);
    const world::BlockState oppositeState =
        world.state(oppositePos.x, oppositePos.y, oppositePos.z);
    return isDiode(oppositeState.block()) && facingOf(oppositeState) != behind;
}

} // namespace mc::gameplay::redstone

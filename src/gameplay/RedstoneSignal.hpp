#pragma once

// The redstone signal-query model (W-4), source-derived from Java 26.1's
// SignalGetter/RedstoneTorchBlock/RedstoneWallTorchBlock/PoweredBlock. This is
// the world-level aggregation that turns each block's per-face emission (now in
// RedstoneEmission.hpp, a BlockId-keyed table rather than a switch) into "is this
// cell powered". Two power kinds, exactly as Java:
//
//   * **weak** (getSignal): powers adjacent redstone dust/components.
//   * **strong / direct** (getDirectSignal): also powers a conductor, which then
//     re-emits weak power to *its* neighbours (getDirectSignalTo). This is why a
//     lever on a block powers dust two cells away but a block of redstone — which
//     is weak-only — does not.
//
// The per-block emission answers (getSignal/getDirectSignal on a BlockState) live
// in RedstoneEmission.hpp; this file reads that table directly on its hot path
// and builds the world aggregation on top. Timing (torch toggle/burnout, repeater
// delay) is NOT here; this is the pure, side-effect-free signal answer the
// component tick and the wire evaluator both consult.

#include "gameplay/RedstoneEmission.hpp"
#include "world/Block.hpp"
#include "world/BlockPos.hpp"
#include "world/BlockState.hpp"
#include "world/World.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>

namespace mc::gameplay::redstone {

// Direction, facingOf, isSignalSource, isDiode and the per-block getSignal/
// getDirectSignal emission answers come from RedstoneEmission.hpp.

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

// AR-B4-6 / AbstractContainerMenu.getRedstoneSignalFromContainer: how full a
// container reads as a 0-15 signal.
//
//   totalPercent = sum(count / maxStackSize) / slotCount
//   signal       = Mth.lerpDiscrete(totalPercent, 0, 15)
//                = floor(totalPercent * 14) + (totalPercent > 0 ? 1 : 0)
//
// The `+1` is why a single item in a double chest still reads 1 rather than
// rounding away to 0: any content at all is at least one level of signal, and
// only a completely full container reaches 15. Taking the sum and the slot count
// rather than the container keeps this a pure function the test can walk through
// every step of.
[[nodiscard]] inline int redstoneSignalFromContainer(float fillSum, int slotCount) {
    if (slotCount <= 0) {
        return 0;
    }
    const float totalPercent = fillSum / static_cast<float>(slotCount);
    return static_cast<int>(std::floor(totalPercent * 14.0F)) + (totalPercent > 0.0F ? 1 : 0);
}

// AR-B4-6 / ComparatorBlock#getInputSignal. A container behind the comparator
// *replaces* the ordinary diode input rather than adding to it — vanilla assigns
// `i = blockState.getAnalogOutputSignal(...)` — so a half-full chest reads 7 even
// with a lit redstone block beside it.
//
// `analogOutput` is < 0 when the block behind has no analog output at all
// (vanilla's `hasAnalogOutputSignal() == false`), which is not the same as an
// empty container reading 0: an empty chest overrides a signal, a stone block
// does not.
[[nodiscard]] inline int comparatorInputSignal(const world::World& world, world::BlockPos pos,
                                               world::BlockState state, int analogOutput) {
    if (analogOutput >= 0) {
        return analogOutput;
    }
    return diodeInputSignal(world, pos, state);
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

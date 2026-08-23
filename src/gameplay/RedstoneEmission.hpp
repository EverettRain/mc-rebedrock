#pragma once

// The per-block redstone signal *emission* model — "how much power does this
// block state push out of a given face", split out of RedstoneSignal.hpp so the
// answer is a table lookup rather than a `switch (state.block())`. This is the
// R1 fold the W-4 signal model always pointed at (see RedstoneSignal.hpp's
// header: "a small switch that R1 folds into the behaviour table"): each block's
// weak/strong emission is a named function, indexed by block ordinal into a
// constexpr table baked into rodata — the same DOD shape as kRandomTickTable and
// the MiningSystem drop table.
//
// Two consumers share these functions, one source of truth:
//   * the redstone signal query (RedstoneSignal.hpp) reads the constexpr tables
//     directly on its hot path, the way the mesher reads world::blockShape
//     directly rather than through the behaviour table (BlockBehavior.hpp blesses
//     that: a hot, constexpr single source is not worth an allocation-backed
//     runtime-table hop);
//   * BlockBehavior wires its reserved getWeakPower/getStrongPower slots and the
//     IsSignalSource pre-filter bit to these same functions, so a block's signal
//     emission is part of the one behaviour face every other block behaviour
//     lives on.
//
// Kept deliberately light — BlockState/Block only, no World, no behaviour table —
// so BlockBehavior.hpp can include it to fill its slots without a cycle.

#include "world/Block.hpp"
#include "world/BlockState.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace mc::gameplay::redstone {

// The six faces, in Java's Direction sense. Order is irrelevant to every query
// (they reduce by max/or), so it is chosen for readability.
enum class Direction : std::uint8_t { Down, Up, North, South, West, East };

// A wall torch / diode / observer FACING as a Direction. Levers and buttons also
// record the side they hang on here.
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

// Whether the block ever emits a redstone signal — the source of truth for the
// IsSignalSource pre-filter (BlockBehavior) and the aggregation's cheap skip.
[[nodiscard]] constexpr bool isSignalSource(world::Block block) {
    return block == world::Block::RedstoneBlock || block == world::Block::RedstoneTorch ||
           block == world::Block::RedstoneWallTorch || block == world::Block::Lever ||
           block == world::Block::Repeater || block == world::Block::Comparator ||
           block == world::Block::RedstoneWire || block == world::Block::Observer ||
           block == world::Block::StoneButton || block == world::Block::StonePressurePlate;
}

// Whether a block is a diode (repeater or comparator) — DiodeBlock.isDiode.
[[nodiscard]] constexpr bool isDiode(world::Block block) {
    return block == world::Block::Repeater || block == world::Block::Comparator;
}

// One block state's emission toward one face. The per-block handlers below are
// the individual `case`s of the old getSignal/getDirectSignal switches, now
// addressable one-by-one.
using PowerFn = int (*)(world::BlockState state, Direction dir);

namespace detail {

// --- weak emission (getSignal): powers adjacent dust/components ---
[[nodiscard]] constexpr int powerNone(world::BlockState, Direction) { return 0; }
// PoweredBlock: every side, always.
[[nodiscard]] constexpr int weakRedstoneBlock(world::BlockState, Direction) { return 15; }
// RedstoneTorchBlock.getSignal: LIT && direction != UP.
[[nodiscard]] constexpr int weakRedstoneTorch(world::BlockState s, Direction dir) {
    return s.lit() && dir != Direction::Up ? 15 : 0;
}
// RedstoneWallTorchBlock.getSignal: LIT && FACING != direction (never into the wall).
[[nodiscard]] constexpr int weakRedstoneWallTorch(world::BlockState s, Direction dir) {
    return s.lit() && facingOf(s) != dir ? 15 : 0;
}
// LeverBlock/ButtonBlock.getSignal: POWERED ? 15 : 0, every side weakly.
[[nodiscard]] constexpr int weakPoweredAllSides(world::BlockState s, Direction) {
    return s.powered() ? 15 : 0;
}
// DiodeBlock/ObserverBlock.getSignal: 15 only out the FACING side while on.
[[nodiscard]] constexpr int poweredOutFacing15(world::BlockState s, Direction dir) {
    return s.powered() && facingOf(s) == dir ? 15 : 0;
}
// ComparatorBlock.getSignal: its analog value out the FACING side while on.
[[nodiscard]] constexpr int weakComparator(world::BlockState s, Direction dir) {
    return s.powered() && facingOf(s) == dir ? s.analogSignal() : 0;
}
// RedStoneWireBlock.getSignal: weakly powers every side except DOWN with its POWER.
[[nodiscard]] constexpr int weakRedstoneWire(world::BlockState s, Direction dir) {
    return dir != Direction::Down ? s.analogSignal() : 0;
}
// BasePressurePlateBlock.getSignal: POWERED ? 15 : 0, every side weakly — the
// same shape as a lever/button's weakPoweredAllSides (PressurePlateBlock's
// getSignalForState is exactly POWERED ? 15 : 0, and BasePressurePlateBlock.
// getSignal returns that unconditionally regardless of direction).
[[nodiscard]] constexpr int weakPressurePlate(world::BlockState s, Direction dir) {
    return weakPoweredAllSides(s, dir);
}

// The weak table, baked into rodata and indexed by block ordinal — the built-in
// block's ordinal is its BlockId, so this is a BlockId-keyed emission table.
inline constexpr std::array<PowerFn, world::kBuiltinBlockCount> kWeakPowerTable = [] {
    std::array<PowerFn, world::kBuiltinBlockCount> table{};
    table.fill(&powerNone);
    const auto set = [&table](world::Block block, PowerFn fn) {
        table[static_cast<std::size_t>(block)] = fn;
    };
    set(world::Block::RedstoneBlock, &weakRedstoneBlock);
    set(world::Block::RedstoneTorch, &weakRedstoneTorch);
    set(world::Block::RedstoneWallTorch, &weakRedstoneWallTorch);
    set(world::Block::Lever, &weakPoweredAllSides);
    set(world::Block::StoneButton, &weakPoweredAllSides);
    set(world::Block::Repeater, &poweredOutFacing15);
    set(world::Block::Comparator, &weakComparator);
    set(world::Block::RedstoneWire, &weakRedstoneWire);
    set(world::Block::Observer, &poweredOutFacing15);
    set(world::Block::StonePressurePlate, &weakPressurePlate);
    return table;
}();

} // namespace detail

// The weak-emission handler for a block — the old getSignal switch as a table
// lookup. External blocks (past the built-ins) emit nothing, the switch default.
[[nodiscard]] constexpr PowerFn weakPowerFn(world::Block block) {
    const auto index = static_cast<std::size_t>(block);
    return index < world::kBuiltinBlockCount ? detail::kWeakPowerTable[index]
                                             : &detail::powerNone;
}

// Weak power this block state emits toward `dir` (SignalGetter/RedstoneTorchBlock
// .getSignal). One indexed load and a call — no per-query switch.
[[nodiscard]] constexpr int getSignal(world::BlockState state, Direction dir) {
    return weakPowerFn(state.block())(state, dir);
}

namespace detail {

// --- strong / direct emission (getDirectSignal): also powers a conductor ---
// RedstoneTorchBlock.getDirectSignal: strong only out its DOWN face, and only as
// much as it emits weakly there (dispatches back through the weak table so the
// torch vs wall-torch DOWN answer stays each block's own).
[[nodiscard]] constexpr int strongTorchDown(world::BlockState s, Direction dir) {
    return dir == Direction::Down ? getSignal(s, Direction::Down) : 0;
}
// RedStoneWireBlock.getDirectSignal: strong only to the block below.
[[nodiscard]] constexpr int strongWireDown(world::BlockState s, Direction dir) {
    return dir == Direction::Down ? s.analogSignal() : 0;
}
// BasePressurePlateBlock.getDirectSignal: direction == UP ? getSignalForState :
// 0 — a pressure plate strongly charges only the block it sits on top of
// (unlike a lever/button, which strongly charge the block they are mounted
// against, i.e. their own FACING).
[[nodiscard]] constexpr int strongPressurePlateUp(world::BlockState s, Direction dir) {
    return dir == Direction::Up ? weakPressurePlate(s, dir) : 0;
}

// The strong table. Levers/buttons strongly power only the block they hang on
// (FACING); diodes and the observer's direct signal equal their weak emission;
// redstone_block is weak-only (inherits the default 0).
inline constexpr std::array<PowerFn, world::kBuiltinBlockCount> kStrongPowerTable = [] {
    std::array<PowerFn, world::kBuiltinBlockCount> table{};
    table.fill(&powerNone);
    const auto set = [&table](world::Block block, PowerFn fn) {
        table[static_cast<std::size_t>(block)] = fn;
    };
    set(world::Block::RedstoneTorch, &strongTorchDown);
    set(world::Block::RedstoneWallTorch, &strongTorchDown);
    set(world::Block::Lever, &poweredOutFacing15);
    set(world::Block::StoneButton, &poweredOutFacing15);
    set(world::Block::Repeater, &poweredOutFacing15);   // == getSignal
    set(world::Block::Comparator, &weakComparator);     // == getSignal
    set(world::Block::Observer, &poweredOutFacing15);   // == getSignal
    set(world::Block::RedstoneWire, &strongWireDown);
    set(world::Block::StonePressurePlate, &strongPressurePlateUp);
    return table;
}();

} // namespace detail

// The strong-emission handler for a block — the old getDirectSignal switch.
[[nodiscard]] constexpr PowerFn strongPowerFn(world::Block block) {
    const auto index = static_cast<std::size_t>(block);
    return index < world::kBuiltinBlockCount ? detail::kStrongPowerTable[index]
                                             : &detail::powerNone;
}

// Strong/direct power this block state emits toward `dir` (getDirectSignal).
[[nodiscard]] constexpr int getDirectSignal(world::BlockState state, Direction dir) {
    return strongPowerFn(state.block())(state, dir);
}

} // namespace mc::gameplay::redstone

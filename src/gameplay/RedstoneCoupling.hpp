#pragma once

// W-6 (island-analysis scope): the per-block coupling descriptor a redstone
// component contributes to the island partitioner. This is the *extensibility
// guardrail* the roadmap asks for: the one place a component states how far its
// scheduled tick can reach, so partitioning stays sound as new components land.
//
// The island model (W-DESIGN §4.4) partitions the redstone components due on a
// gametick into connected components that *cannot influence each other this
// tick*, so a future threaded evaluator can run each island independently while
// staying bit-for-bit identical to the serial drain. Whether two components can
// influence each other is decided entirely from the footprint each declares
// here; a component that under-declares its reach is the one way the partition
// can wrongly split a coupled pair, which is exactly what the lockstep gate
// catches. So the rule for adding a component is: **declare the true reach here,
// conservatively (over-declaring only costs parallelism; under-declaring breaks
// determinism).**
//
// This is a by-BlockId constexpr table (DOD), not a virtual method — the same
// shape as RedstoneSignal's queries and kRandomTickTable. Timing/semantics live
// in the component tick handlers; this file only answers "how far can one tick
// touch".

#include "world/Block.hpp"

#include <array>
#include <cstdint>

namespace mc::gameplay::redstone {

// How far a component's scheduled tick can read or write, and whether it couples
// through channels a positional footprint cannot express.
struct RedstoneCoupling final {
    // True for a block that has a scheduled-tick handler in
    // WorldSimulation::dispatchRedstoneTick — the pre-filter that lets the
    // planner skip anything that never appears in the redstone due set.
    bool component = false;

    // The Chebyshev radius of the footprint the tick touches, as a ball around
    // the component's own cell. Conservative and symmetric (read and write folded
    // into one ball): two components are coupled when their footprints share a
    // cell. A conductor re-emits strong power one cell further, so a component
    // that reads a neighbour through a conductor (torch below, diode input side)
    // reaches 2; one that only reads/writes its own cell and wakes its direct
    // neighbours reaches 1.
    std::uint8_t reach = 0;

    // The footprint follows the connected redstone_wire graph rather than a fixed
    // ball: a wire tick re-solves and rewrites its whole network at once, so its
    // reach is the network's extent, which the planner resolves by flooding. Two
    // wire ticks in the same network — or a component adjacent to it — are the
    // same island.
    bool wireNetwork = false;

    // The tick couples through a channel a positional footprint cannot bound: it
    // queues a shared block event or moves blocks across cells (a piston). The
    // partitioner collapses the whole due set into one island (serial fallback)
    // when any due tick declares this — "cannot prove independent, so don't
    // parallelise", the conservative escape the design mandates. No current drain
    // handler sets it (pistons settle as block events *after* the redstone drain),
    // but it is the guardrail a future in-drain piston/mover must flip.
    bool globalCoupling = false;
};

namespace detail {

inline constexpr std::array<RedstoneCoupling, world::kBuiltinBlockCount> buildCouplingTable() {
    std::array<RedstoneCoupling, world::kBuiltinBlockCount> table{};
    const auto set = [&table](world::Block block, RedstoneCoupling coupling) {
        table[world::blockId(block).index()] = coupling;
    };
    // Torch: reads the block below through a conductor (hasNeighborSignal), so its
    // read reaches two cells; writes its own LIT and wakes neighbours.
    set(world::Block::RedstoneTorch, {.component = true, .reach = 2});
    set(world::Block::RedstoneWallTorch, {.component = true, .reach = 2});
    // Diodes: read the input side and the two perpendicular sides, each possibly
    // through a conductor — reach 2.
    set(world::Block::Repeater, {.component = true, .reach = 2});
    set(world::Block::Comparator, {.component = true, .reach = 2});
    // Observer / button: read only their own state on the tick and wake a direct
    // neighbour — reach 1.
    set(world::Block::Observer, {.component = true, .reach = 1});
    set(world::Block::StoneButton, {.component = true, .reach = 1});
    // Wire: reach follows the network (flooded by the planner).
    set(world::Block::RedstoneWire, {.component = true, .reach = 1, .wireNetwork = true});
    return table;
}

inline constexpr std::array<RedstoneCoupling, world::kBuiltinBlockCount> kCouplingTable =
    buildCouplingTable();

} // namespace detail

// The coupling descriptor for a block. A block with no redstone tick returns the
// default (component = false), which the planner reads as "never in the due set".
[[nodiscard]] constexpr RedstoneCoupling redstoneCoupling(world::Block block) {
    const auto index = world::blockId(block).index();
    return index < detail::kCouplingTable.size() ? detail::kCouplingTable[index]
                                                  : RedstoneCoupling{};
}

} // namespace mc::gameplay::redstone

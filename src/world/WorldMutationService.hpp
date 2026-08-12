#pragma once

#include "world/BlockState.hpp"
#include "world/MutationFlags.hpp"

namespace mc::world {

class World;

// A block position in world space. World-layer callers (the mutation service,
// block behaviour) use this rather than gameplay's SimulationPosition so world/
// keeps no dependency on gameplay/.
struct BlockPos final {
    int x = 0;
    int y = 0;
    int z = 0;

    [[nodiscard]] bool operator==(const BlockPos&) const = default;
};

// What a single setBlock did, so a caller can react without re-reading the cell.
struct BlockMutationResult final {
    bool changed = false;   // the stored state actually differed
    BlockState previous{};  // the state before the write
    BlockState current{};   // the state after the write
};

// The consequences a mutation has that the world layer does not itself own:
// block-entity lifecycle, neighbour reactions, section invalidation, drops and
// events all live in gameplay/ or the renderer. Keeping them behind an interface
// is what lets the whole mutation *flow* live in world/ — deciding when a block
// entity is destroyed, when neighbours are told, when a section relights — while
// the concrete systems (chests, furnaces, the mesher, the loot roller) are
// wired in by whoever implements this. Every method defaults to a no-op so a
// caller that only cares about some effects overrides only those.
class MutationSink {
  public:
    virtual ~MutationSink() = default;

    // The block *kind* changed, so any block entity the old block owned must be
    // destroyed and any the new block needs created. A change that keeps the
    // same block (a furnace lighting, a log re-facing) does NOT call this — the
    // entity, and its smelt, survives. Suppressed by MutationFlags::SkipBlockEntity.
    virtual void onBlockEntityReplaced(BlockPos /*pos*/, BlockState /*previous*/,
                                       BlockState /*current*/) {}

    // A neighbour of the edited cell should re-evaluate: sand falls, a torch
    // pops off, a comparator re-reads. Called once per orthogonal neighbour,
    // only when MutationFlags::NotifyNeighbors is set.
    virtual void onNeighborChanged(BlockPos /*neighbor*/, BlockPos /*source*/) {}

    // The cell's section needs relight and remesh. Called whenever the state
    // actually changed, unconditionally — light and mesh invalidation are
    // derived from the state diff, never a caller's choice, so "forgot to
    // relight after editing" is unrepresentable.
    virtual void onSectionDirty(BlockPos /*pos*/) {}

    // A non-air block was removed or replaced and should roll its drops. The
    // cause selects the loot context (a player break vs a command). Suppressed
    // by MutationFlags::SuppressDrops or MovedByPiston.
    virtual void onDropsRequested(BlockPos /*pos*/, BlockState /*removed*/,
                                  MutationCause /*cause*/) {}
};

// The single path every block change flows through, mirroring 26.1's
// Level.setBlock(pos, state, flags): it writes the cell, then dispatches exactly
// the consequences the flags allow, in a fixed order, so a player break, a fluid
// update and a command edit of the same cell produce the same set of effects.
class WorldMutationService final {
  public:
    // Writes `newState` at `pos` and dispatches its consequences through `sink`.
    // Returns what changed. An out-of-world or unloaded cell is a no-op with
    // changed == false. A write that resolves to the same state it already held
    // is also a no-op: nothing is dirtied, nobody is notified.
    BlockMutationResult setBlock(World& world, BlockPos pos, BlockState newState,
                                 MutationFlags flags, MutationCause cause, MutationSink& sink,
                                 int updateLimit = kDefaultUpdateLimit);
};

} // namespace mc::world

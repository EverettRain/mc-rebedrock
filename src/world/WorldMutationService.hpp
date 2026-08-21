#pragma once

#include "world/BlockPos.hpp"
#include "world/BlockState.hpp"
#include "world/MutationFlags.hpp"
#include "world/NeighborUpdater.hpp"

namespace mc::world {

class World;

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

    // A neighbour of the edited cell should recompute its *shape* against the
    // change: a fence grows or drops a connection arm, a redstone wire re-points,
    // a stair squares off a corner. This is a pure property rewrite of the
    // neighbour (never a block-kind change, never a break) and runs before the
    // reaction pass, mirroring Java's updateNeighbourShapes → neighborChanged
    // order. Called once per orthogonal neighbour in kShapeUpdateOrder unless
    // MutationFlags::KnownShape says the caller already knows no shape changed.
    virtual void onNeighborShapeUpdate(BlockPos /*neighbor*/, BlockPos /*source*/) {}

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

  private:
    // The queue that carries neighbour reactions. Persisting it on the service
    // (rather than a fresh one per call) is what lets a reaction that re-enters
    // setBlock have its own neighbour fan-out collected onto the running drain
    // instead of recursing — the CollectingNeighborUpdater contract.
    NeighborUpdater neighborUpdater_;
};

} // namespace mc::world

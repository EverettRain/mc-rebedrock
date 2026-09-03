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
// wired in by whoever implements this.
//
// W-x-2: every method is pure. They used to default to a no-op, on the reasoning
// that a caller who only cares about some effects should override only those —
// and that is exactly how two bugs got in. RedstoneReactionSink silently dropped
// onSectionDirty, so a door opened in the world and half opened on screen
// (AR-B4-3a); it silently dropped onNeighborShapeUpdate too, so no redstone
// write ever reached an updateShape derivation and a repeater's LOCKED went
// stale (AR-B4-4). Neither was a wrong implementation. Both were an absent one,
// and an absent one looked exactly like a deliberate no-op.
//
// So: an implementer must write every method out, and the ones it genuinely does
// not want are empty bodies *with a reason*. Same guard kShapeByModel's
// static_assert gives the shape table — a missing entry is a build failure, not
// a bug report.
class MutationSink {
  public:
    virtual ~MutationSink() = default;

    // The block *kind* changed, so any block entity the old block owned must be
    // destroyed and any the new block needs created. A change that keeps the
    // same block (a furnace lighting, a log re-facing) does NOT call this — the
    // entity, and its smelt, survives. Suppressed by MutationFlags::SkipBlockEntity.
    virtual void onBlockEntityReplaced(BlockPos pos, BlockState previous,
                                       BlockState current) = 0;

    // A neighbour of the edited cell should recompute its *shape* against the
    // change: a fence grows or drops a connection arm, a redstone wire re-points,
    // a stair squares off a corner. This is a pure property rewrite of the
    // neighbour (never a block-kind change, never a break) and runs before the
    // reaction pass, mirroring Java's updateNeighbourShapes → neighborChanged
    // order. Called once per orthogonal neighbour in kShapeUpdateOrder unless
    // MutationFlags::KnownShape says the caller already knows no shape changed.
    virtual void onNeighborShapeUpdate(BlockPos neighbor, BlockPos source) = 0;

    // A neighbour of the edited cell should re-evaluate: sand falls, a torch
    // pops off, a comparator re-reads. Called once per orthogonal neighbour,
    // only when MutationFlags::NotifyNeighbors is set.
    virtual void onNeighborChanged(BlockPos neighbor, BlockPos source) = 0;

    // The cell's section needs relight and remesh. Called whenever the state
    // actually changed, unconditionally — light and mesh invalidation are
    // derived from the state diff, never a caller's choice, so "forgot to
    // relight after editing" is unrepresentable.
    virtual void onSectionDirty(BlockPos pos) = 0;

    // A non-air block was removed or replaced and should roll its drops. The
    // cause selects the loot context (a player break vs a command). Suppressed
    // by MutationFlags::SuppressDrops or MovedByPiston.
    virtual void onDropsRequested(BlockPos pos, BlockState removed,
                                  MutationCause cause) = 0;

    // W-8/W-9: a block newly *arrived* in this cell — Java's Block#onPlace under
    // the `!oldState.is(state.getBlock())` guard its redstone overrides open
    // with (RedStoneWireBlock:296, TntBlock, ObserverBlock, LightningRodBlock).
    //
    // The distinction that matters is against the *unguarded* onPlace, which
    // runs on every setBlockState, a diode's POWERED flip included. That is what
    // W-7's updateNeighborsInFront wants, and it reaches it through the
    // tick/place/remove calls it makes itself rather than through this — a
    // repeater scheduling its own turn-on from the unguarded hook would
    // reschedule on every flip and never settle.
    //
    // W-8 called this setPlacedBy; W-9 corrects the name. setPlacedBy is
    // narrower than what fires here: Java calls it from BlockItem#place, so it
    // reaches an entity placing an item and nothing else, while this fires for a
    // command fill and a piston move as well. The gate is unchanged and
    // deliberate — those really are placements as far as a diode or a wire is
    // concerned — it is a strict superset of setPlacedBy and an exact match for
    // the guarded onPlace.
    //
    // Raised when the block *kind* at the cell changed and the caller did not
    // set MutationFlags::SkipOnPlace — worldgen sets it, since a chunk being
    // built has nobody to tell. `previous` is what was there, so a future
    // "removed" user has it too.
    virtual void onBlockPlaced(BlockPos pos, BlockState previous, BlockState current) = 0;
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

    // Fire the neighbour-changed pass for `pos`'s six neighbours without a write,
    // the equivalent of Java's Level.updateNeighborsAt. A redstone source uses it
    // to propagate a signal change past the block it powers: a lever notifies the
    // neighbours of the block it hangs on, so a torch standing there reacts even
    // though it is not the lever's own neighbour.
    void updateNeighborsAt(BlockPos pos, MutationSink& sink,
                           int updateLimit = kDefaultUpdateLimit);

    // W-x-1: the same fan-out with one cell held back, Java's
    // `updateNeighborsAtExceptFromFacing`. A diode waking the cell in front of
    // it must not have that cell's fan-out come straight back at the diode; JE
    // excludes exactly that neighbour and so does this. `skip` names the cell
    // rather than a direction because every caller already holds the position.
    void updateNeighborsAtExcept(BlockPos pos, BlockPos skip, MutationSink& sink,
                                 int updateLimit = kDefaultUpdateLimit);

  private:
    // The queue that carries neighbour reactions. Persisting it on the service
    // (rather than a fresh one per call) is what lets a reaction that re-enters
    // setBlock have its own neighbour fan-out collected onto the running drain
    // instead of recursing — the CollectingNeighborUpdater contract.
    NeighborUpdater neighborUpdater_;
};

} // namespace mc::world

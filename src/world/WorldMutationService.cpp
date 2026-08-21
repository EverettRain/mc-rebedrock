#include "world/WorldMutationService.hpp"

#include "world/World.hpp"

namespace mc::world {

BlockMutationResult WorldMutationService::setBlock(World& world, BlockPos pos, BlockState newState,
                                                   MutationFlags flags, MutationCause cause,
                                                   MutationSink& sink, int updateLimit) {
    const BlockState previous = world.state(pos.x, pos.y, pos.z);
    BlockMutationResult result{false, previous, previous};

    // A write that does not change the stored state costs nothing: no dirty, no
    // neighbours, no block-entity churn. This is what makes an idempotent edit
    // (re-placing the same block, a fluid settling at the same level) free.
    if (previous == newState) {
        return result;
    }
    if (!world.setState(pos.x, pos.y, pos.z, newState)) {
        // Out of the world or an unloaded chunk: nothing was written.
        return result;
    }
    result.current = newState;
    result.changed = true;

    // Block-entity lifecycle rides on the block *kind*, never the state. A
    // furnace that merely lights (same block, different LIT) keeps its entity
    // and its smelt; only a real block swap destroys and recreates one.
    if (!hasFlag(flags, MutationFlags::SkipBlockEntity) &&
        previous.block() != newState.block()) {
        sink.onBlockEntityReplaced(pos, previous, newState);
    }

    // A removed non-air block rolls its drops only when it was actually
    // destroyed — a player break or an explosion — not merely overwritten. A
    // command fill, a fluid, worldgen replacing stone with dirt: none of those
    // drop, exactly as vanilla's setBlock stays silent and only destroyBlock
    // rolls loot. SuppressDrops and a piston move veto even a real destruction.
    const bool destruction =
        cause == MutationCause::PlayerBreak || cause == MutationCause::Explosion;
    if (destruction && previous.block() != Block::Air &&
        previous.block() != newState.block() &&
        !hasFlag(flags, MutationFlags::SuppressDrops) &&
        !hasFlag(flags, MutationFlags::MovedByPiston)) {
        sink.onDropsRequested(pos, previous, cause);
    }

    // Light and mesh invalidation are derived from the diff, so they fire on
    // every real change regardless of the notify flags.
    sink.onSectionDirty(pos);

    // Shape pass, before the reaction pass — Java's markAndNotifyBlock order.
    // Each of the six neighbours recomputes its shape against this change (a
    // fence connection, a wire re-point), a pure property rewrite. KnownShape is
    // the caller's promise that nothing shape-relevant changed, so worldgen and
    // bulk edits skip it. The order is kShapeUpdateOrder, distinct from the
    // reaction order below.
    if (!hasFlag(flags, MutationFlags::KnownShape)) {
        for (const auto& offset : kShapeUpdateOrder) {
            sink.onNeighborShapeUpdate({pos.x + offset.x, pos.y + offset.y, pos.z + offset.z}, pos);
        }
    }

    // Neighbours only react when asked. The fan-out runs through the collecting
    // updater rather than a bare loop, so a reaction that itself edits a block
    // has its neighbours queued onto this drain instead of recursing, the depth
    // limit caps a self-feeding chain, and the six cells are still notified in
    // the fixed kNeighborUpdateOrder for a deterministic sequence.
    if (hasFlag(flags, MutationFlags::NotifyNeighbors)) {
        neighborUpdater_.updateNeighborsAt(
            pos, updateLimit,
            [&sink](BlockPos neighbor, BlockPos source) { sink.onNeighborChanged(neighbor, source); });
    }

    return result;
}

} // namespace mc::world

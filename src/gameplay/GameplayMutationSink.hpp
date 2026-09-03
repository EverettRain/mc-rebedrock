#pragma once

// The concrete MutationSink for a running world: the one place that knows what
// a block change *means* to the systems outside world/.
//
// Before this existed, every caller that edited a block re-assembled the
// consequences by hand — write the cell, submit the edit, preview the light,
// notify the simulation, and (only at the two call sites that remembered)
// destroy the chest or furnace entity that used to live there. That is the
// class of bug A3 exists to remove: the consequences are now derived from the
// state diff by WorldMutationService and dispatched here, so a cell edited by a
// player, a bucket or a command produces the same set of effects.

#include "core/ContentId.hpp"
#include "world/MutationFlags.hpp"
#include "world/WorldMutationService.hpp"

#include <optional>
#include <utility>

namespace mc::world {
class World;
}

namespace mc::gameplay {

class GameSession;
struct SimulationHost;
struct ItemStack;

class GameplayMutationSink final : public world::MutationSink {
  public:
    // `world` is the gameplay world being edited (read back for the section
    // update) and `session` owns the reacting systems and the event bus. Both
    // outlive the sink, which is meant to be built on the stack around a
    // mutation. There is deliberately no SimulationHost here: section updates
    // are published as WorldEditEvent, and whoever bound the session's event
    // host performs them.
    GameplayMutationSink(world::World& world, GameSession& session)
        : world_(&world), session_(&session) {}

    // The tool credited for any drops this mutation rolls. A player break sets
    // it to the held stack; a fluid or command edit leaves it empty, which is
    // what the loot tables already expect from a non-player break.
    void setDropTool(const ItemStack& tool) { dropTool_ = &tool; }

    void onBlockEntityReplaced(world::BlockPos pos, world::BlockState previous,
                               world::BlockState current) override;
    void onNeighborShapeUpdate(world::BlockPos neighbor, world::BlockPos source) override;
    void onNeighborChanged(world::BlockPos neighbor, world::BlockPos source) override;
    void onSectionDirty(world::BlockPos pos) override;
    void onDropsRequested(world::BlockPos pos, world::BlockState removed,
                          world::MutationCause cause) override;
    // W-8: Block#setPlacedBy — dispatched through the behaviour table's onPlace
    // slot, so a block's placement behaviour is a slot rather than a switch here.
    void onBlockPlaced(world::BlockPos pos, world::BlockState previous,
                       world::BlockState current) override;

  private:
    // Destroys the block entity a broken/replaced block owned, spilling its
    // contents, and creates the one a newly placed block needs. Keyed on the
    // BlockEntityTypeId the block maps to (BE1's block->BE table), not on the
    // block identity, so two blocks that host the same block entity — a chest and
    // a trapped chest, once it exists — collapse to one arm.
    void destroyBlockEntity(core::BlockEntityTypeId type, world::BlockPos pos);
    void createBlockEntity(core::BlockEntityTypeId type, world::BlockPos pos);

    // Scatters a removed container's contents at `pos`, the way a broken chest
    // or furnace spills its slots.
    void scatterContents(world::BlockPos pos, const ItemStack& stack, std::size_t dropIndex) const;

    world::World* world_ = nullptr;
    GameSession* session_ = nullptr;
    const ItemStack* dropTool_ = nullptr;
    // WorldSimulation's notifyNeighborChanged is still *source*-centric: it fans
    // out to the six neighbours itself, queueing falling blocks, fluid wake-ups,
    // support checks and a leaf-decay flood from the edited cell. Driving it
    // once per neighbour would run that fan-out six times from six origins, so
    // the service's six callbacks are collapsed back into one call here. The
    // service always fires onSectionDirty exactly once, before the neighbour
    // loop, which is what resets this and makes the collapse exact rather than
    // merely deduplicating consecutive edits at the same cell.
    std::optional<world::BlockPos> notifiedSource_;
    // W-x-1: what the edited cell held before this edit, recorded when the block
    // kind actually changed (onBlockEntityReplaced, the first callback the
    // mutation service raises and the one that already carries the old state).
    // A diode that was just broken has to wake what it used to feed, and by the
    // time the neighbour pass runs the cell is air.
    std::optional<std::pair<world::BlockPos, world::BlockState>> sourcePrevious_;
};

} // namespace mc::gameplay

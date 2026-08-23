#include "gameplay/GameplayMutationSink.hpp"

#include "gameplay/BlockBehavior.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/MiningSystem.hpp"
#include "world/Block.hpp"
#include "world/BlockEntityType.hpp"
#include "world/World.hpp"

#include <cmath>
#include <cstdint>
#include <optional>

namespace mc::gameplay {
namespace {

// The empty stack a non-player mutation credits its drops to. A fluid washing a
// torch away or a command clearing a cell breaks it bare-handed, which is what
// the loot tables already assume.
const ItemStack& emptyTool() {
    static const ItemStack empty{};
    return empty;
}

[[nodiscard]] glm::ivec3 toVector(world::BlockPos pos) { return {pos.x, pos.y, pos.z}; }

} // namespace

void GameplayMutationSink::scatterContents(world::BlockPos pos, const ItemStack& stack,
                                           std::size_t dropIndex) const {
    if (stack.empty()) {
        return;
    }
    // The golden-angle fan the rest of the drop paths use, so a spilled
    // container looks like every other scatter in the game.
    const float angle = static_cast<float>(dropIndex) * 2.39996323F;
    session_->itemEntities().spawn(glm::vec3{toVector(pos)} + glm::vec3{0.5F, 0.65F, 0.5F}, stack,
                                   {std::cos(angle) * 0.08F, 0.12F, std::sin(angle) * 0.08F});
}

void GameplayMutationSink::onBlockEntityReplaced(world::BlockPos pos, world::BlockState previous,
                                                 world::BlockState current) {
    // The unified lifecycle entry (A3b): the decision to destroy or create is
    // gated on the block's own `hasBlockEntity` pre-filter (one indexed bit test)
    // and dispatched by the BlockEntityTypeId it maps to — never a per-block
    // switch here. That is what makes "placed a container but forgot to build its
    // block entity" unrepresentable: any block whose table row says it hosts one
    // gets it, without this call site enumerating which blocks those are.
    //
    // Destroy first, then create: a cell that swaps one container for another
    // must not have the new entity clobbered by the old one's removal.
    if (world::hasBlockEntity(previous.block())) {
        destroyBlockEntity(world::blockEntityTypeOf(previous.block()), pos);
    }
    if (world::hasBlockEntity(current.block())) {
        createBlockEntity(world::blockEntityTypeOf(current.block()), pos);
    }
}

void GameplayMutationSink::destroyBlockEntity(core::BlockEntityTypeId type, world::BlockPos pos) {
    // The store removal + content spill is inherently per-kind (a chest's 27
    // slots, a furnace's three), so the concrete arm is selected by the mapped
    // type id rather than duplicated per hosting block.
    if (type == world::blockEntityTypeId(world::BlockEntityKind::Chest) ||
        type == world::blockEntityTypeId(world::BlockEntityKind::TrappedChest)) {
        // A trapped chest spills its 27 slots exactly like a chest; only the
        // container it removes from differs.
        ChestSystem& chests = type == world::blockEntityTypeId(world::BlockEntityKind::TrappedChest)
                                  ? session_->trappedChestSystem()
                                  : session_->chestSystem();
        const auto removed = chests.remove({pos.x, pos.y, pos.z});
        if (removed.has_value()) {
            std::size_t dropIndex = 0U;
            for (const auto& stack : removed->items) {
                if (!stack.empty()) {
                    scatterContents(pos, stack, dropIndex++);
                }
            }
        }
    } else if (type == world::blockEntityTypeId(world::BlockEntityKind::Furnace)) {
        // A broken furnace scatters its three slots — input, fuel and the item
        // mid-smelt in the output — exactly as a chest scatters its inventory.
        const auto removed = session_->furnaceSystem().remove({pos.x, pos.y, pos.z});
        if (removed.has_value()) {
            std::size_t dropIndex = 0U;
            for (const auto& stack : {removed->input, removed->fuel, removed->output}) {
                if (!stack.empty()) {
                    scatterContents(pos, stack, dropIndex++);
                }
            }
        }
    }
}

void GameplayMutationSink::createBlockEntity(core::BlockEntityTypeId type, world::BlockPos pos) {
    if (type == world::blockEntityTypeId(world::BlockEntityKind::Chest)) {
        static_cast<void>(session_->chestSystem().place({pos.x, pos.y, pos.z}));
    } else if (type == world::blockEntityTypeId(world::BlockEntityKind::TrappedChest)) {
        static_cast<void>(session_->trappedChestSystem().place({pos.x, pos.y, pos.z}));
    } else if (type == world::blockEntityTypeId(world::BlockEntityKind::Furnace)) {
        // Give the furnace its block entity immediately so it smelts even
        // before its screen is first opened.
        static_cast<void>(session_->furnaceSystem().place({pos.x, pos.y, pos.z}));
    }
}

void GameplayMutationSink::onNeighborShapeUpdate(world::BlockPos neighbor, world::BlockPos source) {
    // An observer watches for a block-state change on its FACING side; the shape
    // pass is exactly "a neighbour's state changed", so this is where it detects.
    session_->worldSimulation().notifyObserverShapeChange(
        *world_, {neighbor.x, neighbor.y, neighbor.z}, {source.x, source.y, source.z});

    // The neighbour recomputes its shape against the changed source. The
    // pre-filter rejects the overwhelming majority (stone, dirt, ore) with one
    // bit test inside dispatchUpdateShape, before any state is read or a slot
    // fetched — no block in the current content set fills the updateShape slot,
    // so this is the fence/wire/stair join mechanism waiting for its content
    // (W-4+), not yet a runtime write.
    const world::BlockState state = world_->state(neighbor.x, neighbor.y, neighbor.z);
    const NeighborUpdateContext context{
        *world_,
        neighbor,
        state,
        {source.x - neighbor.x, source.y - neighbor.y, source.z - neighbor.z},
        world_->state(source.x, source.y, source.z),
    };
    const std::optional<world::BlockState> updated =
        dispatchUpdateShape(world::blockId(state.block()), context);
    if (!updated.has_value()) {
        return;
    }
    // A pure property rewrite: same block, so no block entity churns and nothing
    // drops. It travels the ordinary state-edit channel — written to the cell,
    // then published so the mesher and save pick it up exactly like any edit.
    static_cast<void>(world_->setState(neighbor.x, neighbor.y, neighbor.z, *updated));
    session_->events().publish(WorldEditEvent{neighbor.x, neighbor.y, neighbor.z, *updated, true});
}

void GameplayMutationSink::onNeighborChanged(world::BlockPos neighbor, world::BlockPos source) {
    // Redstone reacts per-neighbour: whichever of the six cells is a component
    // re-reads its input and may schedule its toggle tick. A no-op for a
    // non-component, so it runs for every neighbour without a type check here —
    // this is the block-update half of the redstone drive (W-4).
    //
    // Most components only schedule a later tick here (a torch's 2gt toggle),
    // whose own write travels through dispatchRedstoneTick's own sink/changes
    // path at drain time — but a trapdoor's neighborChanged (W-signal) writes
    // synchronously, in this same call, exactly as vanilla's TrapDoorBlock does
    // (no scheduled delay). The before/after compare is what makes that write
    // visible without a second special-cased publish path: cheap (one extra
    // state read) even on the overwhelming majority of neighbours where nothing
    // changed, and correct for exactly the one case that does.
    const auto before = world_->state(neighbor.x, neighbor.y, neighbor.z);
    session_->worldSimulation().notifyRedstoneComponent(
        *world_, {neighbor.x, neighbor.y, neighbor.z});
    const auto after = world_->state(neighbor.x, neighbor.y, neighbor.z);
    if (after != before) {
        session_->events().publish(WorldEditEvent{neighbor.x, neighbor.y, neighbor.z, after, true});
    }

    // Everything below is source-centric — the falling-block/fluid/support/leaf
    // fan-out that WorldSimulation drives from the changed cell — so it collapses
    // the service's six per-neighbour callbacks back into one call per mutation.
    if (notifiedSource_.has_value() && *notifiedSource_ == source) {
        return;
    }
    notifiedSource_ = source;
    // Both halves, for every cause. The hand-written call sites this replaces
    // each picked one: a placed block got notifyPlaced but no neighbour pass, a
    // bucket of lava got the neighbour pass but no notifyPlaced (so it sat
    // still until something else woke it). Running both is a superset of all of
    // them and is what makes a break, a bucket and a command agree.
    const auto state = world_->state(source.x, source.y, source.z);
    session_->worldSimulation().notifyPlaced({source.x, source.y, source.z}, state.block());
    session_->worldSimulation().notifyNeighborChanged(*world_, {source.x, source.y, source.z});
}

void GameplayMutationSink::onSectionDirty(world::BlockPos pos) {
    // Fires exactly once per real change, before any neighbour callback, which
    // is what makes notifiedSource_ an exact per-mutation reset rather than a
    // duplicate filter.
    notifiedSource_.reset();
    // Published, not called: an interactive edit must be visible the same frame,
    // so it asks for the immediate preview. P3 Step 3 turns this publish into a
    // cross-thread enqueue without this file changing.
    session_->events().publish(
        WorldEditEvent{pos.x, pos.y, pos.z, world_->state(pos.x, pos.y, pos.z), true});
}

void GameplayMutationSink::onDropsRequested(world::BlockPos pos, world::BlockState removed,
                                            world::MutationCause cause) {
    const ItemStack& tool = dropTool_ != nullptr ? *dropTool_ : emptyTool();
    session_->spawnBlockDrops(toVector(pos), removed, tool);
    // XP-2: mining experience is a player-only reward — an explosion popping
    // the same ore, or any other non-player destruction, must not pay it (the
    // task's "破 stone -> 0" bar applies just as much to "a creeper popped a
    // diamond ore": only a PlayerBreak counts). It also shares the drop's own
    // tool-tier gate: an ore mined with too weak a tool (Block#
    // requiresCorrectToolForDrops, the same canHarvestBlock check
    // minedDrops() applies internally) drops nothing *and* pays nothing,
    // exactly the way a wood pickaxe on diamond ore leaves the block gone but
    // the inventory empty. Rolled off the session's own lootRandomState_ — the
    // identical deterministic stream spawnBlockDrops just advanced for this
    // same break — so replaying the same seed and edit sequence always
    // reproduces the same experience, exactly as the block's own drop roll
    // does.
    if (cause == world::MutationCause::PlayerBreak && canHarvestBlock(removed.block(), tool)) {
        if (const auto range = oreExperienceRange(removed.block()); range.has_value()) {
            const std::int32_t amount = rollOreExperience(session_->lootRandomState(), *range);
            if (amount > 0) {
                session_->spawnExperienceOrbs(
                    {static_cast<float>(pos.x) + 0.5F, static_cast<float>(pos.y) + 0.5F,
                     static_cast<float>(pos.z) + 0.5F},
                    amount);
            }
        }
    }
}

} // namespace mc::gameplay

#include "gameplay/GameplayMutationSink.hpp"

#include "gameplay/GameSession.hpp"
#include "world/World.hpp"

#include <cmath>

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
    // Destroy first, then create: a cell that swaps one container for another
    // must not have the new entity clobbered by the old one's removal.
    if (previous.block() == world::Block::Chest) {
        const auto removed = session_->chestSystem().remove({pos.x, pos.y, pos.z});
        if (removed.has_value()) {
            std::size_t dropIndex = 0U;
            for (const auto& stack : removed->items) {
                if (!stack.empty()) {
                    scatterContents(pos, stack, dropIndex++);
                }
            }
        }
    } else if (previous.block() == world::Block::Furnace) {
        // A broken furnace scatters its three slots, exactly as a chest
        // scatters its inventory.
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

    if (current.block() == world::Block::Chest) {
        static_cast<void>(session_->chestSystem().place({pos.x, pos.y, pos.z}));
    } else if (current.block() == world::Block::Furnace) {
        // Give the furnace its block entity immediately so it smelts even
        // before its screen is first opened.
        static_cast<void>(session_->furnaceSystem().place({pos.x, pos.y, pos.z}));
    }
}

void GameplayMutationSink::onNeighborChanged(world::BlockPos neighbor, world::BlockPos source) {
    static_cast<void>(neighbor); // see notifiedSource_: the reaction is source-centric
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
    static_cast<void>(cause);
    session_->spawnBlockDrops(toVector(pos), removed,
                              dropTool_ != nullptr ? *dropTool_ : emptyTool());
}

} // namespace mc::gameplay

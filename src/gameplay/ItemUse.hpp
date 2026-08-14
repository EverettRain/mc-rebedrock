#pragma once

#include "gameplay/GameMode.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"

#include <cstdint>

// Forward declarations at global scope: World and PlacementContext live in the
// world layer (BlockPlacement.hpp) and are only ever passed by reference here,
// so this header stays free of glm and the world sources.
namespace mc {
namespace world {
class World;
struct PlacementContext;
} // namespace world
} // namespace mc

namespace mc::gameplay {

// ServerPlayerGameMode#useItemOn's first decision:
//
//     suppressUsingBlock = player.isSecondaryUseActive() && haveSomethingInOurHands
//
// Sneaking with an item in hand means "build against this block", not "open
// it" — it is the only way to place a block onto a chest or a furnace. Stated
// here rather than inline in the renderer so it is one rule with one test,
// instead of a condition buried in a switch nobody can exercise.
[[nodiscard]] constexpr bool blockInteractionSuppressed(bool secondaryUseActive,
                                                        bool holdingItem) {
    return secondaryUseActive && holdingItem;
}

// hasInfiniteMaterials: whether the game mode restores the held stack after an
// item has used itself. 26.1 centralises the creative protection in the game
// mode (save, run, restore) instead of making each item ask whether it may
// modify the world — which is why the empty bucket used to be swapped for a
// full one even in creative.
[[nodiscard]] constexpr bool restoresHeldStack(GameMode mode) {
    return mode == GameMode::Creative;
}


// 26.1's InteractionResult (`world/InteractionResult.java`) is a sealed
// interface over four records: Success{swingSource, itemContext}, Fail, Pass
// and TryEmptyHandInteraction. The shape that matters is the four outcomes and
// the two things Success carries; a Java sealed hierarchy in C++ would be a
// virtual call and an allocation per right-click for the same information a
// tagged POD holds in four bytes.
enum class InteractionKind : std::uint8_t {
    // Nothing happened and nothing else should be tried.
    Fail,
    // This handler declines; the caller may try the next one.
    Pass,
    // The item-aware interaction declined specifically because the item had
    // nothing to say — try the block's plain, item-less interaction (in this
    // game, opening its container).
    TryEmptyHand,
    // Something happened.
    Success,
};

// InteractionResult.SwingSource: whether the arm swings, and who decided.
// There is no server here, so the distinction that survives is "swing" versus
// "consume silently" — vanilla's CONSUME is a Success with SwingSource.NONE.
enum class SwingSource : std::uint8_t {
    None,
    Client,
};

struct InteractionResult final {
    InteractionKind kind = InteractionKind::Pass;
    SwingSource swing = SwingSource::None;
    // InteractionResult.ItemContext#wasItemInteraction: false for a Success the
    // block produced on its own, which is how vanilla keeps a container opening
    // from counting as using the held item.
    bool wasItemInteraction = false;

    [[nodiscard]] constexpr bool consumesAction() const {
        return kind == InteractionKind::Success;
    }

    [[nodiscard]] static constexpr InteractionResult success() {
        return {InteractionKind::Success, SwingSource::Client, true};
    }
    [[nodiscard]] static constexpr InteractionResult successWithoutItem() {
        return {InteractionKind::Success, SwingSource::Client, false};
    }
    [[nodiscard]] static constexpr InteractionResult consume() {
        return {InteractionKind::Success, SwingSource::None, true};
    }
    [[nodiscard]] static constexpr InteractionResult fail() {
        return {InteractionKind::Fail, SwingSource::None, false};
    }
    [[nodiscard]] static constexpr InteractionResult pass() {
        return {InteractionKind::Pass, SwingSource::None, false};
    }
    [[nodiscard]] static constexpr InteractionResult tryEmptyHand() {
        return {InteractionKind::TryEmptyHand, SwingSource::None, false};
    }
};

// What the game does with a right-click on a block, once the two rules above
// have been applied. This is the decision that used to be spelled as
// `switch (suppressBlockUse ? ContainerType::None : definition.container)`
// inside the renderer's input loop, where nothing could reach it.
enum class BlockInteraction : std::uint8_t {
    // No block interaction: fall through to the held item's own use.
    UseItem,
    OpenCraftingTable,
    OpenFurnace,
    OpenChest,
};

// ServerPlayerGameMode#useItemOn's ordering, as a pure decision:
//
//   sneaking with something in hand  -> the block is a surface to build on
//   the block opens a container      -> open it, without using the item
//   otherwise                        -> the item decides
//
// Returning the interaction *and* the vanilla result lets the caller both act
// and know whether the arm should swing, without re-deriving either.
struct BlockInteractionDecision final {
    BlockInteraction interaction = BlockInteraction::UseItem;
    InteractionResult result = InteractionResult::tryEmptyHand();
};

[[nodiscard]] constexpr BlockInteractionDecision decideBlockInteraction(
    world::ContainerType container,
    bool secondaryUseActive,
    bool holdingItem) {
    if (blockInteractionSuppressed(secondaryUseActive, holdingItem)) {
        return {BlockInteraction::UseItem, InteractionResult::tryEmptyHand()};
    }
    switch (container) {
    case world::ContainerType::CraftingTable:
        // A container opening is a Success the block produced, not an item
        // interaction — vanilla's useWithoutItem path.
        return {BlockInteraction::OpenCraftingTable, InteractionResult::successWithoutItem()};
    case world::ContainerType::Furnace:
        return {BlockInteraction::OpenFurnace, InteractionResult::successWithoutItem()};
    case world::ContainerType::Chest:
        return {BlockInteraction::OpenChest, InteractionResult::successWithoutItem()};
    case world::ContainerType::None:
        break;
    }
    return {BlockInteraction::UseItem, InteractionResult::tryEmptyHand()};
}

// The Item class is defined in Item.hpp; the useOn function type only passes a
// pointer to it.
class Item;

// What the held item's right-click resolved to (vanilla Item#useOn). The item
// subclass decides the outcome; the interaction system owns the world-edit,
// audio and animation side effects it triggers.
enum class ItemUseAction : std::uint8_t {
    Nothing,
    // PlaceBlock: result.block and result.orientation name the placed state.
    PlaceBlock,
    // Fluid bucket interactions. Lava currently places/collects the project's
    // source block for visual verification; full lava flow simulation is a
    // separate feature from this item action.
    PlaceWater,
    CollectWater,
    PlaceLava,
    CollectLava,
    // SpawnEntity: the spawn egg is re-read for the entity type to spawn.
    SpawnEntity,
    // TilGround: the hoe converts the *clicked* block in place (dirt/grass/
    // podzol to farmland, coarse dirt to dirt). result.block names the new block.
    TilGround,
};

struct ItemUseResult final {
    ItemUseAction action = ItemUseAction::Nothing;
    // The state to place, for the PlaceBlock action. One value rather than a
    // block plus a loose orientation: a placed leaf block has to say it is
    // PERSISTENT, and that is a property of the state, not a direction.
    world::BlockState state{};
};

// Item#useOn (1.16.1): one function pointer per item class, so the interaction
// system calls the item instead of switching on it. Constexpr items cannot be
// virtual, so the behaviour is a function pointer set by the useAction() chain
// for ordinary items — the same pattern as SpawnEggItem's entity supplier.
// Built-in items dispatch through gameplay::itemUseOn (ItemPlacement.hpp) by
// their class; this slot lets a custom item override that with its own handler.
using ItemUseFn = ItemUseResult (*)(
    const Item* item, world::World& world, const world::PlacementContext& context);

} // namespace mc::gameplay

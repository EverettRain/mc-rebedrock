#pragma once

#include "gameplay/Inventory.hpp"
#include "world/BlockState.hpp"
#include "world/BlockPlacement.hpp"

#include <optional>

namespace mc::gameplay {

// BlockItem#useOn → getPlacementState. Resolves the block a held stack actually
// places, dispatching on the item's subclass: StandingAndWallBlockItem resolves
// the wall/standing torch variants, a plain BlockItem places its own block, and
// a legacy block stack (null item) places the stack's block. Returns nullopt
// when nothing can survive at the placement position.
[[nodiscard]] std::optional<world::BlockState> itemPlacementBlock(
    const world::World& world,
    const ItemStack& stack,
    const world::PlacementContext& context);

// Item#useOn: the single place right-click behaviour is resolved, dispatching on
// the held item's class — a spawn egg spawns, a block item places (with its
// subclass resolving the variant), the buckets collect or pour water. A custom
// item's useOn slot (Item::useAction) wins over the built-in classes. Returns
// Nothing when the item has no right-click behaviour here.
[[nodiscard]] ItemUseResult itemUseOn(
    const Item* item,
    world::World& world,
    const world::PlacementContext& context);

// The useOn for a legacy block stack (null item sentinel): resolves it to its
// canonical block item and places like BlockItem#useOn, so a stack that never
// carried its BlockItem still behaves exactly like a live one.
[[nodiscard]] ItemUseResult legacyBlockStackUseOn(
    const ItemStack& stack,
    world::World& world,
    const world::PlacementContext& context);

// The crop a seed item plants (wheat_seeds -> wheat, carrot -> carrots,
// potato -> potatoes), or Air for anything that is not a seed.
[[nodiscard]] world::Block cropForSeedItem(const Item* seed);

// The produce and seed items a crop species drops and is re-planted by
// (wheat -> wheat + wheat_seeds, carrots -> carrot, potatoes -> potato).
[[nodiscard]] const Item* produceForCrop(world::Block crop);
[[nodiscard]] const Item* seedForCrop(world::Block crop);

} // namespace mc::gameplay

#pragma once

#include "world/Block.hpp"

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
    // PlaceWater / CollectWater: the bucket interactions.
    PlaceWater,
    CollectWater,
    // SpawnEntity: the spawn egg is re-read for the entity type to spawn.
    SpawnEntity,
    // TilGround: the hoe converts the *clicked* block in place (dirt/grass/
    // podzol to farmland, coarse dirt to dirt). result.block names the new block.
    TilGround,
};

struct ItemUseResult final {
    ItemUseAction action = ItemUseAction::Nothing;
    world::Block block = world::Block::Air;
    world::BlockOrientation orientation = world::BlockOrientation::North;
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

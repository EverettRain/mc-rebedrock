#include "gameplay/ItemPlacement.hpp"

#include "gameplay/WorldSimulation.hpp"
#include "world/World.hpp"

namespace mc::gameplay {
namespace {

// LeavesBlockItem#place: hand-placed leaves are PERSISTENT so they never decay.
// ReBedrock stores the flag in the block's own orientation state, so the item
// resolves the orientation a placed leaves block should carry. Every other block
// defers to the block-property rules (log axis, facing back at the player).
[[nodiscard]] world::BlockOrientation itemPlacementOrientation(
    const ItemStack& stack,
    world::Block placed,
    const world::PlacementContext& context) {
    if (world::isLeaves(placed) &&
        (asLeavesBlockItem(stack.item) != nullptr || stack.item == nullptr)) {
        return world::kPersistentLeavesState;
    }
    return world::placementOrientation(placed, context);
}

// BlockItem#useOn → place: resolves the placed block state and its orientation,
// or Nothing when nothing can survive there.
[[nodiscard]] ItemUseResult placeBlockResult(
    const Item* item,
    world::Block block,
    world::World& world,
    const world::PlacementContext& context) {
    const ItemStack stack{block, 1U, item};
    const auto placed = itemPlacementBlock(world, stack, context);
    if (!placed.has_value()) {
        return {};
    }
    ItemUseResult result;
    result.action = ItemUseAction::PlaceBlock;
    result.block = *placed;
    result.orientation = itemPlacementOrientation(stack, *placed, context);
    return result;
}

[[nodiscard]] ItemUseResult blockItemUseOn(
    const Item* item, world::World& world, const world::PlacementContext& context) {
    const auto* blockItem = asBlockItem(item);
    if (blockItem == nullptr) {
        return {};
    }
    return placeBlockResult(blockItem, blockItem->block(), world, context);
}

[[nodiscard]] ItemUseResult spawnEggItemUseOn(
    const Item* item, world::World&, const world::PlacementContext&) {
    // The egg always resolves to "spawn here"; the renderer still checks the
    // species' model is loaded before the creature appears, and consumes the
    // egg in survival.
    return asSpawnEgg(item) != nullptr
        ? ItemUseResult{ItemUseAction::SpawnEntity}
        : ItemUseResult{};
}

[[nodiscard]] ItemUseResult bucketCollectUseOn(
    const Item*, world::World& world, const world::PlacementContext& context) {
    // BucketItem#useOn on a water source: only a still water block is collectable.
    const auto source = context.clickedBlock;
    return isCollectableWaterSource(world, {source.x, source.y, source.z})
        ? ItemUseResult{ItemUseAction::CollectWater}
        : ItemUseResult{};
}

[[nodiscard]] ItemUseResult bucketPlaceUseOn(
    const Item*, world::World& world, const world::PlacementContext& context) {
    // WaterBucketItem#useOn: water pours into a replaceable cell, washing away
    // the decoration blocks it covers.
    const auto target = context.placePosition;
    const auto existing = world.block(target.x, target.y, target.z);
    return (world::isReplaceable(existing) || world::isDestroyedByFluid(existing))
        ? ItemUseResult{ItemUseAction::PlaceWater}
        : ItemUseResult{};
}

// SeedsItem#useOn: a seed plants its crop on the farmland under the placement
// cell. Right-clicking the top of farmland resolves the placement cell to the
// cell above it (the interaction system's replaceable check), so the block
// below that cell is where the seed's crop must find its farmland — the same
// canSurvive check the placement path would run for the crop block.
[[nodiscard]] ItemUseResult plantCropOnFarmland(
    const Item* item, world::World& world, const world::PlacementContext& context) {
    const auto crop = cropForSeedItem(item);
    if (crop == world::Block::Air) {
        return {};
    }
    const auto below = context.placePosition - glm::ivec3{0, 1, 0};
    if (below.y < 0 ||
        !world::isFarmland(world.block(below.x, below.y, below.z))) {
        return {};
    }
    // A freshly planted crop starts at age 0.
    return {ItemUseAction::PlaceBlock, crop, world::cropOrientation(0)};
}

// HoeItem#useOn → setTilledAndGetDrop: the clicked dirt-family block converts in
// place. Dirt, grass and podzol become farmland; coarse dirt becomes dirt (the
// re-till step of the vanilla HOE_LOOKUP map).
[[nodiscard]] ItemUseResult tillBlockWithHoe(
    const Item*, world::World& world, const world::PlacementContext& context) {
    const auto target = context.clickedBlock;
    const auto block = world.block(target.x, target.y, target.z);
    world::Block tilled = world::Block::Air;
    switch (block) {
    case world::Block::Dirt:
    case world::Block::Grass:
    case world::Block::Podzol:
        tilled = world::Block::Farmland;
        break;
    case world::Block::CoarseDirt:
        tilled = world::Block::Dirt;
        break;
    default:
        return {};
    }
    return {ItemUseAction::TilGround, tilled, world::BlockOrientation::North};
}

} // namespace

std::optional<world::Block> itemPlacementBlock(
    const world::World& world,
    const ItemStack& stack,
    const world::PlacementContext& context) {
    const auto* blockItem = asBlockItem(stack.item);
    if (blockItem == nullptr) {
        // Legacy block stack: the null item pointer is the block sentinel, so
        // the stack places its own block under the canonical block item.
        if (stack.item != nullptr || stack.block == world::Block::Air) {
            return std::nullopt;
        }
        blockItem = blockItemFor(stack.block);
    }
    if (asStandingAndWallBlockItem(blockItem) != nullptr) {
        return world::standingAndWallPlacement(world, context);
    }
    return world::placementBlock(world, blockItem->block(), context);
}

ItemUseResult itemUseOn(
    const Item* item, world::World& world, const world::PlacementContext& context) {
    if (item == nullptr) {
        return {};
    }
    // A custom item's own handler wins over the built-in classes.
    if (item->useOn != nullptr) {
        return item->useOn(item, world, context);
    }
    // The built-in behaviours, dispatched by the item's class.
    if (asSpawnEgg(item) != nullptr) {
        return spawnEggItemUseOn(item, world, context);
    }
    if (asBlockItem(item) != nullptr) {
        return blockItemUseOn(item, world, context);
    }
    if (item == &items::Bucket) {
        return bucketCollectUseOn(item, world, context);
    }
    if (item == &items::WaterBucket) {
        return bucketPlaceUseOn(item, world, context);
    }
    // The seed items (SeedsItem subclasses): wheat seeds, carrot and potato plant
    // their crop on the farmland under the placement cell.
    if (cropForSeedItem(item) != world::Block::Air) {
        return plantCropOnFarmland(item, world, context);
    }
    // HoeItem#useOn: any hoe tills dirt-family blocks.
    if (item->toolType == ToolType::Hoe) {
        return tillBlockWithHoe(item, world, context);
    }
    return {};
}

ItemUseResult legacyBlockStackUseOn(
    const ItemStack& stack, world::World& world, const world::PlacementContext& context) {
    if (stack.item != nullptr || stack.block == world::Block::Air) {
        return {};
    }
    return placeBlockResult(blockItemFor(stack.block), stack.block, world, context);
}

world::Block cropForSeedItem(const Item* seed) {
    if (seed == &items::WheatSeeds) return world::Block::WheatCrops;
    if (seed == &items::Carrot) return world::Block::Carrots;
    if (seed == &items::Potato) return world::Block::Potatoes;
    return world::Block::Air;
}

const Item* produceForCrop(world::Block crop) {
    switch (crop) {
    case world::Block::WheatCrops:
        return &items::Wheat;
    case world::Block::Carrots:
        return &items::Carrot;
    case world::Block::Potatoes:
        return &items::Potato;
    default:
        return nullptr;
    }
}

const Item* seedForCrop(world::Block crop) {
    switch (crop) {
    case world::Block::WheatCrops:
        return &items::WheatSeeds;
    case world::Block::Carrots:
        return &items::Carrot;
    case world::Block::Potatoes:
        return &items::Potato;
    default:
        return nullptr;
    }
}

} // namespace mc::gameplay

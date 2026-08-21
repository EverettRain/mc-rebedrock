#include "gameplay/ItemPlacement.hpp"

#include "gameplay/ItemBehavior.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "gameplay/WorldSimulation.hpp"
#include "world/World.hpp"

#include <cstddef>
#include <vector>

namespace mc::gameplay {
namespace {

// LeavesBlockItem#place: hand-placed leaves are PERSISTENT so they never decay.
// Everything else keeps the state the placement rules resolved — a wall torch's
// facing comes from the wall it found and cannot be recomputed from the block
// alone. The context is not read: placementBlock resolved the facing already.
[[nodiscard]] world::BlockState itemPlacementState(
    const ItemStack& stack,
    world::BlockState placed) {
    if (world::isLeaves(placed.block()) &&
        (asLeavesBlockItem(stack.item) != nullptr || stack.item == nullptr)) {
        return placed.withPersistent(true);
    }
    return placed;
}

// BlockItem#useOn → place: resolves the state to place, or Nothing when nothing
// can survive there.
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
    return {ItemUseAction::PlaceBlock, itemPlacementState(stack, *placed)};
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
    // BucketItem#useOn: water must be a still source; the current lava
    // implementation has source blocks only, so every lava cell is collectable.
    const auto source = context.clickedBlock;
    if (world.block(source.x, source.y, source.z) == world::Block::Lava) {
        return {ItemUseAction::CollectLava};
    }
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

[[nodiscard]] ItemUseResult lavaBucketPlaceUseOn(
    const Item*, world::World& world, const world::PlacementContext& context) {
    const auto target = context.placePosition;
    const auto existing = world.block(target.x, target.y, target.z);
    return (world::isReplaceable(existing) || world::isDestroyedByFluid(existing))
        ? ItemUseResult{ItemUseAction::PlaceLava}
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
    // A freshly planted crop starts at age 0, which is the block's default
    // state — the age property needs no explicit write.
    return {ItemUseAction::PlaceBlock, world::BlockState{crop}};
}

// HoeItem#useOn → setTilledAndGetDrop: the block a hoe turns `block` into, or Air
// when a hoe does nothing to it. Dirt, grass and podzol become farmland; coarse
// dirt becomes dirt (the re-till step of the vanilla HOE_LOOKUP map). Expressed
// as a predicate chain rather than a switch on identity, like the block traits.
[[nodiscard]] world::Block hoeTilledForm(world::Block block) {
    using world::Block;
    if (block == Block::Dirt || block == Block::Grass || block == Block::Podzol) {
        return Block::Farmland;
    }
    if (block == Block::CoarseDirt) {
        return Block::Dirt;
    }
    return Block::Air;
}

// HoeItem#useOn → setTilledAndGetDrop: the clicked dirt-family block converts in
// place; anything else is left untouched.
[[nodiscard]] ItemUseResult tillBlockWithHoe(
    const Item*, world::World& world, const world::PlacementContext& context) {
    const auto target = context.clickedBlock;
    const auto tilled = hoeTilledForm(world.block(target.x, target.y, target.z));
    if (tilled == world::Block::Air) {
        return {};
    }
    return {ItemUseAction::TilGround, world::BlockState{tilled}};
}

// The useOn handler for a registered item, classified once at table build. This
// is the old itemUseOn `if`-chain, run at registration time so the runtime path
// is a table lookup rather than a per-click scan. Block items and a custom
// Item::useAction override are handled in itemUseOn itself (a block item is not a
// registry entry, and a custom override wins over its class), so they are absent
// here: this classifies only the built-in item classes.
[[nodiscard]] ItemUseFn itemUseOnSlot(const Item* item) {
    if (asSpawnEgg(item) != nullptr) {
        return spawnEggItemUseOn;
    }
    if (item == &items::Bucket) {
        return bucketCollectUseOn;
    }
    if (item == &items::WaterBucket) {
        return bucketPlaceUseOn;
    }
    if (item == &items::LavaBucket) {
        return lavaBucketPlaceUseOn;
    }
    if (cropForSeedItem(item) != world::Block::Air) {
        return plantCropOnFarmland;
    }
    if (item->toolType == ToolType::Hoe) {
        return tillBlockWithHoe;
    }
    return nullptr;
}

// Builds the runtime behaviour table, sized to the frozen item registry and
// indexed by ItemId — the same shape buildBlockBehaviorTable gives blocks. Every
// registered item takes its classified useOn slot and the matching pre-filter
// bit; an item that does nothing on right-click stays an empty entry.
[[nodiscard]] std::vector<ItemBehavior> buildItemBehaviorTable() {
    const auto& registry = itemRegistry();
    std::vector<ItemBehavior> table(registry.size());
    for (std::size_t index = 0; index < table.size(); ++index) {
        const Item* item = registry.get(
            core::ItemId::of(static_cast<core::ItemId::Value>(index)));
        auto& entry = table[index];
        entry.useOn = itemUseOnSlot(item);
        entry.prefilter.set(ItemBehaviorBit::HasUseOn, entry.useOn != nullptr);
    }
    return table;
}

} // namespace

std::optional<world::BlockState> itemPlacementBlock(
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

const std::vector<ItemBehavior>& itemBehaviorTable() {
    static const std::vector<ItemBehavior> table = buildItemBehaviorTable();
    return table;
}

const ItemBehavior& itemBehaviorFor(core::ItemId id) {
    static const ItemBehavior empty{};
    const auto& table = itemBehaviorTable();
    const auto index = id.index();
    return id.valid() && index < table.size() ? table[index] : empty;
}

bool itemHasBehavior(core::ItemId id, ItemBehaviorBit bit) {
    return itemBehaviorFor(id).prefilter.has(bit);
}

ItemUseResult itemUseOn(
    const Item* item, world::World& world, const world::PlacementContext& context) {
    if (item == nullptr) {
        return {};
    }
    // A custom item's own handler wins over its class, and reaches items that
    // never registered (a bare custom Item), so it is checked ahead of the table.
    if (item->useOn != nullptr) {
        return item->useOn(item, world, context);
    }
    // A block item places its block; its identity is the block's, so it is not a
    // registry entry and dispatches on the block side rather than the item table.
    if (asBlockItem(item) != nullptr) {
        return blockItemUseOn(item, world, context);
    }
    // Every other built-in item resolves through the behaviour table by its
    // ItemId: one indexed load and a pre-filter bit, no per-click identity scan.
    const ItemBehavior& behavior = itemBehaviorFor(itemId(item));
    if (behavior.prefilter.has(ItemBehaviorBit::HasUseOn) && behavior.useOn != nullptr) {
        return behavior.useOn(item, world, context);
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

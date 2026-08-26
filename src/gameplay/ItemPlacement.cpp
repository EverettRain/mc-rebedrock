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

// DoorBlock#getHinge: the hinge side is picked by counting how many sturdy
// (full-cube) blocks flank each side of the placement, tie-broken by which
// half of the clicked cell the player aimed at. A neighbouring lower door half
// biases the same way (so two doors placed side by side hinge away from each
// other) unless the solid-block count already decided it. Ported field-for-
// field from the Java source (DoorBlock.java) rather than simplified, since
// the tie-break order is exactly what makes doors along a wall hinge the way
// vanilla players expect.
[[nodiscard]] world::DoorHinge doorHingeFor(const world::World& world,
                                            const world::PlacementContext& context) {
    using world::BlockOrientation;
    const auto pos = context.placePosition;
    const auto placeDirection = world::horizontalFacing(context.lookDirection);
    const glm::ivec3 above{pos.x, pos.y + 1, pos.z};

    const auto leftDirection = world::counterClockwiseOrientation(placeDirection);
    const auto leftOffset = world::orientationOffset(leftDirection);
    const glm::ivec3 leftPos = pos + leftOffset;
    const glm::ivec3 leftAbovePos = above + leftOffset;
    const auto rightDirection = world::clockwiseOrientation(placeDirection);
    const auto rightOffset = world::orientationOffset(rightDirection);
    const glm::ivec3 rightPos = pos + rightOffset;
    const glm::ivec3 rightAbovePos = above + rightOffset;

    const auto sturdy = [&world](glm::ivec3 p) {
        return world::isFaceSturdy(world.block(p.x, p.y, p.z));
    };
    const int solidBlockBalance = (sturdy(leftPos) ? -1 : 0) + (sturdy(leftAbovePos) ? -1 : 0) +
                                  (sturdy(rightPos) ? 1 : 0) + (sturdy(rightAbovePos) ? 1 : 0);

    const auto isLowerDoor = [&world](glm::ivec3 p) {
        const auto state = world.state(p.x, p.y, p.z);
        return world::blockDefinition(state.block()).model == world::BlockModel::Door &&
               !state.isDoorUpperHalf();
    };
    const bool doorLeft = isLowerDoor(leftPos);
    const bool doorRight = isLowerDoor(rightPos);

    if ((!doorLeft || doorRight) && solidBlockBalance <= 0) {
        if ((!doorRight || doorLeft) && solidBlockBalance >= 0) {
            // The click's sub-cell position within the placement cell breaks the
            // remaining tie, mirroring the Java stepX/stepZ comparison exactly.
            const auto offset = world::orientationOffset(placeDirection);
            const float clickX = context.hitPosition.x - static_cast<float>(pos.x);
            const float clickZ = context.hitPosition.z - static_cast<float>(pos.z);
            const bool left = (offset.x >= 0 || !(clickZ < 0.5F)) &&
                (offset.x <= 0 || !(clickZ > 0.5F)) &&
                (offset.z >= 0 || !(clickX > 0.5F)) &&
                (offset.z <= 0 || !(clickX < 0.5F));
            return left ? world::DoorHinge::Left : world::DoorHinge::Right;
        }
        return world::DoorHinge::Left;
    }
    return world::DoorHinge::Right;
}

// DoorBlock#getStateForPlacement: resolves the *lower* half's state (facing +
// hinge), or Nothing when the cell above cannot take the upper half, and when
// the placement cell itself cannot survive (needs sturdy ground below, the
// same BlockSupport::Ground rule an ordinary block's canBlockSurvive already
// answers, since a door declares no support flag of its own to avoid the
// generic support table dispatching a wall-torch-style facing read). The
// upper half itself, and writing both cells, are the caller's job
// (PlayerInteraction.cpp) — this stays a pure "what would the lower half be"
// query so it can be probed the same way every other placement decision is.
[[nodiscard]] ItemUseResult doorPlaceResult(world::Block block, world::World& world,
                                            const world::PlacementContext& context) {
    const auto pos = context.placePosition;
    const glm::ivec3 above{pos.x, pos.y + 1, pos.z};
    if (!world::isReplaceable(world.block(above.x, above.y, above.z))) {
        return {};
    }
    if (!world::isFaceSturdy(world.block(pos.x, pos.y - 1, pos.z))) {
        return {}; // DoorBlock#canSurvive: the lower half needs sturdy ground
    }
    const auto facing = world::horizontalFacing(context.lookDirection);
    const auto hinge = doorHingeFor(world, context);
    const auto lower = world::BlockState{block, facing}.withHinge(hinge).withDoorUpperHalf(false);
    return {ItemUseAction::PlaceDoor, lower};
}

[[nodiscard]] ItemUseResult blockItemUseOn(
    const Item* item, world::World& world, const world::PlacementContext& context) {
    // AR-B2: a door places two cells atomically, so its own kind routes to
    // doorPlaceResult ahead of the ordinary single-cell path.
    if (const auto* doorItem = asDoorBlockItem(item); doorItem != nullptr) {
        return doorPlaceResult(doorItem->block(), world, context);
    }
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
    if (isCollectableWaterSource(world, {source.x, source.y, source.z})) {
        return {ItemUseAction::CollectWater};
    }
    // BucketPickup#pickupBlock's other branch (F2): the clicked block is not
    // itself water, but it may be carrying a parasitic source on its
    // SubmergedFluid axis (a wet slab) — the empty bucket takes that instead,
    // and the slab stays behind dry rather than being replaced.
    if (world::canBeSubmerged(world.block(source.x, source.y, source.z)) &&
        world.state(source.x, source.y, source.z).submergedFluid() ==
            world::SubmergedFluid::Water) {
        return {ItemUseAction::CollectSubmergedWater};
    }
    return {};
}

[[nodiscard]] ItemUseResult bucketPlaceUseOn(
    const Item*, world::World& world, const world::PlacementContext& context) {
    // BucketItem#useOn / LiquidBlockContainer (F2): a water bucket used
    // directly on a dry submergible block wets it in place, before falling
    // back to WaterBucketItem#useOn's ordinary "pour into the adjacent
    // replaceable cell" behaviour — the same clicked-before-adjacent order
    // vanilla's emptyContents dispatch uses.
    const auto clicked = context.clickedBlock;
    const auto clickedBlock = world.block(clicked.x, clicked.y, clicked.z);
    if (world::canBeSubmerged(clickedBlock) &&
        world.state(clicked.x, clicked.y, clicked.z).submergedFluid() ==
            world::SubmergedFluid::None) {
        return {ItemUseAction::SubmergeBlock};
    }
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

// AR-CX4-b: FlintAndSteelItem#useOn → BaseFireBlock.getState/setBlockState. Fire
// is placed in the cell adjacent to the clicked face (the same replaceable cell
// PlaceBlock targets), but only when a Fire block would actually survive there
// (FireBlock#canSurvive: sturdy floor below, or a flammable neighbour) — a lit
// tool clicked at empty air over nothing does nothing, exactly like vanilla.
[[nodiscard]] ItemUseResult igniteWithFlintAndSteel(
    const Item*, world::World& world, const world::PlacementContext& context) {
    const auto target = context.placePosition;
    if (!world::isReplaceable(world.block(target.x, target.y, target.z))) {
        return {};
    }
    if (!world::canBlockSurvive(world, target, world::Block::Fire,
                                world::BlockOrientation::Up)) {
        return {};
    }
    return {ItemUseAction::PlaceFire, world::BlockState{world::Block::Fire}};
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
    if (item->toolType == ToolType::FlintAndSteel) {
        return igniteWithFlintAndSteel;
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
    if (const auto* standingAndWall = asStandingAndWallBlockItem(blockItem);
        standingAndWall != nullptr) {
        return world::standingAndWallPlacement(world, context, standingAndWall->block(),
                                               standingAndWall->wallBlock());
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
    // AR-B2: a legacy door stack (a null item pointer naming a door block)
    // still routes through the two-cell placement, matching the live-item path.
    if (world::blockDefinition(stack.block).model == world::BlockModel::Door) {
        return doorPlaceResult(stack.block, world, context);
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

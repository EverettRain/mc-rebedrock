// I1-1 acceptance: item identity flows through the ItemRegistry (round-trip
// byName), a BlockItem references its block by BlockId, and item right-click
// behaviour dispatches through the ItemBehavior table (the item-side twin of
// BlockBehavior) rather than the old per-click identity chain.

#include "gameplay/ItemBehavior.hpp"
#include "gameplay/ItemPlacement.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "gameplay/SpawnEggItems.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"
#include "world/BlockPlacement.hpp"
#include "world/World.hpp"

#include <cassert>
#include <string>

using namespace mc;
using namespace mc::gameplay;

namespace {

// A registered item resolves to a valid, stable id, and every name form it
// answers to (the rebedrock key, the minecraft alias, the bare path) lands on
// that same id — the round-trip the identity layer rests on.
void checkRoundTrip(const Item* item) {
    const core::ItemId id = itemId(item);
    assert(id.valid());
    // id -> Item and Item -> id agree.
    assert(itemFromId(id) == item);
    // The canonical name the registry stored is the item's own identifier.
    assert(itemRegistry().identifier(id) == item->identifier);
    // Name -> id, through the rebedrock key and the bare path.
    assert(itemRegistry().byName(item->identifier) == id);
    assert(itemRegistry().byName(item->identifier.path) == id);
    // itemFromIdentifier is a view over the registry.
    assert(itemFromIdentifier(item->identifier.toString()) == item);
    assert(itemFromIdentifier(std::string{item->identifier.path}) == item);
    // The vanilla alias, when the item has one, resolves to the same id.
    if (!item->vanillaAlias.empty() && item->vanillaAlias != item->identifier) {
        assert(itemRegistry().byName(item->vanillaAlias) == id);
        assert(itemFromIdentifier(item->vanillaAlias.toString()) == item);
    }
}

void buildFloor(world::World& world) {
    world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, world::Block::Stone);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
}

} // namespace

int main() {
    // Spawn eggs register through the runtime slot at static-init; entities must
    // exist for their suppliers, mirroring the app's startup order.
    entities::registerBuiltinEntities();

    // --- Round-trip byName over every registered item, incl. spawn eggs. ---
    for (const Item* item : kItemRegistry) {
        checkRoundTrip(item);
    }
    for (const Item* egg : kSpawnEggItems) {
        checkRoundTrip(egg);
    }
    // The registry size covers the built-in items plus the runtime-slot eggs.
    assert(itemRegistry().size() == kItemRegistry.size() + extraItemRegistry().size());
    // An unknown name resolves to no id and no item.
    assert(!itemRegistry().byName("rebedrock:not_an_item").valid());
    assert(itemFromIdentifier("rebedrock:not_an_item") == nullptr);
    // A bare custom Item that never registered has no id.
    const Item widget = Item::of("test_widget").custom();
    assert(!itemId(&widget).valid());

    // --- BlockItem references its block by BlockId. ---
    {
        const Item* stone = blockItemFor(world::Block::Stone);
        const auto* blockItem = asBlockItem(stone);
        assert(blockItem != nullptr);
        assert(blockItem->block() == world::Block::Stone);
        assert(blockItem->blockId() == world::blockId(world::Block::Stone));
        // The torch is a StandingAndWallBlockItem: both variants keep their ids.
        const Item* torch = blockItemFor(world::Block::Torch);
        const auto* saw = asStandingAndWallBlockItem(torch);
        assert(saw != nullptr);
        assert(saw->block() == world::Block::Torch);
        assert(saw->wallBlock() == world::Block::WallTorch);
        // A block wielded as its BlockItem resolves through the block bridge, and
        // that item reports the right block — the `Items.STONE` beside
        // `Blocks.STONE` path, which is not a registry ItemId of its own.
        const Item* bridged = itemFromIdentifier("rebedrock:stone");
        assert(asBlockItem(bridged) != nullptr);
        assert(asBlockItem(bridged)->block() == world::Block::Stone);
    }

    // --- The behaviour table is sized to the registry and indexed by ItemId. ---
    assert(itemBehaviorTable().size() == itemRegistry().size());

    // Items with a right-click behaviour carry a useOn slot and the HasUseOn bit;
    // a plain material carries neither. This is the table that replaced the
    // per-click `if (item == &Bucket) … else if (toolType == Hoe) …` chain.
    const auto hasUseOn = [](const Item* item) {
        const ItemBehavior& behavior = itemBehaviorFor(itemId(item));
        const bool bit = behavior.prefilter.has(ItemBehaviorBit::HasUseOn);
        assert(bit == (behavior.useOn != nullptr));
        return bit;
    };
    assert(hasUseOn(&items::Bucket));
    assert(hasUseOn(&items::WaterBucket));
    assert(hasUseOn(&items::LavaBucket));
    assert(hasUseOn(&items::WheatSeeds));
    assert(hasUseOn(&items::Carrot));   // carrot is both food and a seed
    assert(hasUseOn(&items::WoodenHoe));
    assert(hasUseOn(&items::DiamondHoe));
    assert(hasUseOn(&items::PigSpawnEgg));
    // Plain materials / tools that are not hoes do nothing on right-click.
    assert(!hasUseOn(&items::Diamond));
    assert(!hasUseOn(&items::Stick));
    assert(!hasUseOn(&items::DiamondPickaxe));
    assert(!hasUseOn(&items::Apple));

    // --- Behaviour parity through the table: a hoe still tills dirt. This runs
    // the full itemUseOn path (table lookup -> slot), proving the unification
    // kept the behaviour, not just the wiring. ---
    {
        world::World world;
        buildFloor(world);
        world.setBlock(5, 1, 5, world::Block::Dirt);
        world::PlacementContext ctx;
        ctx.clickedBlock = {5, 1, 5};
        ctx.placePosition = {5, 2, 5};
        ctx.clickedFace = world::BlockOrientation::Up;
        const ItemUseResult tilled = itemUseOn(&items::WoodenHoe, world, ctx);
        assert(tilled.action == ItemUseAction::TilGround);
        assert(tilled.state.block() == world::Block::Farmland);
        // A non-hoe tool tills nothing through the same path.
        const ItemUseResult none = itemUseOn(&items::DiamondPickaxe, world, ctx);
        assert(none.action == ItemUseAction::Nothing);
    }

    // --- Behaviour parity: a seed plants its crop on farmland below the cell. ---
    {
        world::World world;
        buildFloor(world);
        world.setBlock(5, 1, 5, world::Block::Farmland);
        world::PlacementContext ctx;
        ctx.clickedBlock = {5, 1, 5};
        ctx.placePosition = {5, 2, 5}; // the cell above farmland
        ctx.clickedFace = world::BlockOrientation::Up;
        const ItemUseResult planted = itemUseOn(&items::WheatSeeds, world, ctx);
        assert(planted.action == ItemUseAction::PlaceBlock);
        assert(planted.state.block() == world::Block::WheatCrops);
    }

    // --- Behaviour parity: a block item places its block through itemUseOn,
    // reached by the block-side branch (block items are not registry entries). ---
    {
        world::World world;
        buildFloor(world);
        world::PlacementContext ctx;
        ctx.clickedBlock = {5, 0, 5};
        ctx.placePosition = {5, 1, 5};
        ctx.clickedFace = world::BlockOrientation::Up;
        const ItemUseResult placed =
            itemUseOn(blockItemFor(world::Block::Cobblestone), world, ctx);
        assert(placed.action == ItemUseAction::PlaceBlock);
        assert(placed.state.block() == world::Block::Cobblestone);
    }

    return 0;
}

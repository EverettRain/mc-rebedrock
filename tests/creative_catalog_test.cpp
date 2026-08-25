// AR-CI: the creative-catalog reachability guard.
#include <span>
//
// The whole point of AR-CI is that block catalog membership is data-driven off
// each block's own BlockDefinition::creativeCategory (BlockProperties::
// creative() in Block.hpp) rather than a parallel hand-maintained array in
// ContentRegistry.cpp. This test is the guard that keeps that true: every
// non-Hidden registered block must appear in exactly one catalog, the landed
// AR-B shaped blocks and W-4 redstone components (the ones the old three
// arrays silently dropped) must specifically be present, and a representative
// set of technical/placed-only blocks must specifically be absent.

#include "gameplay/ContentRegistry.hpp"
#include "world/Block.hpp"
#include "world/BlockRegistry.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>

namespace {

using namespace mc;
using namespace mc::gameplay;

// ItemStack::block defaults to Air for an *item* stack (Air is the "this stack
// is not a block" sentinel — see Inventory.hpp's isBlockStack), so a plain
// `stack.block == block` match would also catch every item stack when queried
// with Block::Air itself. registerBlock never stores Air (it rejects it
// outright), so guarding on `block != Air` here makes an Air query correctly
// find nothing, matching this test's own "Air is Hidden" assertion below.
bool inCatalog(const ContentRegistry& registry, CreativeCategory category, world::Block block) {
    if (block == world::Block::Air) return false;
    const auto catalog = registry.catalog(category);
    return std::ranges::any_of(
        catalog, [block](const ItemStack& stack) { return stack.block == block; });
}

// Is `block` present in *some* non-Hidden catalog (any tab)? Used for the
// reachability guard, which does not care which tab a block landed in, only
// that it landed in exactly one.
std::size_t catalogCountContaining(const ContentRegistry& registry, world::Block block) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < static_cast<std::size_t>(CreativeCategory::Count); ++index) {
        const auto category = static_cast<CreativeCategory>(index);
        if (inCatalog(registry, category, block)) ++count;
    }
    return count;
}

} // namespace

int main() {
    const auto& registry = contentRegistry();

    // --- Reachability guard: every non-Hidden registered block appears in
    // exactly one category catalog. ---
    const auto& blocks = world::blockRegistry();
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const auto id = core::BlockId::of(static_cast<core::BlockId::Value>(index));
        const world::BlockDefinition& definition = blocks.get(id);
        if (definition.block == world::Block::Air) continue; // never catalogued, by design
        const std::size_t count = catalogCountContaining(registry, definition.block);
        if (definition.creativeCategory == CreativeCategory::Hidden) {
            if (count != 0) {
                assert(false && "Hidden block unexpectedly appears in a creative catalog");
            }
        } else {
            if (count != 1) {
                // Name the offending block in the abort message's spirit: the
                // assert condition below embeds enough to grep the identifier
                // straight out of a core dump / the failing line.
                assert(count == 1 && "non-Hidden block must appear in exactly one catalog");
            }
        }
    }

    // --- AR-B shaped blocks explicitly present (the ones the old hand-array
    // silently dropped). ---
    assert(catalogCountContaining(registry, world::Block::OakStairs) == 1);
    assert(catalogCountContaining(registry, world::Block::OakSlab) == 1);
    assert(catalogCountContaining(registry, world::Block::OakDoor) == 1);
    assert(catalogCountContaining(registry, world::Block::OakFenceGate) == 1);
    assert(catalogCountContaining(registry, world::Block::OakTrapdoor) == 1);
    assert(catalogCountContaining(registry, world::Block::StonePressurePlate) == 1);
    assert(catalogCountContaining(registry, world::Block::CobblestoneWall) == 1);

    // --- W-4 redstone components explicitly present. ---
    assert(catalogCountContaining(registry, world::Block::RedstoneBlock) == 1);
    assert(catalogCountContaining(registry, world::Block::RedstoneTorch) == 1);
    assert(catalogCountContaining(registry, world::Block::Lever) == 1);
    assert(catalogCountContaining(registry, world::Block::Repeater) == 1);
    assert(catalogCountContaining(registry, world::Block::Comparator) == 1);
    assert(catalogCountContaining(registry, world::Block::RedstoneWire) == 1);
    assert(catalogCountContaining(registry, world::Block::Observer) == 1);
    assert(catalogCountContaining(registry, world::Block::StoneButton) == 1);
    assert(catalogCountContaining(registry, world::Block::Piston) == 1);
    assert(catalogCountContaining(registry, world::Block::StickyPiston) == 1);
    assert(catalogCountContaining(registry, world::Block::TrappedChest) == 1);

    // B7-0: the Redstone tab holds the eleven redstone components plus the
    // openables/actuators 26.1's REDSTONE_BLOCKS tab files there (doors, fence
    // gate, trapdoor, pressure plate) and TNT — sixteen in all.
    const auto redstoneTab = registry.catalog(CreativeCategory::Redstone);
    assert(redstoneTab.size() == 16);
    assert(inCatalog(registry, CreativeCategory::Redstone, world::Block::RedstoneBlock));
    assert(inCatalog(registry, CreativeCategory::Redstone, world::Block::RedstoneTorch));
    assert(inCatalog(registry, CreativeCategory::Redstone, world::Block::Lever));
    assert(inCatalog(registry, CreativeCategory::Redstone, world::Block::Repeater));
    assert(inCatalog(registry, CreativeCategory::Redstone, world::Block::Comparator));
    assert(inCatalog(registry, CreativeCategory::Redstone, world::Block::RedstoneWire));
    assert(inCatalog(registry, CreativeCategory::Redstone, world::Block::Observer));
    assert(inCatalog(registry, CreativeCategory::Redstone, world::Block::StoneButton));
    assert(inCatalog(registry, CreativeCategory::Redstone, world::Block::Piston));
    assert(inCatalog(registry, CreativeCategory::Redstone, world::Block::StickyPiston));
    assert(inCatalog(registry, CreativeCategory::Redstone, world::Block::TrappedChest));
    assert(inCatalog(registry, CreativeCategory::Redstone, world::Block::Tnt));
    assert(inCatalog(registry, CreativeCategory::Redstone, world::Block::OakDoor));
    assert(inCatalog(registry, CreativeCategory::Redstone, world::Block::OakFenceGate));
    assert(inCatalog(registry, CreativeCategory::Redstone, world::Block::OakTrapdoor));
    assert(inCatalog(registry, CreativeCategory::Redstone, world::Block::StonePressurePlate));
    // Ores are raw nature: RedstoneOre lives in Natural Blocks, not Redstone.
    assert(!inCatalog(registry, CreativeCategory::Redstone, world::Block::RedstoneOre));
    assert(inCatalog(registry, CreativeCategory::NaturalBlocks, world::Block::RedstoneOre));

    // --- No technical leakage: a representative set of technical/hidden
    // blocks are not in any catalog. ---
    assert(catalogCountContaining(registry, world::Block::Air) == 0);
    assert(catalogCountContaining(registry, world::Block::Water) == 0);
    assert(catalogCountContaining(registry, world::Block::Lava) == 0);
    assert(catalogCountContaining(registry, world::Block::WallTorch) == 0);
    assert(catalogCountContaining(registry, world::Block::RedstoneWallTorch) == 0);
    assert(catalogCountContaining(registry, world::Block::Farmland) == 0);
    assert(catalogCountContaining(registry, world::Block::WheatCrops) == 0);
    assert(catalogCountContaining(registry, world::Block::Carrots) == 0);
    assert(catalogCountContaining(registry, world::Block::Potatoes) == 0);
    // registry.block() must not resolve a Hidden block by name either — it was
    // never registered, so its identifier has no catalog entry to find.
    assert(registry.block("rebedrock:wall_torch") == nullptr);
    assert(registry.block("rebedrock:water") == nullptr);
    assert(registry.block("rebedrock:farmland") == nullptr);

    // --- B7-0: representative blocks land in their 26.1 tab. Building = processed
    // materials, Natural = raw nature (stone/dirt/ore/log/leaf/plant), Colored =
    // the 16-colour + glass families, Functional = utility blocks. ---
    const auto building = registry.catalog(CreativeCategory::BuildingBlocks);
    const auto natural = registry.catalog(CreativeCategory::NaturalBlocks);
    const auto colored = registry.catalog(CreativeCategory::ColoredBlocks);
    const auto functional = registry.catalog(CreativeCategory::Functional);
    const auto inTab = [](std::span<const ItemStack> tab, world::Block block) {
        return std::ranges::any_of(tab,
                                   [block](const ItemStack& s) { return s.block == block; });
    };
    const world::Block kBuilding[] = {
        world::Block::OakPlanks,     world::Block::Bricks,       world::Block::StoneBricks,
        world::Block::QuartzBlock,   world::Block::SmoothStone,  world::Block::PolishedGranite,
        world::Block::OakSlab,       world::Block::StoneSlab,    world::Block::OakStairs,
        world::Block::CobblestoneWall, world::Block::NetherBricks, world::Block::PurpurBlock,
    };
    for (const auto block : kBuilding) assert(inTab(building, block));
    const world::Block kNatural[] = {
        world::Block::Dirt,    world::Block::Stone,     world::Block::Cobblestone,
        world::Block::Sand,    world::Block::Gravel,    world::Block::OakLog,
        world::Block::CoalOre, world::Block::IronOre,   world::Block::DiamondOre,
        world::Block::OakLeaves, world::Block::OakSapling, world::Block::Dandelion,
        world::Block::GrassPlant, world::Block::Pumpkin, world::Block::SugarCane,
        world::Block::Glowstone, world::Block::Bedrock, world::Block::Granite,
    };
    for (const auto block : kNatural) assert(inTab(natural, block));
    const world::Block kColored[] = {
        world::Block::WhiteWool, world::Block::RedWool, world::Block::BlackWool,
        world::Block::GreenWool, world::Block::Glass,
    };
    for (const auto block : kColored) assert(inTab(colored, block));
    const world::Block kFunctional[] = {
        world::Block::CraftingTable, world::Block::Furnace, world::Block::Chest,
        world::Block::Torch,         world::Block::Bookshelf,
    };
    for (const auto block : kFunctional) assert(inTab(functional, block));

    // --- Determinism: catalog membership/order stable across two builds. ---
    ContentRegistry rebuilt;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const auto id = core::BlockId::of(static_cast<core::BlockId::Value>(index));
        const world::BlockDefinition& definition = blocks.get(id);
        if (definition.creativeCategory == CreativeCategory::Hidden) continue;
        rebuilt.registerBlock(definition.block, definition.creativeCategory);
    }
    const auto rebuiltBuilding = rebuilt.catalog(CreativeCategory::BuildingBlocks);
    assert(rebuiltBuilding.size() == building.size());
    for (std::size_t i = 0; i < building.size(); ++i) {
        assert(rebuiltBuilding[i].block == building[i].block);
    }
    const auto rebuiltRedstone = rebuilt.catalog(CreativeCategory::Redstone);
    assert(rebuiltRedstone.size() == redstoneTab.size());
    for (std::size_t i = 0; i < redstoneTab.size(); ++i) {
        assert(rebuiltRedstone[i].block == redstoneTab[i].block);
    }

    // --- Hidden and Count are both rejected by the low-level API. ---
    ContentRegistry isolated;
    assert(!isolated.registerBlock(world::Block::Stone, CreativeCategory::Hidden));
    assert(!isolated.registerBlock(world::Block::Stone, CreativeCategory::Count));
    assert(isolated.catalog(CreativeCategory::Hidden).empty());

    return 0;
}

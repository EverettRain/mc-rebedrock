#include "gameplay/Inventory.hpp"
#include "gameplay/MiningSystem.hpp"
#include "world/Block.hpp"

#include <cassert>

// The tool rules a block gets are supposed to fall out of its registry entry,
// not out of a second and third hand-kept list. Until B2' reads the vanilla
// mineable/needs_*_tool tags, MiningSystem keeps those lists by hand, and a
// block added to the registry without being added to them silently loses its
// pickaxe speed-up and its correct-tool drop rule. That is exactly what
// happened to the four polished stones and smooth stone, which the crafting
// and smelting tables already hand the player. This test pins the rule so the
// next block cannot drift the same way.
int main() {
    using namespace mc;
    using namespace mc::gameplay;

    const ItemStack bareHand{};
    const ItemStack woodenPickaxe{world::Block::Air, 1U, &items::WoodenPickaxe};
    const ItemStack woodenShovel{world::Block::Air, 1U, &items::WoodenShovel};

    // The stones the recipes hand out: polished granite/diorite/andesite come
    // from a 2x2 of their base stone, smooth stone from the furnace. All four
    // call requiresCorrectToolForDrops, and none of them sit in needs_stone_tool,
    // so the wooden tier is both necessary and sufficient.
    const world::Block craftedStones[] = {
        world::Block::PolishedGranite,
        world::Block::PolishedDiorite,
        world::Block::PolishedAndesite,
        world::Block::SmoothStone,
    };

    for (const auto stone : craftedStones) {
        const auto requirement = harvestRequirement(stone);
        assert(requirement.tool == ToolType::Pickaxe);
        assert(requirement.tier == ToolTier::Wood);

        // A bare hand still breaks the block, it just keeps nothing.
        assert(!canHarvestBlock(stone, bareHand));
        assert(canHarvestBlock(stone, woodenPickaxe));
        // The wrong tool is no better than a hand.
        assert(!canHarvestBlock(stone, woodenShovel));

        // Being in mineable/pickaxe is a separate concern from the drop rule:
        // it is what makes the pickaxe actually dig faster.
        const float handSeconds = miningSeconds(stone, bareHand, false, false);
        const float pickSeconds = miningSeconds(stone, woodenPickaxe, false, false);
        assert(pickSeconds < handSeconds);
        // The shovel earns no speed-up on stone.
        assert(miningSeconds(stone, woodenShovel, false, false) == handSeconds);
    }

    // The polished stones share their base stone's hardness, so they mine at
    // the same rate — a regression here means the registry and the tool list
    // disagree about which family they belong to.
    assert(miningSeconds(world::Block::PolishedGranite, woodenPickaxe, false, false) ==
           miningSeconds(world::Block::Granite, woodenPickaxe, false, false));

    // The burning furnace is now the plain furnace's lit *state*, not a second
    // block, so it cannot drift out of the tool lists at all — there is only
    // one furnace to keep in them. (It used to be a separate Block that was
    // missing from the pickaxe list, which made a lit furnace slower to break
    // than the very same furnace one tick earlier.)
    {
        const float hand = miningSeconds(world::Block::Furnace, bareHand, false, false);
        const float pick = miningSeconds(world::Block::Furnace, woodenPickaxe, false, false);
        assert(pick < hand);
    }

    return 0;
}

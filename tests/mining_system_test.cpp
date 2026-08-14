#include "gameplay/Inventory.hpp"
#include "gameplay/ItemUse.hpp"
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

    // --- B2': the rules above are now read off the block tags rather than off
    // five switch chains, and that fixed a real divergence from 26.1. Sandstone,
    // bricks, quartz, netherrack and the furnace sit in mineable/pickaxe, so
    // vanilla marks them requiresCorrectToolForDrops — but the old
    // harvestRequirement switch omitted them and let a bare hand keep their
    // drop. Tag-derived, they now need a pickaxe like every other stone. ---
    {
        const world::Block correctedStones[] = {
            world::Block::Sandstone,   world::Block::Bricks,   world::Block::QuartzBlock,
            world::Block::Netherrack,  world::Block::Furnace,
        };
        for (const auto block : correctedStones) {
            const auto requirement = harvestRequirement(block);
            assert(requirement.tool == ToolType::Pickaxe);
            assert(requirement.tier == ToolTier::Wood);
            assert(!canHarvestBlock(block, bareHand));
            assert(canHarvestBlock(block, woodenPickaxe));
        }
    }

    // Wood, dirt and leaves are mineable with their own tool but are *not*
    // requiresCorrectToolForDrops, so a bare hand must still keep them. Deriving
    // the drop gate from mineable/pickaxe alone is what preserves that; a
    // version that gated on "in any mineable tag" would take away hand-chopped
    // logs, which is the most obvious possible regression.
    {
        const world::Block handHarvestable[] = {
            world::Block::OakLog, world::Block::OakPlanks, world::Block::Dirt,
            world::Block::Sand,   world::Block::OakLeaves,
        };
        for (const auto block : handHarvestable) {
            assert(harvestRequirement(block).tool == ToolType::None);
            assert(canHarvestBlock(block, bareHand));
        }
        // The matching tool still digs them faster, which is the other half of
        // the tag data.
        const ItemStack woodenAxe{world::Block::Air, 1U, &items::WoodenAxe};
        assert(miningSeconds(world::Block::OakLog, woodenAxe, false, false) <
               miningSeconds(world::Block::OakLog, bareHand, false, false));
        assert(miningSeconds(world::Block::Dirt, woodenShovel, false, false) <
               miningSeconds(world::Block::Dirt, bareHand, false, false));
    }

    // The harvest tier comes from the needs_*_tool family, independently of
    // which tool mines the block.
    {
        assert(harvestRequirement(world::Block::IronOre).tier == ToolTier::Stone);
        assert(harvestRequirement(world::Block::DiamondOre).tier == ToolTier::Iron);
        assert(harvestRequirement(world::Block::Obsidian).tier == ToolTier::Diamond);
        assert(harvestRequirement(world::Block::Stone).tier == ToolTier::Wood);
    }

    // --- B3: the two 26.1 interaction rules, stated once in ItemUse.hpp and
    // exercised here. They used to be conditions buried in the renderer's
    // interaction switch, where nothing could reach them. ---
    {
        using gameplay::blockInteractionSuppressed;
        using gameplay::restoresHeldStack;

        // Sneaking with an item in hand builds against the block instead of
        // opening it — the only way to place onto a chest or a furnace.
        assert(blockInteractionSuppressed(true, true));
        // Sneaking empty-handed still opens: otherwise a crouched player could
        // never use a container at all.
        assert(!blockInteractionSuppressed(true, false));
        // Standing upright always opens, held item or not.
        assert(!blockInteractionSuppressed(false, true));
        assert(!blockInteractionSuppressed(false, false));

        // Creative restores the held stack after an item has used itself, which
        // is what stops the empty bucket being swapped for a full one.
        assert(restoresHeldStack(gameplay::GameMode::Creative));
        assert(!restoresHeldStack(gameplay::GameMode::Survival));
    }

    // --- B3: the whole right-click ordering as one pure decision, the shape
    // 26.1 spells as ServerPlayerGameMode#useItemOn. It used to be
    // `switch (suppressed ? None : definition.container)` inside the renderer's
    // input loop. ---
    {
        using gameplay::BlockInteraction;
        using gameplay::decideBlockInteraction;
        using gameplay::InteractionKind;
        using world::ContainerType;

        // A container under an upright player opens, and that is a Success the
        // *block* produced: vanilla's useWithoutItem path does not count as
        // using the held item, which is what keeps a container opening from
        // spending a durability point or triggering an item cooldown.
        const auto chest = decideBlockInteraction(ContainerType::Chest, false, true);
        assert(chest.interaction == BlockInteraction::OpenChest);
        assert(chest.result.kind == InteractionKind::Success);
        assert(chest.result.consumesAction());
        assert(!chest.result.wasItemInteraction);
        assert(decideBlockInteraction(ContainerType::Furnace, false, false).interaction ==
               BlockInteraction::OpenFurnace);
        assert(decideBlockInteraction(ContainerType::CraftingTable, false, false).interaction ==
               BlockInteraction::OpenCraftingTable);

        // Sneaking with something in hand turns every container back into a
        // surface to build on. Regression: the container branch used to consume
        // the click unconditionally, so a block could never be placed onto a
        // chest or a furnace.
        const auto sneaking = decideBlockInteraction(ContainerType::Chest, true, true);
        assert(sneaking.interaction == BlockInteraction::UseItem);
        assert(sneaking.result.kind == InteractionKind::TryEmptyHand);
        assert(!sneaking.result.consumesAction());
        // Sneaking empty-handed still opens: a crouched player must be able to
        // use a container at all.
        assert(decideBlockInteraction(ContainerType::Chest, true, false).interaction ==
               BlockInteraction::OpenChest);

        // A plain block always falls through to the item, whatever the stance.
        for (const bool sneak : {false, true}) {
            for (const bool holding : {false, true}) {
                assert(decideBlockInteraction(ContainerType::None, sneak, holding).interaction ==
                       BlockInteraction::UseItem);
            }
        }

        // The four outcomes carry what 26.1's records carry: only Success
        // consumes, and only the item-driven Success counts as an item
        // interaction.
        assert(gameplay::InteractionResult::success().wasItemInteraction);
        assert(gameplay::InteractionResult::consume().consumesAction());
        assert(gameplay::InteractionResult::consume().swing == gameplay::SwingSource::None);
        assert(!gameplay::InteractionResult::fail().consumesAction());
        assert(!gameplay::InteractionResult::pass().consumesAction());
    }

    return 0;
}

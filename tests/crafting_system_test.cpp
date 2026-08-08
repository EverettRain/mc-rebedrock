#include "gameplay/CraftingSystem.hpp"
#include "gameplay/Inventory.hpp"

#include <cassert>
#include <cstddef>

// Locks in the shift-click craft-result behaviour the player chose for 1.16.1:
// one click = one craft, and the result goes to the player inventory's main
// grid (slots 9..35) before the hotbar, stacking into existing stacks before
// empty slots. (Vanilla's ScreenHandler#QUICK_MOVE actually crafts repeatedly
// and prefers the hotbar; ReBedrock deliberately stays with single craft and
// main-grid-first, matching how the player plays.)
int main() {
    using namespace mc;
    using namespace mc::gameplay;

    // The torch recipe is 1x2: coal above a stick, yielding 4 torches. Its 2x2
    // window is slot0 (top-left) + slot2 (bottom-left).
    const auto torchStack = [](std::uint8_t count) {
        return ItemStack{world::Block::Torch, count, blockItemFor(world::Block::Torch)};
    };

    // Scenario 1: a single shift-click crafts exactly once, not the whole grid.
    {
        Inventory inventory;
        CraftingSystem crafting;
        // One matching window whose ingredients carry enough for a second craft,
        // so a batch implementation would leave the grid empty after one click.
        crafting.playerGridSlot(0) = {world::Block::Air, 2U, &items::Coal};
        crafting.playerGridSlot(2) = {world::Block::Air, 2U, &items::Stick};
        assert(!crafting.playerOutput().empty());

        assert(crafting.craftPlayer(inventory, true));

        // One craft produced 4 torches into the main grid (slot 9), not the
        // hotbar (slots 0..8 stay empty).
        assert(inventory.slot(9U) == torchStack(4U));
        assert(inventory.slot(0U).empty());

        // Only one coal + one stick were spent; the second set is still in the
        // grid, proving the click did not batch-craft both sets.
        assert(crafting.playerSlot(0).item == &items::Coal);
        assert(crafting.playerSlot(0).count == 1U);
        assert(crafting.playerSlot(2).item == &items::Stick);
        assert(crafting.playerSlot(2).count == 1U);

        // The next shift-click spends the final set.
        assert(crafting.craftPlayer(inventory, true));
        assert(inventory.slot(9U) == torchStack(8U));
        assert(crafting.playerOutput().empty());
    }

    // Scenario 2: the result merges into an existing stack in the main grid
    // before a fresh empty slot, and never reaches the hotbar.
    {
        Inventory inventory;
        CraftingSystem crafting;
        inventory.mutableSlot(15U) = torchStack(5U);
        crafting.playerGridSlot(0) = {world::Block::Air, 1U, &items::Coal};
        crafting.playerGridSlot(2) = {world::Block::Air, 1U, &items::Stick};

        assert(crafting.craftPlayer(inventory, true));

        // 5 + 4 merged into the existing slot 15, not a new slot 9.
        assert(inventory.slot(15U) == torchStack(9U));
        assert(inventory.slot(9U).empty());
        assert(inventory.slot(0U).empty());
    }

    // Scenario 3: the decorative stone recipes — a 2x2 block of diorite crafts
    // four polished diorite into the main grid.
    {
        Inventory inventory;
        CraftingSystem crafting;
        const auto diorite = [](std::uint8_t count) {
            return ItemStack{world::Block::Diorite, count, blockItemFor(world::Block::Diorite)};
        };
        for (std::size_t index = 0; index < 4U; ++index) {
            crafting.playerGridSlot(index) = diorite(1U);
        }
        const ItemStack polishedDiorite{
            world::Block::PolishedDiorite, 4U, blockItemFor(world::Block::PolishedDiorite)};
        assert(!crafting.playerOutput().empty());
        assert(crafting.craftPlayer(inventory, true));
        assert(inventory.slot(9U) == polishedDiorite);
        assert(inventory.slot(0U).empty());
        assert(crafting.playerOutput().empty());
    }
}

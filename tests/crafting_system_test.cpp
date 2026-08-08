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

    // Scenario 1: one shift-click crafts the whole batch — the single matching
    // window holds two torch sets (2 coal above 2 stick), so the click produces
    // eight torches, matching vanilla's QUICK_MOVE loop that keeps crafting
    // while the result can find a home.
    {
        Inventory inventory;
        CraftingSystem crafting;
        crafting.playerGridSlot(0) = {world::Block::Air, 2U, &items::Coal};
        crafting.playerGridSlot(2) = {world::Block::Air, 2U, &items::Stick};
        assert(!crafting.playerOutput().empty());

        assert(crafting.craftPlayer(inventory, true));

        // Both sets were consumed and all eight torches reached the main grid
        // (slot 9); the hotbar stays empty.
        assert(inventory.slot(9U) == torchStack(8U));
        assert(inventory.slot(0U).empty());
        assert(crafting.playerOutput().empty());
    }

    // Scenario 1b: the batch stops when the inventory can no longer take the
    // result, leaving that batch's ingredients unconsumed. Slot 35 holds 60
    // torches, so the first result fills it to 64 and the second has no home.
    {
        Inventory inventory;
        CraftingSystem crafting;
        for (std::size_t index = 0U; index <= 34U; ++index) {
            inventory.mutableSlot(index) = {world::Block::Air, 1U, &items::Wheat};
        }
        inventory.mutableSlot(35U) = torchStack(60U);
        crafting.playerGridSlot(0) = {world::Block::Air, 2U, &items::Coal};
        crafting.playerGridSlot(2) = {world::Block::Air, 2U, &items::Stick};
        assert(crafting.craftPlayer(inventory, true));
        // The first batch filled the pre-existing stack to its 64 cap.
        assert(inventory.slot(35U) == torchStack(64U));
        // Only one set was spent; the second stays in the grid.
        assert(crafting.playerSlot(0).item == &items::Coal);
        assert(crafting.playerSlot(0).count == 1U);
        assert(crafting.playerSlot(2).item == &items::Stick);
        assert(crafting.playerSlot(2).count == 1U);
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

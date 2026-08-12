#pragma once

#include "gameplay/Inventory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace mc::gameplay {

enum class IngredientKind : std::uint8_t {
    Empty,
    Block,
    Item,
    AnyPlanks,
};

struct RecipeIngredient final {
    IngredientKind kind = IngredientKind::Empty;
    world::Block block = world::Block::Air;
    const Item* item = nullptr;
};

struct CraftingRecipe final {
    std::string_view identifier;
    std::uint8_t width = 0U;
    std::uint8_t height = 0U;
    bool shapeless = false;
    bool allowMirror = false;
    std::vector<RecipeIngredient> ingredients;
    ItemStack output;
};

struct FurnaceRecipe final {
    std::string_view identifier;
    RecipeIngredient input;
    ItemStack output;
    int cookTicks = 200;
    float experience = 0.0F;
};

[[nodiscard]] std::span<const CraftingRecipe> craftingRecipes();
[[nodiscard]] std::span<const FurnaceRecipe> furnaceRecipes();
[[nodiscard]] int fuelBurnTicks(const ItemStack& stack);
// The furnace recipe an input smelts into, or nullptr. Shared by FurnaceSystem
// (the block entities that smelt) so recipe matching lives with the recipe data
// rather than being duplicated per consumer.
[[nodiscard]] const FurnaceRecipe* matchedFurnaceRecipe(const ItemStack& input);

class CraftingSystem final {
  public:
    [[nodiscard]] const ItemStack& playerSlot(std::size_t index) const;
    [[nodiscard]] const ItemStack& tableSlot(std::size_t index) const;
    // Mutable slot references for the drag path (SlotActionType.QUICK_CRAFT),
    // which needs the exact storage of each slot the cursor swept over.
    [[nodiscard]] ItemStack& playerGridSlot(std::size_t index);
    [[nodiscard]] ItemStack& tableGridSlot(std::size_t index);

    void clickPlayerSlot(Inventory& inventory, std::size_t index, InventoryMouseButton button,
                         bool shiftHeld = false);
    void clickTableSlot(Inventory& inventory, std::size_t index, InventoryMouseButton button,
                        bool shiftHeld = false);
    // QUICK_MOVE's inventory direction: move as much of `stack` into the grid as
    // the container accepts, leaving the remainder behind.
    bool movePlayerInto(ItemStack& stack);
    bool moveTableInto(ItemStack& stack);

    [[nodiscard]] ItemStack playerOutput() const;
    [[nodiscard]] ItemStack tableOutput() const;
    // Crafting the output: plain click takes it onto the cursor; Shift-click is
    // QUICK_MOVE, which sends the whole result into the player inventory (main
    // grid before the hotbar), matching vanilla's quickMove on the result slot.
    bool craftPlayer(Inventory& inventory, bool shiftHeld = false);
    bool craftTable(Inventory& inventory, bool shiftHeld = false);
    void stowAll(Inventory& inventory);

  private:
    std::array<ItemStack, 4> playerGrid_{};
    std::array<ItemStack, 9> tableGrid_{};

    template <std::size_t Size>
    [[nodiscard]] static ItemStack recipeOutput(const std::array<ItemStack, Size>& grid);
    template <std::size_t Size>
    static void consumeRecipe(std::array<ItemStack, Size>& grid);
    // Shared by craftPlayer/craftTable: plain clicks take the result onto the
    // cursor, Shift-clicks QUICK_MOVE it into the player inventory.
    template <std::size_t Size>
    bool craftInto(Inventory& inventory, bool shiftHeld, std::array<ItemStack, Size>& grid);
};

} // namespace mc::gameplay

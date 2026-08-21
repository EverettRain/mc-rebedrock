#include "gameplay/CraftingSystem.hpp"

#include "gameplay/RecipeTable.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <stdexcept>

namespace mc::gameplay {
namespace {

[[nodiscard]] bool blockIs(const ItemStack& stack, world::Block block) {
    // Matches a block ingredient whatever form the stack's item pointer takes
    // (the block's BlockItem or the legacy null sentinel).
    return isBlockStack(stack) && stack.block == block && !stack.empty();
}

[[nodiscard]] bool itemIs(const ItemStack& stack, const Item* item) {
    return stack.item == item && !stack.empty();
}

[[nodiscard]] bool anyPlanks(const ItemStack& stack) {
    using enum world::Block;
    return blockIs(stack, OakPlanks) || blockIs(stack, SprucePlanks) ||
        blockIs(stack, BirchPlanks) || blockIs(stack, JunglePlanks) ||
        blockIs(stack, AcaciaPlanks) || blockIs(stack, DarkOakPlanks);
}

[[nodiscard]] bool ingredientMatches(
    const RecipeIngredient& ingredient,
    const ItemStack& stack) {
    switch (ingredient.kind) {
    case IngredientKind::Empty:
        return stack.empty();
    case IngredientKind::Block:
        return blockIs(stack, ingredient.block);
    case IngredientKind::Item:
        return itemIs(stack, ingredient.item);
    case IngredientKind::AnyPlanks:
        return anyPlanks(stack);
    }
    return false;
}


template <std::size_t Size>
[[nodiscard]] bool shapedRecipeMatches(
    const CraftingRecipe& recipe,
    const std::array<ItemStack, Size>& grid,
    std::size_t gridWidth,
    bool mirrored) {
    const std::size_t gridHeight = Size / gridWidth;
    if (recipe.width > gridWidth || recipe.height > gridHeight) return false;
    for (std::size_t offsetY = 0; offsetY + recipe.height <= gridHeight; ++offsetY) {
        for (std::size_t offsetX = 0; offsetX + recipe.width <= gridWidth; ++offsetX) {
            bool matches = true;
            for (std::size_t y = 0; y < gridHeight && matches; ++y) {
                for (std::size_t x = 0; x < gridWidth; ++x) {
                    RecipeIngredient expected{};
                    if (x >= offsetX && x < offsetX + recipe.width &&
                        y >= offsetY && y < offsetY + recipe.height) {
                        const std::size_t recipeX = mirrored
                            ? recipe.width - 1U - (x - offsetX)
                            : x - offsetX;
                        expected = recipe.ingredients[
                            (y - offsetY) * recipe.width + recipeX];
                    }
                    if (!ingredientMatches(expected, grid[y * gridWidth + x])) {
                        matches = false;
                        break;
                    }
                }
            }
            if (matches) return true;
        }
    }
    return false;
}

template <std::size_t Size>
[[nodiscard]] bool shapelessRecipeMatches(
    const CraftingRecipe& recipe,
    const std::array<ItemStack, Size>& grid) {
    const auto occupied = std::ranges::count_if(
        grid, [](const ItemStack& stack) { return !stack.empty(); });
    if (static_cast<std::size_t>(occupied) != recipe.ingredients.size()) return false;
    std::vector<bool> used(recipe.ingredients.size(), false);
    for (const auto& stack : grid) {
        if (stack.empty()) continue;
        bool found = false;
        for (std::size_t index = 0; index < recipe.ingredients.size(); ++index) {
            if (!used[index] && ingredientMatches(recipe.ingredients[index], stack)) {
                used[index] = true;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

template <std::size_t Size>
[[nodiscard]] const CraftingRecipe* matchedCraftingRecipe(
    const std::array<ItemStack, Size>& grid) {
    const std::size_t gridWidth = Size == 4U ? 2U : 3U;
    for (const auto& recipe : recipeTable().crafting()) {
        if (recipe.width > gridWidth || recipe.height > gridWidth) continue;
        if (recipe.shapeless) {
            if (shapelessRecipeMatches(recipe, grid)) return &recipe;
        } else if (shapedRecipeMatches(recipe, grid, gridWidth, false) ||
                   (recipe.allowMirror &&
                    shapedRecipeMatches(recipe, grid, gridWidth, true))) {
            return &recipe;
        }
    }
    return nullptr;
}

} // namespace

const FurnaceRecipe* matchedFurnaceRecipe(const ItemStack& input) {
    const auto recipes = recipeTable().furnace();
    const auto recipe = std::ranges::find_if(recipes, [&input](const FurnaceRecipe& candidate) {
        return ingredientMatches(candidate.input, input);
    });
    return recipe == recipes.end() ? nullptr : &*recipe;
}

std::span<const CraftingRecipe> craftingRecipes() {
    return recipeTable().crafting();
}

std::span<const FurnaceRecipe> furnaceRecipes() {
    return recipeTable().furnace();
}

int fuelBurnTicks(const ItemStack& stack) {
    if (itemIs(stack, &items::Coal)) return 1600;
    if (itemIs(stack, &items::Stick)) return 100;
    if (blockIs(stack, world::Block::OakLog) ||
        blockIs(stack, world::Block::SpruceLog) ||
        blockIs(stack, world::Block::BirchLog) || anyPlanks(stack)) {
        return 300;
    }
    return 0;
}

namespace {

template <std::size_t Size>
void consumeMatchedRecipe(std::array<ItemStack, Size>& grid) {
    for (auto& stack : grid) {
        if (stack.empty()) continue;
        --stack.count;
        if (stack.count == 0U) stack = {};
    }
}

} // namespace

const ItemStack& CraftingSystem::playerSlot(std::size_t index) const {
    if (index >= playerGrid_.size()) throw std::out_of_range("player crafting slot");
    return playerGrid_[index];
}

const ItemStack& CraftingSystem::tableSlot(std::size_t index) const {
    if (index >= tableGrid_.size()) throw std::out_of_range("table crafting slot");
    return tableGrid_[index];
}

ItemStack& CraftingSystem::playerGridSlot(std::size_t index) {
    if (index >= playerGrid_.size()) throw std::out_of_range("player crafting slot");
    return playerGrid_[index];
}

ItemStack& CraftingSystem::tableGridSlot(std::size_t index) {
    if (index >= tableGrid_.size()) throw std::out_of_range("table crafting slot");
    return tableGrid_[index];
}

void CraftingSystem::clickPlayerSlot(
    Inventory& inventory,
    std::size_t index,
    InventoryMouseButton button,
    bool shiftHeld) {
    if (index >= playerGrid_.size()) return;
    if (shiftHeld) {
        inventory.quickMoveInto(playerGrid_[index]);
        return;
    }
    inventory.clickExternalSlot(playerGrid_[index], button);
}

void CraftingSystem::clickTableSlot(
    Inventory& inventory,
    std::size_t index,
    InventoryMouseButton button,
    bool shiftHeld) {
    if (index >= tableGrid_.size()) return;
    if (shiftHeld) {
        inventory.quickMoveInto(tableGrid_[index]);
        return;
    }
    inventory.clickExternalSlot(tableGrid_[index], button);
}

namespace {

template <std::size_t Size>
[[nodiscard]] bool moveIntoGrid(std::array<ItemStack, Size>& grid, ItemStack& stack) {
    if (stack.empty()) return true;
    const auto maximum = itemMaximumStackSize(stack);
    for (auto& target : grid) {
        if (!sameItem(target, stack) || target.count >= maximum) continue;
        const auto moved =
            std::min(stack.count, static_cast<std::uint8_t>(maximum - target.count));
        target.count = static_cast<std::uint8_t>(target.count + moved);
        stack.count = static_cast<std::uint8_t>(stack.count - moved);
        if (stack.count == 0U) {
            stack = {};
            return true;
        }
    }
    for (auto& target : grid) {
        if (target.empty()) {
            target = stack;
            stack = {};
            return true;
        }
    }
    return false;
}

} // namespace

bool CraftingSystem::movePlayerInto(ItemStack& stack) {
    return moveIntoGrid(playerGrid_, stack);
}

bool CraftingSystem::moveTableInto(ItemStack& stack) {
    return moveIntoGrid(tableGrid_, stack);
}

template <std::size_t Size>
ItemStack CraftingSystem::recipeOutput(const std::array<ItemStack, Size>& grid) {
    const auto* recipe = matchedCraftingRecipe(grid);
    return recipe != nullptr ? recipe->output : ItemStack{};
}

template <std::size_t Size>
void CraftingSystem::consumeRecipe(std::array<ItemStack, Size>& grid) {
    consumeMatchedRecipe(grid);
}

ItemStack CraftingSystem::playerOutput() const { return recipeOutput(playerGrid_); }
ItemStack CraftingSystem::tableOutput() const { return recipeOutput(tableGrid_); }

template <std::size_t Size>
bool CraftingSystem::craftInto(Inventory& inventory, bool shiftHeld,
                               std::array<ItemStack, Size>& grid) {
    const ItemStack result = recipeOutput(grid);
    if (result.empty()) {
        return false;
    }
    if (!shiftHeld) {
        if (!inventory.mergeIntoCursor(result)) {
            return false;
        }
        consumeRecipe(grid);
        return true;
    }
    // Shift-click is QUICK_MOVE on the result slot. ScreenHandler#method_30010
    // loops `transferSlot` while the result keeps producing the same item, so
    // one shift-click crafts as many as the ingredients and the player
    // inventory allow. The loop stops the moment a result cannot find a home,
    // leaving that batch's ingredients unconsumed — a full inventory never
    // wastes them.
    while (!recipeOutput(grid).empty()) {
        ItemStack remainder = recipeOutput(grid);
        inventory.quickMoveInto(remainder);
        if (!remainder.empty()) {
            break;
        }
        consumeRecipe(grid);
    }
    return true;
}

bool CraftingSystem::craftPlayer(Inventory& inventory, bool shiftHeld) {
    return craftInto(inventory, shiftHeld, playerGrid_);
}

bool CraftingSystem::craftTable(Inventory& inventory, bool shiftHeld) {
    return craftInto(inventory, shiftHeld, tableGrid_);
}

void CraftingSystem::stowAll(Inventory& inventory) {
    for (auto& stack : playerGrid_) inventory.add(stack);
    for (auto& stack : tableGrid_) inventory.add(stack);
}

} // namespace mc::gameplay

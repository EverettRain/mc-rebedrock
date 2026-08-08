#include "gameplay/CraftingSystem.hpp"

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

[[nodiscard]] constexpr RecipeIngredient block(world::Block value) {
    return {IngredientKind::Block, value, nullptr};
}

[[nodiscard]] constexpr RecipeIngredient item(const Item& value) {
    return {IngredientKind::Item, world::Block::Air, &value};
}

[[nodiscard]] constexpr RecipeIngredient planks() {
    return {IngredientKind::AnyPlanks, world::Block::Air, nullptr};
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

[[nodiscard]] const std::vector<CraftingRecipe>& craftingRecipeStorage() {
    static const std::vector<CraftingRecipe> recipes{
        {"minecraft:oak_planks", 1, 1, false, false,
         {block(world::Block::OakLog)}, {world::Block::OakPlanks, 4U, blockItemFor(world::Block::OakPlanks)}},
        {"minecraft:spruce_planks", 1, 1, false, false,
         {block(world::Block::SpruceLog)}, {world::Block::SprucePlanks, 4U, blockItemFor(world::Block::SprucePlanks)}},
        {"minecraft:birch_planks", 1, 1, false, false,
         {block(world::Block::BirchLog)}, {world::Block::BirchPlanks, 4U, blockItemFor(world::Block::BirchPlanks)}},
        {"minecraft:sticks", 1, 2, false, false,
         {planks(), planks()}, {world::Block::Air, 4U, &items::Stick}},
        // Three wheat in a row is bread, the 1.16.1 bread.json pattern "###".
        {"minecraft:bread", 3, 1, false, false,
         {item(items::Wheat), item(items::Wheat), item(items::Wheat)},
         {world::Block::Air, 1U, &items::Bread}},
        {"minecraft:crafting_table", 2, 2, false, false,
         {planks(), planks(), planks(), planks()}, {world::Block::CraftingTable, 1U, blockItemFor(world::Block::CraftingTable)}},
        {"minecraft:chest", 3, 3, false, false,
         {planks(), planks(), planks(), planks(), {}, planks(),
          planks(), planks(), planks()},
         {world::Block::Chest, 1U, blockItemFor(world::Block::Chest)}},
        {"minecraft:torch", 1, 2, false, false,
         {item(items::Coal), item(items::Stick)}, {world::Block::Torch, 4U, blockItemFor(world::Block::Torch)}},
        {"minecraft:sandstone", 2, 2, false, false,
         {block(world::Block::Sand), block(world::Block::Sand),
          block(world::Block::Sand), block(world::Block::Sand)},
         {world::Block::Sandstone, 1U, blockItemFor(world::Block::Sandstone)}},
        {"minecraft:coarse_dirt", 2, 2, true, false,
         {block(world::Block::Dirt), block(world::Block::Dirt),
          block(world::Block::Gravel), block(world::Block::Gravel)},
         {world::Block::CoarseDirt, 4U, blockItemFor(world::Block::CoarseDirt)}},
        {"minecraft:stone_bricks", 2, 2, false, false,
         {block(world::Block::Stone), block(world::Block::Stone),
          block(world::Block::Stone), block(world::Block::Stone)},
         {world::Block::StoneBricks, 4U, blockItemFor(world::Block::StoneBricks)}},
        {"minecraft:furnace", 3, 3, false, false,
         {block(world::Block::Cobblestone), block(world::Block::Cobblestone),
          block(world::Block::Cobblestone), block(world::Block::Cobblestone), {},
          block(world::Block::Cobblestone), block(world::Block::Cobblestone),
          block(world::Block::Cobblestone), block(world::Block::Cobblestone)},
         {world::Block::Furnace, 1U, blockItemFor(world::Block::Furnace)}},
        {"minecraft:wooden_pickaxe", 3, 3, false, false,
         {planks(), planks(), planks(), {}, item(items::Stick), {}, {},
          item(items::Stick), {}},
         {world::Block::Air, 1U, &items::WoodenPickaxe}},
        {"minecraft:stone_pickaxe", 3, 3, false, false,
         {block(world::Block::Cobblestone), block(world::Block::Cobblestone),
          block(world::Block::Cobblestone), {}, item(items::Stick), {}, {},
          item(items::Stick), {}},
         {world::Block::Air, 1U, &items::StonePickaxe}},
        {"minecraft:iron_pickaxe", 3, 3, false, false,
         {item(items::IronIngot), item(items::IronIngot),
          item(items::IronIngot), {}, item(items::Stick), {}, {},
          item(items::Stick), {}},
         {world::Block::Air, 1U, &items::IronPickaxe}},
        {"minecraft:diamond_pickaxe", 3, 3, false, false,
         {item(items::Diamond), item(items::Diamond), item(items::Diamond),
          {}, item(items::Stick), {}, {}, item(items::Stick), {}},
         {world::Block::Air, 1U, &items::DiamondPickaxe}},
        {"minecraft:golden_pickaxe", 3, 3, false, false,
         {item(items::GoldIngot), item(items::GoldIngot),
          item(items::GoldIngot), {}, item(items::Stick), {}, {},
          item(items::Stick), {}},
         {world::Block::Air, 1U, &items::GoldPickaxe}},
        {"minecraft:jungle_planks", 1, 1, false, false,
         {block(world::Block::JungleLog)}, {world::Block::JunglePlanks, 4U, blockItemFor(world::Block::JunglePlanks)}},
        {"minecraft:acacia_planks", 1, 1, false, false,
         {block(world::Block::AcaciaLog)}, {world::Block::AcaciaPlanks, 4U, blockItemFor(world::Block::AcaciaPlanks)}},
        {"minecraft:dark_oak_planks", 1, 1, false, false,
         {block(world::Block::DarkOakLog)}, {world::Block::DarkOakPlanks, 4U, blockItemFor(world::Block::DarkOakPlanks)}},
        {"minecraft:wooden_axe", 2, 3, false, true,
         {planks(), planks(), planks(), item(items::Stick), {},
          item(items::Stick)},
         {world::Block::Air, 1U, &items::WoodenAxe}},
        {"minecraft:stone_axe", 2, 3, false, true,
         {block(world::Block::Cobblestone), block(world::Block::Cobblestone),
          block(world::Block::Cobblestone), item(items::Stick), {},
          item(items::Stick)},
         {world::Block::Air, 1U, &items::StoneAxe}},
        {"minecraft:iron_axe", 2, 3, false, true,
         {item(items::IronIngot), item(items::IronIngot),
          item(items::IronIngot), item(items::Stick), {},
          item(items::Stick)},
         {world::Block::Air, 1U, &items::IronAxe}},
        {"minecraft:golden_axe", 2, 3, false, true,
         {item(items::GoldIngot), item(items::GoldIngot),
          item(items::GoldIngot), item(items::Stick), {},
          item(items::Stick)},
         {world::Block::Air, 1U, &items::GoldAxe}},
        {"minecraft:diamond_axe", 2, 3, false, true,
         {item(items::Diamond), item(items::Diamond),
          item(items::Diamond), item(items::Stick), {},
          item(items::Stick)},
         {world::Block::Air, 1U, &items::DiamondAxe}},
        {"minecraft:wooden_shovel", 1, 3, false, false,
         {planks(), item(items::Stick), item(items::Stick)},
         {world::Block::Air, 1U, &items::WoodenShovel}},
        {"minecraft:stone_shovel", 1, 3, false, false,
         {block(world::Block::Cobblestone), item(items::Stick),
          item(items::Stick)},
         {world::Block::Air, 1U, &items::StoneShovel}},
        {"minecraft:iron_shovel", 1, 3, false, false,
         {item(items::IronIngot), item(items::Stick), item(items::Stick)},
         {world::Block::Air, 1U, &items::IronShovel}},
        {"minecraft:golden_shovel", 1, 3, false, false,
         {item(items::GoldIngot), item(items::Stick), item(items::Stick)},
         {world::Block::Air, 1U, &items::GoldShovel}},
        {"minecraft:diamond_shovel", 1, 3, false, false,
         {item(items::Diamond), item(items::Stick), item(items::Stick)},
         {world::Block::Air, 1U, &items::DiamondShovel}},
        {"minecraft:wooden_hoe", 2, 2, false, false,
         {planks(), planks(), item(items::Stick), item(items::Stick)},
         {world::Block::Air, 1U, &items::WoodenHoe}},
        {"minecraft:stone_hoe", 2, 2, false, false,
         {block(world::Block::Cobblestone), block(world::Block::Cobblestone),
          item(items::Stick), item(items::Stick)},
         {world::Block::Air, 1U, &items::StoneHoe}},
        {"minecraft:iron_hoe", 2, 2, false, false,
         {item(items::IronIngot), item(items::IronIngot),
          item(items::Stick), item(items::Stick)},
         {world::Block::Air, 1U, &items::IronHoe}},
        {"minecraft:golden_hoe", 2, 2, false, false,
         {item(items::GoldIngot), item(items::GoldIngot),
          item(items::Stick), item(items::Stick)},
         {world::Block::Air, 1U, &items::GoldHoe}},
        {"minecraft:diamond_hoe", 2, 2, false, false,
         {item(items::Diamond), item(items::Diamond),
          item(items::Stick), item(items::Stick)},
         {world::Block::Air, 1U, &items::DiamondHoe}},
        {"minecraft:wooden_sword", 1, 3, false, false,
         {planks(), planks(), item(items::Stick)},
         {world::Block::Air, 1U, &items::WoodenSword}},
        {"minecraft:stone_sword", 1, 3, false, false,
         {block(world::Block::Cobblestone), block(world::Block::Cobblestone),
          item(items::Stick)},
         {world::Block::Air, 1U, &items::StoneSword}},
        {"minecraft:iron_sword", 1, 3, false, false,
         {item(items::IronIngot), item(items::IronIngot),
          item(items::Stick)},
         {world::Block::Air, 1U, &items::IronSword}},
        {"minecraft:golden_sword", 1, 3, false, false,
         {item(items::GoldIngot), item(items::GoldIngot),
          item(items::Stick)},
         {world::Block::Air, 1U, &items::GoldSword}},
        {"minecraft:diamond_sword", 1, 3, false, false,
         {item(items::Diamond), item(items::Diamond),
          item(items::Stick)},
         {world::Block::Air, 1U, &items::DiamondSword}},
    };
    return recipes;
}

[[nodiscard]] const std::vector<FurnaceRecipe>& furnaceRecipeStorage() {
    static const std::vector<FurnaceRecipe> recipes{
        {"minecraft:iron_ingot_from_smelting", block(world::Block::IronOre),
         {world::Block::Air, 1U, &items::IronIngot}, 200, 0.7F},
        {"minecraft:gold_ingot_from_smelting", block(world::Block::GoldOre),
         {world::Block::Air, 1U, &items::GoldIngot}, 200, 1.0F},
        {"minecraft:stone_from_smelting", block(world::Block::Cobblestone),
         {world::Block::Stone, 1U, blockItemFor(world::Block::Stone)}, 200, 0.1F},
        {"minecraft:glass_from_sand", block(world::Block::Sand),
         {world::Block::Glass, 1U, blockItemFor(world::Block::Glass)}, 200, 0.1F},
        {"minecraft:glass_from_red_sand", block(world::Block::RedSand),
         {world::Block::Glass, 1U, blockItemFor(world::Block::Glass)}, 200, 0.1F},
        {"minecraft:cooked_porkchop", item(items::Porkchop),
         {world::Block::Air, 1U, &items::CookedPorkchop}, 200, 0.35F},
    };
    return recipes;
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
    for (const auto& recipe : craftingRecipeStorage()) {
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

[[nodiscard]] const FurnaceRecipe* matchedFurnaceRecipe(const ItemStack& input) {
    const auto& recipes = furnaceRecipeStorage();
    const auto recipe = std::ranges::find_if(recipes, [&input](const FurnaceRecipe& candidate) {
        return ingredientMatches(candidate.input, input);
    });
    return recipe == recipes.end() ? nullptr : &*recipe;
}

} // namespace

std::span<const CraftingRecipe> craftingRecipes() {
    return craftingRecipeStorage();
}

std::span<const FurnaceRecipe> furnaceRecipes() {
    return furnaceRecipeStorage();
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

void CraftingSystem::clickFurnaceInput(
    Inventory& inventory, InventoryMouseButton button, bool shiftHeld) {
    if (shiftHeld) {
        inventory.quickMoveInto(furnaceInput_);
        return;
    }
    inventory.clickExternalSlot(furnaceInput_, button);
}
void CraftingSystem::clickFurnaceFuel(
    Inventory& inventory, InventoryMouseButton button, bool shiftHeld) {
    if (shiftHeld) {
        inventory.quickMoveInto(furnaceFuel_);
        return;
    }
    inventory.clickExternalSlot(furnaceFuel_, button);
}
void CraftingSystem::clickFurnaceOutput(Inventory& inventory, bool shiftHeld) {
    if (furnaceOutput_.empty()) {
        return;
    }
    // Plain click takes the smelted result onto the cursor; Shift-click is
    // QUICK_MOVE and routes it into the player inventory (main grid first, then
    // the hotbar), exactly like vanilla's furnace result slot.
    if (shiftHeld) {
        inventory.quickMoveInto(furnaceOutput_);
        return;
    }
    if (inventory.mergeIntoCursor(furnaceOutput_)) {
        furnaceOutput_ = {};
    }
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

bool CraftingSystem::moveFurnaceInto(ItemStack& stack) {
    if (stack.empty()) return true;
    // QUICK_MOVE routes a smeltable item to the input slot and a burnable one
    // to the fuel slot, merging into a partial stack just like a click would.
    ItemStack* target = nullptr;
    if (matchedFurnaceRecipe(stack) != nullptr) {
        target = &furnaceInput_;
    } else if (fuelBurnTicks(stack) > 0) {
        target = &furnaceFuel_;
    }
    if (target == nullptr) return false;
    if (target->empty()) {
        *target = stack;
        stack = {};
        return true;
    }
    if (sameItem(*target, stack) && target->count < itemMaximumStackSize(stack)) {
        const auto moved = std::min(
            stack.count, static_cast<std::uint8_t>(itemMaximumStackSize(stack) - target->count));
        target->count = static_cast<std::uint8_t>(target->count + moved);
        stack.count = static_cast<std::uint8_t>(stack.count - moved);
        if (stack.count == 0U) {
            stack = {};
            return true;
        }
    }
    return false;
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
    // Shift-click on the result: QUICK_MOVE the whole output into the player
    // inventory (main grid then hotbar). If the inventory is full, fall back to
    // the cursor so the crafted item is never silently lost; only when the
    // result finds a home is the recipe consumed, so a full inventory cannot
    // waste the ingredients.
    ItemStack remainder = result;
    inventory.quickMoveInto(remainder);
    if (!remainder.empty() && !inventory.mergeIntoCursor(remainder)) {
        return false;
    }
    consumeRecipe(grid);
    return true;
}

bool CraftingSystem::craftPlayer(Inventory& inventory, bool shiftHeld) {
    return craftInto(inventory, shiftHeld, playerGrid_);
}

bool CraftingSystem::craftTable(Inventory& inventory, bool shiftHeld) {
    return craftInto(inventory, shiftHeld, tableGrid_);
}

float CraftingSystem::furnaceProgress() const {
    return cookDurationTicks_ > 0
        ? std::clamp(
              static_cast<float>(cookTicks_) /
                  static_cast<float>(cookDurationTicks_),
              0.0F,
              1.0F)
        : 0.0F;
}
float CraftingSystem::furnaceFuelProgress() const {
    return initialBurnTicks_ > 0
        ? static_cast<float>(burnTicks_) / static_cast<float>(initialBurnTicks_)
        : 0.0F;
}

void CraftingSystem::tickFurnace() {
    const auto* recipe = matchedFurnaceRecipe(furnaceInput_);
    if (recipe == nullptr || recipe->identifier != activeFurnaceRecipe_) {
        cookTicks_ = 0;
        activeFurnaceRecipe_ = recipe != nullptr ? recipe->identifier : std::string_view{};
        cookDurationTicks_ = recipe != nullptr ? recipe->cookTicks : 200;
    }
    const ItemStack result = recipe != nullptr ? recipe->output : ItemStack{};
    const bool outputAccepts = recipe != nullptr &&
        (furnaceOutput_.empty() ||
         (sameItem(furnaceOutput_, result) && furnaceOutput_.count <
              itemMaximumStackSize(furnaceOutput_)));
    const int availableFuelTicks = fuelBurnTicks(furnaceFuel_);
    if (burnTicks_ <= 0 && outputAccepts && availableFuelTicks > 0) {
        --furnaceFuel_.count;
        if (furnaceFuel_.count == 0U) furnaceFuel_ = {};
        burnTicks_ = availableFuelTicks;
        initialBurnTicks_ = availableFuelTicks;
    }
    const bool burning = burnTicks_ > 0;
    if (burning && outputAccepts) {
        ++cookTicks_;
        if (cookTicks_ >= cookDurationTicks_) {
            cookTicks_ = 0;
            --furnaceInput_.count;
            if (furnaceInput_.count == 0U) furnaceInput_ = {};
            if (furnaceOutput_.empty()) furnaceOutput_ = result;
            else ++furnaceOutput_.count;
        }
    } else {
        cookTicks_ = 0;
    }
    if (burnTicks_ > 0) --burnTicks_;
}

void CraftingSystem::stowAll(Inventory& inventory) {
    for (auto& stack : playerGrid_) inventory.add(stack);
    for (auto& stack : tableGrid_) inventory.add(stack);
}

} // namespace mc::gameplay

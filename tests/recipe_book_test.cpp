// 配方书的后端：「够不够做」的匹配器、一键填充、已解锁/待高亮两个集合与它们的
// 持久化，以及配方的分类。
//
// ★ 这里的每个数都是对着 26.1 的源码/数据手算出来的，**不是**把本作跑出来的值
//   抄回来的。三处最关键的：
//
//   1. `biggestCraftableStack` 不是「每种材料的数量除以需要量再取最小值」。
//      工作台配方要 4 格「任意木板」，背包里 7 块橡木板 + 1 块白桦木板 = 8 块：
//      整除会说做得出 8/4 = 2 个。真正的答案是 **1**——
//      `StackedContents.RecipePicker.tryPick(2)`（StackedContents.java:141-191）
//      要给 4 个材料格各配一种**当时还剩 ≥2 个**的木板：橡木 7→5→3→1 只喂饱三格，
//      第四格橡木只剩 1、白桦只有 1，都不够 2，于是 tryPick(2) 失败。
//      而 `getResultUpperBound`（:72-100）会给出 7（单看一格，橡木最多），
//      这正说明它是**上界不是答案**。
//
//   2. `PlaceRecipeHelper.placeRecipe`（PlaceRecipeHelper.java:19-57）会把比网格
//      小的配方**居中**。1 宽 2 高的火把配方摆进 3x3：
//      竖直方向 `2 < 3/2.0` 为假不居中，水平方向 `1 < 3/2.0` 为真、
//      `floor(3/2.0 - 1/2.0) = 1`，所以落在 1 号与 4 号格（中列的上两格），
//      不是 0 号与 3 号。
//
//   3. 「网格里已经摆着这条配方」时普通点击是**加一层**：
//      `calculateAmountToCraft`（ServerPlaceRecipe.java:145-168）取网格里最小的
//      那一摞 + 1；而 :93-100 那道闸门在「再加一层就超过做得出来的次数」时
//      什么都不做。

#include "gameplay/ContentRegistry.hpp"
#include "gameplay/CraftingSystem.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/RecipeBook.hpp"
#include "gameplay/RecipeBookCategory.hpp"
#include "gameplay/RecipeTable.hpp"
#include "gameplay/ScreenTypes.hpp"
#include "gameplay/StackedItemContents.hpp"
#include "persistence/SaveRepository.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace mc::gameplay;
using mc::world::Block;

[[nodiscard]] const CraftingRecipe& craftingRecipe(std::string_view identifier) {
    const auto recipes = recipeTable().crafting();
    const auto found = std::ranges::find(recipes, identifier, &CraftingRecipe::identifier);
    assert(found != recipes.end());
    return *found;
}

[[nodiscard]] ItemStack blockStack(Block block, std::uint8_t count) {
    return ItemStack{block, count, blockItemFor(block)};
}

[[nodiscard]] ItemStack itemStack(const Item* item, std::uint8_t count) {
    return ItemStack{Block::Air, count, item};
}

// 一份背包 + 一张网格里所有物品的总件数。一键填充绝不许改变它。
[[nodiscard]] int totalItemCount(const Inventory& inventory, const CraftingSystem& crafting,
                                 std::size_t gridSize) {
    int total = 0;
    for (const auto& stack : inventory.slots()) {
        if (!stack.empty()) total += static_cast<int>(stack.count);
    }
    if (!inventory.cursorStack().empty()) {
        total += static_cast<int>(inventory.cursorStack().count);
    }
    for (std::size_t index = 0; index < gridSize; ++index) {
        const ItemStack& stack =
            gridSize == 4U ? crafting.playerSlot(index) : crafting.tableSlot(index);
        if (!stack.empty()) total += static_cast<int>(stack.count);
    }
    return total;
}

void putInInventory(Inventory& inventory, std::span<const ItemStack> stacks) {
    for (const auto& stack : stacks) {
        ItemStack copy = stack;
        assert(inventory.add(copy));
    }
}

// --- 1. 「够不够做 / 最多做几次」：二分图匹配，不是整除 -------------------

void testBiggestCraftableStackIsNotDivision() {
    const CraftingRecipe& table = craftingRecipe("minecraft:crafting_table");
    assert(table.ingredients.size() == 4U);

    StackedItemContents contents;
    contents.accountStack(blockStack(Block::OakPlanks, 7U));
    contents.accountStack(blockStack(Block::BirchPlanks, 1U));

    // 上界只看单格：每一格都收橡木，橡木最多 7 个 -> 上界 7。
    const auto placement = recipePlacement(table);
    assert(contents.resultUpperBound(placement.ingredients) == 7);

    // 真正的答案是 1（见文件头的手算）。
    assert(contents.biggestCraftableStack(table) == 1);
    assert(contents.canCraft(table, 1));
    assert(!contents.canCraft(table, 2));
}

void testMatchingLetsTwoSlotsShareOneItemType() {
    // 8 块橡木板刚好做 2 个工作台：容量 2 时橡木 8->6->4->2->0 喂饱四格。
    const CraftingRecipe& table = craftingRecipe("minecraft:crafting_table");
    StackedItemContents contents;
    contents.accountStack(blockStack(Block::OakPlanks, 8U));
    assert(contents.biggestCraftableStack(table) == 2);
    assert(contents.canCraft(table, 2));
    assert(!contents.canCraft(table, 3));
}

void testShapedRecipeCountsEveryFilledCell() {
    // 箱子是 3x3 减去正中一格 = 8 格木板。7 块不够，8 块正好做一个。
    const CraftingRecipe& chest = craftingRecipe("minecraft:chest");
    assert(chest.ingredients.size() == 9U);
    const auto placement = recipePlacement(chest);
    assert(placement.ingredients.size() == 8U);
    assert(placement.slotsToIngredientIndex.size() == 9U);
    assert(placement.slotsToIngredientIndex[4U] == -1);  // 正中那格是空的
    assert(placement.slotsToIngredientIndex[0U] == 0);
    assert(placement.slotsToIngredientIndex[5U] == 4);   // 空格之后编号顺延

    StackedItemContents seven;
    seven.accountStack(blockStack(Block::OakPlanks, 7U));
    assert(!seven.canCraft(chest, 1));

    StackedItemContents eight;
    eight.accountStack(blockStack(Block::OakPlanks, 8U));
    assert(eight.canCraft(chest, 1));
    assert(eight.biggestCraftableStack(chest) == 1);
}

void testTwoDistinctIngredientsTakeTheMinimum() {
    // 火把 = 1 煤 + 1 木棍。5 煤 + 2 木棍 -> 2 次。
    const CraftingRecipe& torch = craftingRecipe("minecraft:torch");
    StackedItemContents contents;
    contents.accountStack(itemStack(&items::Coal, 5U));
    contents.accountStack(itemStack(&items::Stick, 2U));
    assert(contents.biggestCraftableStack(torch) == 2);
}

void testUnusableStacksAreNotAccounted() {
    // `Inventory.isUsableForCrafting`（Inventory.java:144-146）：有损伤 / 有附魔 /
    // 有自定义名的堆不算原料。面包要 3 个小麦；6 个小麦能做 2 个，但其中 3 个被
    // 改过名之后只剩 3 个可用，就只能做 1 个。
    const CraftingRecipe& bread = craftingRecipe("minecraft:bread");

    StackedItemContents plain;
    plain.accountSimpleStack(itemStack(&items::Wheat, 3U));
    plain.accountSimpleStack(itemStack(&items::Wheat, 3U));
    assert(plain.biggestCraftableStack(bread) == 2);

    StackedItemContents named;
    named.accountSimpleStack(itemStack(&items::Wheat, 3U));
    ItemStack renamed = itemStack(&items::Wheat, 3U);
    renamed.customNameId = 7U;
    assert(!isUsableForCrafting(renamed));
    named.accountSimpleStack(renamed);
    assert(named.biggestCraftableStack(bread) == 1);
}

void testBlockStackKeysCollapse() {
    // 同一种方块的两种写法（空 item 哨兵 / 它自己的 BlockItem）必须折成同一个键，
    // 否则 4 块木板会被记成两种各 2 块，做不出工作台。
    const CraftingRecipe& table = craftingRecipe("minecraft:crafting_table");
    ItemStack legacy{Block::OakPlanks, 2U, nullptr};
    ItemStack modern = blockStack(Block::OakPlanks, 2U);
    assert(stackedContentsKey(legacy) == stackedContentsKey(modern));

    StackedItemContents contents;
    contents.accountStack(legacy);
    contents.accountStack(modern);
    assert(contents.amounts().size() == 1U);
    assert(contents.canCraft(table, 1));
}

void testInventoryConvenienceEntryPoints() {
    Inventory inventory;
    const std::array<ItemStack, 1U> planks{blockStack(Block::OakPlanks, 8U)};
    putInInventory(inventory, planks);
    const CraftingRecipe& table = craftingRecipe("minecraft:crafting_table");
    assert(canCraft(inventory, table, 2));
    assert(!canCraft(inventory, table, 3));
    assert(biggestCraftableStack(inventory, table) == 2);
}

// --- 2. 分类：钉 vanilla 自己的 recipe JSON --------------------------------

void testRecipeBookCategoriesMatchVanillaJson() {
    // 逐条对着 26.1 `data/minecraft/recipe/<name>.json` 的 `category` 字段。
    assert(craftingRecipeBookCategory("minecraft:oak_planks") ==
           RecipeBookCategory::CraftingBuildingBlocks);   // oak_planks.json: building
    assert(craftingRecipeBookCategory("minecraft:stone_bricks") ==
           RecipeBookCategory::CraftingBuildingBlocks);   // stone_bricks.json: building
    assert(craftingRecipeBookCategory("minecraft:tnt") ==
           RecipeBookCategory::CraftingRedstone);         // tnt.json: redstone
    assert(craftingRecipeBookCategory("minecraft:wooden_pickaxe") ==
           RecipeBookCategory::CraftingEquipment);        // wooden_pickaxe.json: equipment
    assert(craftingRecipeBookCategory("minecraft:bow") ==
           RecipeBookCategory::CraftingEquipment);        // bow.json: equipment
    assert(craftingRecipeBookCategory("minecraft:bread") ==
           RecipeBookCategory::CraftingMisc);             // bread.json: misc
    assert(craftingRecipeBookCategory("minecraft:crafting_table") ==
           RecipeBookCategory::CraftingMisc);             // crafting_table.json: misc
    // 木棍在本作叫 `sticks`，vanilla 的文件是 stick.json（category: misc）。
    assert(craftingRecipeBookCategory("minecraft:sticks") == RecipeBookCategory::CraftingMisc);

    assert(furnaceRecipeBookCategory("minecraft:cooked_porkchop") ==
           RecipeBookCategory::FurnaceFood);              // cooked_porkchop.json: food
    assert(furnaceRecipeBookCategory("minecraft:stone_from_smelting") ==
           RecipeBookCategory::FurnaceBlocks);            // stone.json: blocks
    assert(furnaceRecipeBookCategory("minecraft:smooth_stone") ==
           RecipeBookCategory::FurnaceBlocks);            // smooth_stone.json: blocks
    assert(furnaceRecipeBookCategory("minecraft:glass_from_sand") ==
           RecipeBookCategory::FurnaceBlocks);            // glass.json: blocks
    assert(furnaceRecipeBookCategory("minecraft:iron_ingot_from_smelting") ==
           RecipeBookCategory::FurnaceMisc);  // iron_ingot_from_smelting_iron_ore.json: misc

    // 没登记的（datapack 追加的新配方）落到 vanilla codec 的默认值 MISC
    // （CraftingRecipe.java:47-49 / AbstractCookingRecipe 的 CookingBookCategory.MISC）。
    assert(craftingRecipeBookCategory("example:not_a_recipe") == RecipeBookCategory::CraftingMisc);
    assert(furnaceRecipeBookCategory("example:not_a_recipe") == RecipeBookCategory::FurnaceMisc);
}

void testEveryRegisteredRecipeHasAReachableCategory() {
    // 合成配方只会落在四个 Crafting* 分类里，熔炉配方只会落在三个 Furnace* 里
    // ——这正是 `CraftingRecipe.recipeBookCategory`（:38-45）与
    // `SmeltingRecipe.recipeBookCategory`（:41-48）两个 switch 的值域。
    for (const auto& recipe : recipeTable().crafting()) {
        const auto category = craftingRecipeBookCategory(recipe.identifier);
        assert(category == RecipeBookCategory::CraftingBuildingBlocks ||
               category == RecipeBookCategory::CraftingRedstone ||
               category == RecipeBookCategory::CraftingEquipment ||
               category == RecipeBookCategory::CraftingMisc);
    }
    for (const auto& recipe : recipeTable().furnace()) {
        const auto category = furnaceRecipeBookCategory(recipe.identifier);
        assert(category == RecipeBookCategory::FurnaceFood ||
               category == RecipeBookCategory::FurnaceBlocks ||
               category == RecipeBookCategory::FurnaceMisc);
    }
    // 哨兵的位置：13 个分类，照 RecipeBookCategories.java:7-19 的注册顺序。
    static_assert(static_cast<int>(RecipeBookCategory::Count) == 13);
    static_assert(static_cast<int>(RecipeBookCategory::CraftingBuildingBlocks) == 0);
    static_assert(static_cast<int>(RecipeBookCategory::Campfire) == 12);
}

// --- 3. 两个集合 ----------------------------------------------------------

void testAddRecipesHighlightsOnlyTheNewOnes() {
    RecipeBook book;
    assert(!book.contains("minecraft:torch"));
    assert(book.addRecipe("minecraft:torch") == 1);
    assert(book.contains("minecraft:torch"));
    assert(book.highlighted("minecraft:torch"));

    // `ServerRecipeBook.addRecipes`（:64-73）：已经认识的一条都不动。玩家看过之后
    // 再"解锁"一次不会重新点亮。
    book.removeHighlight("minecraft:torch");
    assert(!book.highlighted("minecraft:torch"));
    assert(book.addRecipe("minecraft:torch") == 0);
    assert(!book.highlighted("minecraft:torch"));
    assert(book.contains("minecraft:torch"));

    // `remove`（:48-51）：两个集合一起删。
    assert(book.addRecipe("minecraft:chest") == 1);
    assert(book.highlighted("minecraft:chest"));
    book.remove("minecraft:chest");
    assert(!book.contains("minecraft:chest"));
    assert(!book.highlighted("minecraft:chest"));
}

void testLoadNormalizes() {
    RecipeBook book;
    book.load({"minecraft:torch", "minecraft:chest", "minecraft:torch"}, {"minecraft:chest"});
    assert(book.known().size() == 2U);
    assert(book.known()[0] == "minecraft:chest");
    assert(book.known()[1] == "minecraft:torch");
    assert(book.highlighted("minecraft:chest"));
    assert(!book.highlighted("minecraft:torch"));
}

// --- 4. 解锁规则（本作的简化规则） ----------------------------------------

void testAcquiringAnIngredientUnlocksItsRecipes() {
    RecipeBook book;
    assert(!book.contains("minecraft:oak_planks"));
    // 橡木原木是 oak_planks 的唯一材料。
    assert(awardRecipesForAcquiredStack(book, blockStack(Block::OakLog, 1U)) >= 1);
    assert(book.contains("minecraft:oak_planks"));
    assert(book.highlighted("minecraft:oak_planks"));
    // 木板配方本身的材料是原木不是木板，所以拿到原木不会解锁「用木板做的」那些。
    assert(!book.contains("minecraft:sticks"));
    assert(!book.contains("minecraft:crafting_table"));

    // 拿到木板才解锁用木板的那些。
    assert(awardRecipesForAcquiredStack(book, blockStack(Block::OakPlanks, 1U)) >= 1);
    assert(book.contains("minecraft:sticks"));
    assert(book.contains("minecraft:crafting_table"));
    assert(book.contains("minecraft:chest"));

    // 熔炉配方同一条规则：拿到铁矿石解锁烧铁。
    assert(!book.contains("minecraft:iron_ingot_from_smelting"));
    static_cast<void>(awardRecipesForAcquiredStack(book, blockStack(Block::IronOre, 1U)));
    assert(book.contains("minecraft:iron_ingot_from_smelting"));

    // 第二次拿到同一种材料不再新增。
    assert(awardRecipesForAcquiredStack(book, blockStack(Block::OakPlanks, 1U)) == 0);
}

void testEmptyStackUnlocksNothing() {
    RecipeBook book;
    assert(awardRecipesForAcquiredStack(book, ItemStack{}) == 0);
    assert(book.known().empty());
}

void testInventoryScanUnlocksEverythingItHolds() {
    Inventory inventory;
    const std::array<ItemStack, 2U> held{blockStack(Block::OakPlanks, 4U),
                                         itemStack(&items::Coal, 1U)};
    putInInventory(inventory, held);
    RecipeBook book;
    static_cast<void>(awardRecipesForInventory(book, inventory));
    assert(book.contains("minecraft:crafting_table"));
    assert(book.contains("minecraft:torch"));  // 煤是火把的材料
}

// --- 5. 配方书列表 --------------------------------------------------------

void testEntriesRespectTheGridSize() {
    Inventory inventory;
    RecipeBook book;
    const auto small = recipeBookEntries(ContainerScreen::PlayerInventory, inventory, book);
    const auto large = recipeBookEntries(ContainerScreen::CraftingTable, inventory, book);
    assert(!small.empty());
    assert(large.size() > small.size());

    // 箱子是 3x3，2x2 的玩家格里显示不出来；木棍是 1x2，两屏都显示得出来。
    const auto has = [](const std::vector<RecipeBookEntry>& entries, std::string_view id) {
        return std::ranges::any_of(entries, [id](const RecipeBookEntry& entry) {
            return entry.identifier == id;
        });
    };
    assert(!has(small, "minecraft:chest"));
    assert(has(large, "minecraft:chest"));
    assert(has(small, "minecraft:sticks"));
    assert(has(large, "minecraft:sticks"));

    // 26.1 里箱子/附魔台/铁砧的菜单根本不是 RecipeBookMenu，没有配方书。
    assert(recipeBookEntries(ContainerScreen::Chest, inventory, book).empty());
    assert(recipeBookEntries(ContainerScreen::EnchantingTable, inventory, book).empty());
    assert(recipeBookEntries(ContainerScreen::Anvil, inventory, book).empty());

    // 熔炉屏给熔炉配方，而且只给熔炉配方。
    const auto furnace = recipeBookEntries(ContainerScreen::Furnace, inventory, book);
    assert(furnace.size() == recipeTable().furnace().size());
    for (const auto& entry : furnace) {
        assert(entry.category == RecipeBookCategory::FurnaceFood ||
               entry.category == RecipeBookCategory::FurnaceBlocks ||
               entry.category == RecipeBookCategory::FurnaceMisc);
    }
}

void testEntriesAreGroupedByCategory() {
    Inventory inventory;
    RecipeBook book;
    const auto entries = recipeBookEntries(ContainerScreen::CraftingTable, inventory, book);
    // 分类之间按 RecipeBookCategories.java 的注册顺序，所以下标是不降的；
    // 同一分类的排在一起（不降 == 分好组）。
    for (std::size_t index = 1U; index < entries.size(); ++index) {
        assert(static_cast<int>(entries[index - 1U].category) <=
               static_cast<int>(entries[index].category));
    }
}

void testCraftableAndUnlockedFlags() {
    Inventory inventory;
    const std::array<ItemStack, 1U> planks{blockStack(Block::OakPlanks, 4U)};
    putInInventory(inventory, planks);
    RecipeBook book;
    static_cast<void>(book.addRecipe("minecraft:crafting_table"));

    const auto entries = recipeBookEntries(ContainerScreen::CraftingTable, inventory, book);
    const auto find = [&entries](std::string_view id) -> const RecipeBookEntry& {
        const auto found = std::ranges::find(entries, id, &RecipeBookEntry::identifier);
        assert(found != entries.end());
        return *found;
    };
    const auto& table = find("minecraft:crafting_table");
    assert(table.craftable);
    assert(table.unlocked);
    assert(table.result.block == Block::CraftingTable);
    assert(table.result.count == 1U);

    const auto& chest = find("minecraft:chest");
    assert(!chest.craftable);  // 箱子要 8 块木板，只有 4 块
    assert(!chest.unlocked);
}

// --- 6. 一键填充 ----------------------------------------------------------

struct PlaceFixture final {
    Inventory inventory;
    CraftingSystem crafting;
};

void testPlaceOneSetIntoTheTableGrid() {
    PlaceFixture fixture;
    const std::array<ItemStack, 1U> planks{blockStack(Block::OakPlanks, 8U)};
    putInInventory(fixture.inventory, planks);
    const int before = totalItemCount(fixture.inventory, fixture.crafting, 9U);

    assert(placeRecipe(fixture.inventory, fixture.crafting, ContainerScreen::CraftingTable,
                       "minecraft:crafting_table", /*maxStack=*/false));

    // 2x2 的配方在 3x3 网格里不居中（`2 < 3/2.0` 为假），落在左上角 0/1/3/4。
    const std::array<std::size_t, 4U> filled{0U, 1U, 3U, 4U};
    for (std::size_t index = 0; index < 9U; ++index) {
        const bool expectFilled = std::ranges::find(filled, index) != filled.end();
        const ItemStack& slot = fixture.crafting.tableSlot(index);
        assert(slot.empty() != expectFilled);
        if (expectFilled) {
            assert(slot.block == Block::OakPlanks);
            assert(slot.count == 1U);
        }
    }
    assert(totalItemCount(fixture.inventory, fixture.crafting, 9U) == before);
}

void testShiftPlacesAsManyAsPossible() {
    PlaceFixture fixture;
    const std::array<ItemStack, 1U> planks{blockStack(Block::OakPlanks, 8U)};
    putInInventory(fixture.inventory, planks);
    const int before = totalItemCount(fixture.inventory, fixture.crafting, 9U);

    assert(placeRecipe(fixture.inventory, fixture.crafting, ContainerScreen::CraftingTable,
                       "minecraft:crafting_table", /*maxStack=*/true));

    // 8 块木板 -> 每格 2 块（biggestCraftableStack == 2），背包清空。
    for (const std::size_t index : {0U, 1U, 3U, 4U}) {
        assert(fixture.crafting.tableSlot(index).count == 2U);
    }
    for (const auto& stack : fixture.inventory.slots()) {
        assert(stack.empty());
    }
    assert(totalItemCount(fixture.inventory, fixture.crafting, 9U) == before);
}

void testCenteringPutsTheTorchInTheMiddleColumn() {
    PlaceFixture fixture;
    const std::array<ItemStack, 2U> supplies{itemStack(&items::Coal, 3U),
                                             itemStack(&items::Stick, 3U)};
    putInInventory(fixture.inventory, supplies);
    const int before = totalItemCount(fixture.inventory, fixture.crafting, 9U);

    assert(placeRecipe(fixture.inventory, fixture.crafting, ContainerScreen::CraftingTable,
                       "minecraft:torch", /*maxStack=*/false));

    // 1 宽 2 高的配方在 3x3 里水平居中、竖直靠上：1 号（煤）与 4 号（木棍）。
    assert(fixture.crafting.tableSlot(1U).item == &items::Coal);
    assert(fixture.crafting.tableSlot(1U).count == 1U);
    assert(fixture.crafting.tableSlot(4U).item == &items::Stick);
    assert(fixture.crafting.tableSlot(4U).count == 1U);
    for (const std::size_t index : {0U, 2U, 3U, 5U, 6U, 7U, 8U}) {
        assert(fixture.crafting.tableSlot(index).empty());
    }
    assert(totalItemCount(fixture.inventory, fixture.crafting, 9U) == before);
}

void testPlayerGridIsTwoByTwo() {
    PlaceFixture fixture;
    const std::array<ItemStack, 1U> planks{blockStack(Block::OakPlanks, 4U)};
    putInInventory(fixture.inventory, planks);
    const int before = totalItemCount(fixture.inventory, fixture.crafting, 4U);

    // 木棍配方是 1 宽 2 高；2x2 网格里 `1 < 2/2.0` 为假不居中，落在 0 与 2。
    assert(placeRecipe(fixture.inventory, fixture.crafting, ContainerScreen::PlayerInventory,
                       "minecraft:sticks", /*maxStack=*/false));
    assert(fixture.crafting.playerSlot(0U).block == Block::OakPlanks);
    assert(fixture.crafting.playerSlot(0U).count == 1U);
    assert(fixture.crafting.playerSlot(1U).empty());
    assert(fixture.crafting.playerSlot(2U).block == Block::OakPlanks);
    assert(fixture.crafting.playerSlot(2U).count == 1U);
    assert(fixture.crafting.playerSlot(3U).empty());
    assert(totalItemCount(fixture.inventory, fixture.crafting, 4U) == before);

    // 3x3 的箱子配方在 2x2 的玩家格里根本放不下。
    assert(!placeRecipe(fixture.inventory, fixture.crafting, ContainerScreen::PlayerInventory,
                        "minecraft:chest", /*maxStack=*/false));
}

void testSecondClickAddsALayerAndTheThirdRefuses() {
    PlaceFixture fixture;
    const std::array<ItemStack, 1U> planks{blockStack(Block::OakPlanks, 8U)};
    putInInventory(fixture.inventory, planks);
    const int before = totalItemCount(fixture.inventory, fixture.crafting, 9U);

    assert(placeRecipe(fixture.inventory, fixture.crafting, ContainerScreen::CraftingTable,
                       "minecraft:crafting_table", /*maxStack=*/false));
    assert(fixture.crafting.tableSlot(0U).count == 1U);

    // 第二次普通点击：网格里已经是这条配方，`calculateAmountToCraft` 取
    // 「最小的一摞 + 1」= 2，于是每格变成 2。
    assert(placeRecipe(fixture.inventory, fixture.crafting, ContainerScreen::CraftingTable,
                       "minecraft:crafting_table", /*maxStack=*/false));
    for (const std::size_t index : {0U, 1U, 3U, 4U}) {
        assert(fixture.crafting.tableSlot(index).count == 2U);
    }

    // 第三次：材料只够做 2 次，再加一层就超了 —— `ServerPlaceRecipe.java:93-100`
    // 的闸门，**什么都不做**。
    assert(!placeRecipe(fixture.inventory, fixture.crafting, ContainerScreen::CraftingTable,
                        "minecraft:crafting_table", /*maxStack=*/false));
    for (const std::size_t index : {0U, 1U, 3U, 4U}) {
        assert(fixture.crafting.tableSlot(index).count == 2U);
    }
    assert(totalItemCount(fixture.inventory, fixture.crafting, 9U) == before);
}

void testNotEnoughMaterialChangesNothing() {
    PlaceFixture fixture;
    const std::array<ItemStack, 1U> planks{blockStack(Block::OakPlanks, 3U)};
    putInInventory(fixture.inventory, planks);
    const int before = totalItemCount(fixture.inventory, fixture.crafting, 9U);

    assert(!placeRecipe(fixture.inventory, fixture.crafting, ContainerScreen::CraftingTable,
                        "minecraft:crafting_table", /*maxStack=*/false));
    for (std::size_t index = 0; index < 9U; ++index) {
        assert(fixture.crafting.tableSlot(index).empty());
    }
    assert(fixture.inventory.slots()[0].count == 3U);
    assert(totalItemCount(fixture.inventory, fixture.crafting, 9U) == before);

    // 不存在的配方标识符同样什么都不动。
    assert(!placeRecipe(fixture.inventory, fixture.crafting, ContainerScreen::CraftingTable,
                        "example:no_such_recipe", /*maxStack=*/false));
    assert(totalItemCount(fixture.inventory, fixture.crafting, 9U) == before);
}

void testGridIsEmptiedBackIntoTheInventoryFirst() {
    // 网格里先摆着别的东西：一键填充要把它们退回背包，再摆新配方。
    PlaceFixture fixture;
    const std::array<ItemStack, 1U> planks{blockStack(Block::OakPlanks, 8U)};
    putInInventory(fixture.inventory, planks);
    fixture.crafting.tableGridSlot(8U) = blockStack(Block::Cobblestone, 5U);
    const int before = totalItemCount(fixture.inventory, fixture.crafting, 9U);

    assert(placeRecipe(fixture.inventory, fixture.crafting, ContainerScreen::CraftingTable,
                       "minecraft:crafting_table", /*maxStack=*/false));
    assert(fixture.crafting.tableSlot(8U).empty());
    const bool cobbleBack = std::ranges::any_of(
        fixture.inventory.slots(), [](const ItemStack& stack) {
            return stack.block == Block::Cobblestone && stack.count == 5U;
        });
    assert(cobbleBack);
    assert(totalItemCount(fixture.inventory, fixture.crafting, 9U) == before);
}

void testFullInventoryRefusesRatherThanDropping() {
    // 网格里有东西、背包又没有一格能收下它：`testClearGrid`（:195-228）判定退不
    // 回去，整个操作作废（**不**把玩家的东西扔到地上）。
    //
    // ★ 夹具形状是有讲究的：材料**必须够**。背包塞满石头就完事的话，
    //   `canCraft` 那一关会先把它挡下来，`testClearGrid` 那一关根本轮不到——
    //   于是把 testClearGrid 改成恒真也照样"红不了"。所以这里留一摞 8 块木板
    //   （做得出工作台），只让圆石**无处可退**：其余 35 格全是满 64 的石头，
    //   木板那格既装不下圆石也不是空格。
    PlaceFixture fixture;
    for (std::size_t index = 0; index + 1U < Inventory::kSlotCount; ++index) {
        fixture.inventory.mutableSlot(index) = blockStack(Block::Stone, 64U);
    }
    fixture.inventory.mutableSlot(Inventory::kSlotCount - 1U) =
        blockStack(Block::OakPlanks, 8U);
    fixture.crafting.tableGridSlot(8U) = blockStack(Block::Cobblestone, 5U);
    const int before = totalItemCount(fixture.inventory, fixture.crafting, 9U);
    // 前置：材料确实够（否则这个夹具分辨不出 testClearGrid 有没有起作用）。
    assert(canCraft(fixture.inventory, craftingRecipe("minecraft:crafting_table"), 1));

    assert(!placeRecipe(fixture.inventory, fixture.crafting, ContainerScreen::CraftingTable,
                        "minecraft:crafting_table", /*maxStack=*/false));
    // 什么都没动：圆石还在网格里，木板还是 8 块，别的格子一个都没被摆上。
    assert(fixture.crafting.tableSlot(8U).count == 5U);
    assert(fixture.inventory.slots()[Inventory::kSlotCount - 1U].count == 8U);
    for (const std::size_t index : {0U, 1U, 3U, 4U}) {
        assert(fixture.crafting.tableSlot(index).empty());
    }
    assert(totalItemCount(fixture.inventory, fixture.crafting, 9U) == before);
}

void testGridContentsCountAsAvailableMaterial() {
    // `ServerPlaceRecipe.java:44-46`：可用材料 = 背包 + 网格里已经摆着的东西。
    // 背包里一块木板都没有，8 块全在网格里散着，一键填充照样摆得出工作台。
    PlaceFixture fixture;
    for (std::size_t index = 0; index < 8U; ++index) {
        fixture.crafting.tableGridSlot(index) = blockStack(Block::OakPlanks, 1U);
    }
    const int before = totalItemCount(fixture.inventory, fixture.crafting, 9U);
    assert(placeRecipe(fixture.inventory, fixture.crafting, ContainerScreen::CraftingTable,
                       "minecraft:crafting_table", /*maxStack=*/true));
    for (const std::size_t index : {0U, 1U, 3U, 4U}) {
        assert(fixture.crafting.tableSlot(index).count == 2U);
    }
    assert(totalItemCount(fixture.inventory, fixture.crafting, 9U) == before);
}

// --- 7. 持久化 ------------------------------------------------------------

[[nodiscard]] std::filesystem::path scratchRoot(std::string_view name) {
    const auto root = std::filesystem::temp_directory_path() /
        ("mc_rebedrock_recipe_book_" + std::string{name});
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

void testRecipeBookSurvivesASaveRoundTrip() {
    const auto root = scratchRoot("round_trip");
    mc::persistence::SaveRepository repository{root};

    mc::persistence::SaveGame game;
    game.summary.identifier = "world";
    game.summary.displayName = "world";
    game.unlockedRecipes = {"minecraft:chest", "minecraft:torch"};
    game.highlightedRecipes = {"minecraft:torch"};
    repository.save(game);

    const auto loaded = repository.load("world");
    assert(loaded.unlockedRecipes == game.unlockedRecipes);
    assert(loaded.highlightedRecipes == game.highlightedRecipes);
    std::filesystem::remove_all(root);
}

void testEmptyRecipeBookRoundTrips() {
    const auto root = scratchRoot("empty");
    mc::persistence::SaveRepository repository{root};
    mc::persistence::SaveGame game;
    game.summary.identifier = "world";
    game.summary.displayName = "world";
    repository.save(game);
    const auto loaded = repository.load("world");
    assert(loaded.unlockedRecipes.empty());
    assert(loaded.highlightedRecipes.empty());
    std::filesystem::remove_all(root);
}

// 把一个 world.dat 里的 RCPB 块整个摘掉，模拟「配方书这一层还不存在时写的存档」。
void stripRecipeBookBlock(const std::filesystem::path& worldDat) {
    std::vector<std::uint8_t> bytes;
    {
        std::ifstream input{worldDat, std::ios::binary | std::ios::ate};
        assert(input);
        const auto length = static_cast<std::size_t>(input.tellg());
        bytes.resize(length);
        input.seekg(0);
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
    }
    constexpr std::array<std::uint8_t, 4U> tag{'R', 'C', 'P', 'B'};
    const auto found = std::ranges::search(bytes, tag);
    assert(!found.empty());
    const std::size_t start = static_cast<std::size_t>(found.begin() - bytes.begin());
    std::uint32_t blockSize = 0U;
    for (std::size_t index = 0; index < 4U; ++index) {
        blockSize |= static_cast<std::uint32_t>(bytes[start + 4U + index]) << (index * 8U);
    }
    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                bytes.begin() + static_cast<std::ptrdiff_t>(start + blockSize));
    // 尾部 8 字节是 FNV-1a 校验和，重算。
    bytes.resize(bytes.size() - sizeof(std::uint64_t));
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
        bytes.push_back(static_cast<std::uint8_t>(hash >> (index * 8U)));
    }
    std::ofstream output{worldDat, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void testOldSaveWithoutTheBlockLoadsAsAnEmptyBook() {
    const auto root = scratchRoot("legacy");
    mc::persistence::SaveRepository repository{root};
    mc::persistence::SaveGame game;
    game.summary.identifier = "world";
    game.summary.displayName = "world";
    game.unlockedRecipes = {"minecraft:torch"};
    game.highlightedRecipes = {"minecraft:torch"};
    game.playerExperienceLevel = 7;  // 摘掉块之后别的字段必须还读得回来
    repository.save(game);

    stripRecipeBookBlock(root / "world" / "world.dat");

    const auto loaded = repository.load("world");
    assert(loaded.unlockedRecipes.empty());
    assert(loaded.highlightedRecipes.empty());
    assert(loaded.playerExperienceLevel == 7);
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    static_cast<void>(contentRegistry());
    recipeTable().loadBuiltinDefaults();

    testBiggestCraftableStackIsNotDivision();
    testMatchingLetsTwoSlotsShareOneItemType();
    testShapedRecipeCountsEveryFilledCell();
    testTwoDistinctIngredientsTakeTheMinimum();
    testUnusableStacksAreNotAccounted();
    testBlockStackKeysCollapse();
    testInventoryConvenienceEntryPoints();

    testRecipeBookCategoriesMatchVanillaJson();
    testEveryRegisteredRecipeHasAReachableCategory();

    testAddRecipesHighlightsOnlyTheNewOnes();
    testLoadNormalizes();

    testAcquiringAnIngredientUnlocksItsRecipes();
    testEmptyStackUnlocksNothing();
    testInventoryScanUnlocksEverythingItHolds();

    testEntriesRespectTheGridSize();
    testEntriesAreGroupedByCategory();
    testCraftableAndUnlockedFlags();

    testPlaceOneSetIntoTheTableGrid();
    testShiftPlacesAsManyAsPossible();
    testCenteringPutsTheTorchInTheMiddleColumn();
    testPlayerGridIsTwoByTwo();
    testSecondClickAddsALayerAndTheThirdRefuses();
    testNotEnoughMaterialChangesNothing();
    testGridIsEmptiedBackIntoTheInventoryFirst();
    testFullInventoryRefusesRatherThanDropping();
    testGridContentsCountAsAvailableMaterial();

    testRecipeBookSurvivesASaveRoundTrip();
    testEmptyRecipeBookRoundTrips();
    testOldSaveWithoutTheBlockLoadsAsAnEmptyBook();

    std::cout << "recipe_book: all checks passed\n";
    return 0;
}

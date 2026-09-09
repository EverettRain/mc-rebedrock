#include "gameplay/RecipeBook.hpp"

#include "compat/ContentNamespace.hpp"
#include "gameplay/RecipeTable.hpp"
#include "gameplay/StackedItemContents.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <utility>

namespace mc::gameplay {
namespace {

// 两个集合都是有序 vector，插入/查找都走二分。
[[nodiscard]] bool containsSorted(const std::vector<std::string>& sorted,
                                  std::string_view identifier) {
    return std::ranges::binary_search(sorted, identifier, std::less<>{});
}

bool insertSorted(std::vector<std::string>& sorted, std::string_view identifier) {
    const auto position = std::ranges::lower_bound(sorted, identifier, std::less<>{});
    if (position != sorted.end() && *position == identifier) {
        return false;
    }
    sorted.emplace(position, identifier);
    return true;
}

void eraseSorted(std::vector<std::string>& sorted, std::string_view identifier) {
    const auto position = std::ranges::lower_bound(sorted, identifier, std::less<>{});
    if (position != sorted.end() && *position == identifier) {
        sorted.erase(position);
    }
}

// 存档/外部来的整张列表：先逐条归一化命名空间，再排序去重。
// ★ 归一化必须在去重**之前**：老存档里同一条配方可能同时以 `minecraft:` 与
// `rebedrock:` 两种拼法出现（一次是老版本写的、一次是新版本写的），先归一化
// 才认得出它们是同一条。
void normalize(std::vector<std::string>& entries) {
    for (auto& entry : entries) {
        if (compat::isVanillaNamespaced(entry)) {
            entry = compat::canonicalContentId(entry);
        }
    }
    std::ranges::sort(entries);
    const auto duplicates = std::ranges::unique(entries);
    entries.erase(duplicates.begin(), duplicates.end());
}

// 这一屏的合成网格是几乘几。没有网格的屏返回 0。
struct GridShape final {
    std::size_t width = 0U;
    std::size_t height = 0U;
    [[nodiscard]] bool valid() const { return width > 0U && height > 0U; }
    [[nodiscard]] std::size_t size() const { return width * height; }
};

[[nodiscard]] GridShape gridShapeOf(ContainerScreen screen) {
    switch (screen) {
    case ContainerScreen::PlayerInventory:
        return GridShape{2U, 2U};
    case ContainerScreen::CraftingTable:
        return GridShape{3U, 3U};
    case ContainerScreen::Furnace:
    case ContainerScreen::Chest:
    case ContainerScreen::EnchantingTable:
    case ContainerScreen::Anvil:
    case ContainerScreen::Count:
        return GridShape{};
    }
    return GridShape{};
}

// `CraftingRecipeBookComponent.canDisplay`（:44-55）：有形状的看宽高装不装得下，
// 无序的看格数够不够。
[[nodiscard]] bool canDisplay(const CraftingRecipe& recipe, const GridShape& grid) {
    if (recipe.shapeless) {
        return grid.size() >= recipe.ingredients.size();
    }
    return grid.width >= recipe.width && grid.height >= recipe.height;
}

// 这一屏的网格存储。`CraftingSystem` 把 2x2 与 3x3 分成两个数组，这里取出对应的
// 那一片可写引用。
[[nodiscard]] std::vector<ItemStack*> gridSlotsOf(CraftingSystem& crafting,
                                                  ContainerScreen screen,
                                                  const GridShape& grid) {
    std::vector<ItemStack*> slots;
    slots.reserve(grid.size());
    for (std::size_t index = 0; index < grid.size(); ++index) {
        slots.push_back(screen == ContainerScreen::CraftingTable
                            ? &crafting.tableGridSlot(index)
                            : &crafting.playerGridSlot(index));
    }
    return slots;
}

[[nodiscard]] std::vector<ItemStack> gridContents(const std::vector<ItemStack*>& slots) {
    std::vector<ItemStack> contents;
    contents.reserve(slots.size());
    for (const auto* slot : slots) contents.push_back(*slot);
    return contents;
}

// `Inventory.hasRemainingSpaceForItem`（Inventory.java:94-99）
[[nodiscard]] bool hasRemainingSpaceForItem(const ItemStack& slot, const ItemStack& incoming) {
    return !slot.empty() && sameItem(slot, incoming) &&
        itemMaximumStackSize(slot) > 1U && slot.count < itemMaximumStackSize(slot);
}

// `ServerPlaceRecipe.testClearGrid`（:195-228）：网格里的东西退得回背包吗。退不
// 回就整个操作作废（vanilla 在 allowDroppingItemsToClear=false 时也是这样）。
[[nodiscard]] bool testClearGrid(const Inventory& inventory,
                                 const std::vector<ItemStack*>& gridSlots) {
    std::vector<ItemStack> freeSlots;
    const int freeSlotsInInventory = static_cast<int>(std::ranges::count_if(
        inventory.slots(), [](const ItemStack& stack) { return stack.empty(); }));
    for (const auto* slotPointer : gridSlots) {
        ItemStack stack = *slotPointer;
        if (stack.empty()) continue;
        const bool hasSpace = std::ranges::any_of(
            inventory.slots(),
            [&stack](const ItemStack& candidate) {
                return hasRemainingSpaceForItem(candidate, stack);
            });
        if (!hasSpace) {
            if (static_cast<int>(freeSlots.size()) <= freeSlotsInInventory) {
                for (auto& pending : freeSlots) {
                    if (sameItem(pending, stack) &&
                        pending.count != itemMaximumStackSize(pending) &&
                        pending.count + stack.count <= itemMaximumStackSize(pending)) {
                        pending.count =
                            static_cast<std::uint8_t>(pending.count + stack.count);
                        stack = {};
                        break;
                    }
                }
                if (!stack.empty()) {
                    if (static_cast<int>(freeSlots.size()) >= freeSlotsInInventory) {
                        return false;
                    }
                    freeSlots.push_back(stack);
                }
            } else {
                return false;
            }
        }
    }
    return true;
}

// `ServerPlaceRecipe.clearGrid`（:80-88）：网格里原有的先退回背包。
void clearGrid(Inventory& inventory, const std::vector<ItemStack*>& gridSlots) {
    for (auto* slot : gridSlots) {
        if (slot->empty()) continue;
        ItemStack leftover = *slot;
        static_cast<void>(inventory.add(leftover));
        // 退不回去的留在格子里（testClearGrid 已经先保证过退得回，这一行是不变量
        // 的落点：物品既不凭空生成也不被吞掉）。
        *slot = leftover;
    }
}

// `Inventory.findSlotMatchingCraftingIngredient`（Inventory.java:148-160）
[[nodiscard]] int findSlotMatchingCraftingIngredient(const Inventory& inventory,
                                                     const ItemStack& wantedKey,
                                                     const ItemStack& existingInTarget) {
    const auto& slots = inventory.slots();
    for (std::size_t index = 0; index < slots.size(); ++index) {
        const ItemStack& candidate = slots[index];
        if (candidate.empty()) continue;
        if (!(stackedContentsKey(candidate) == wantedKey)) continue;
        if (!isUsableForCrafting(candidate)) continue;
        if (!existingInTarget.empty() && !sameItem(existingInTarget, candidate)) continue;
        return static_cast<int>(index);
    }
    return -1;
}

// `ServerPlaceRecipe.moveItemToGrid`（:170-193）。返回还差几个；-1 = 背包里没有
// 这种材料了。
[[nodiscard]] int moveItemToGrid(Inventory& inventory, ItemStack& targetSlot,
                                 const ItemStack& itemKey, int count) {
    const int inventorySlot =
        findSlotMatchingCraftingIngredient(inventory, itemKey, targetSlot);
    if (inventorySlot == -1) {
        return -1;
    }
    ItemStack& source = inventory.mutableSlot(static_cast<std::size_t>(inventorySlot));
    ItemStack taken = source;
    if (count < static_cast<int>(source.count)) {
        taken.count = static_cast<std::uint8_t>(count);
        source.count = static_cast<std::uint8_t>(source.count - count);
    } else {
        source = {};
    }
    const int takenCount = static_cast<int>(taken.count);
    if (targetSlot.empty()) {
        targetSlot = taken;
    } else {
        targetSlot.count = static_cast<std::uint8_t>(targetSlot.count + takenCount);
    }
    return count - takenCount;
}

// `PlaceRecipeHelper.placeRecipe`（PlaceRecipeHelper.java:19-57）。配方比网格小时
// 会居中：3x3 网格里的 1x1 配方落在正中那格（4 号），1 宽 2 高的配方落在 1 与 4。
// 逐字移植，包括那两处 `< gridWidth / 2.0F` 的浮点比较——把它们代数化简成整数
// 判断会在偶数网格上改结果。
template <typename Output>
void placeRecipeIntoGrid(int gridWidth, int gridHeight, int recipeWidth, int recipeHeight,
                         std::span<const int> entries, const Output& output) {
    std::size_t next = 0U;
    int gridIndex = 0;
    for (int gridYPos = 0; gridYPos < gridHeight; ++gridYPos) {
        bool shouldCenterRecipe =
            static_cast<float>(recipeHeight) < static_cast<float>(gridHeight) / 2.0F;
        int startPosCenterRecipe = static_cast<int>(std::floor(
            static_cast<float>(gridHeight) / 2.0F - static_cast<float>(recipeHeight) / 2.0F));
        if (shouldCenterRecipe && startPosCenterRecipe > gridYPos) {
            gridIndex += gridWidth;
            ++gridYPos;
        }
        for (int gridXPos = 0; gridXPos < gridWidth; ++gridXPos) {
            if (next >= entries.size()) {
                return;
            }
            shouldCenterRecipe =
                static_cast<float>(recipeWidth) < static_cast<float>(gridWidth) / 2.0F;
            startPosCenterRecipe = static_cast<int>(std::floor(
                static_cast<float>(gridWidth) / 2.0F - static_cast<float>(recipeWidth) / 2.0F));
            int totalRecipeWidthInGrid = recipeWidth;
            bool addIngredientToSlot = gridXPos < recipeWidth;
            if (shouldCenterRecipe) {
                totalRecipeWidthInGrid = startPosCenterRecipe + recipeWidth;
                addIngredientToSlot = startPosCenterRecipe <= gridXPos &&
                    gridXPos < startPosCenterRecipe + recipeWidth;
            }
            if (addIngredientToSlot) {
                output(entries[next], gridIndex);
                ++next;
            } else if (totalRecipeWidthInGrid == gridXPos) {
                gridIndex += gridWidth - gridXPos;
                break;
            }
            ++gridIndex;
        }
    }
}

// `ServerPlaceRecipe.clampToMaxStackSize`（:137-143）
[[nodiscard]] int clampToMaxStackSize(int value, const std::vector<ItemStack>& items) {
    for (const auto& item : items) {
        value = std::min(value, static_cast<int>(itemMaximumStackSize(item)));
    }
    return value;
}

// `ServerPlaceRecipe.calculateAmountToCraft`（:145-168）
[[nodiscard]] int calculateAmountToCraft(int biggestCraftableStack, bool recipeMatchesPlaced,
                                         bool useMaxItems,
                                         const std::vector<ItemStack*>& gridSlots) {
    if (useMaxItems) {
        return biggestCraftableStack;
    }
    if (recipeMatchesPlaced) {
        int smallestStackSize = std::numeric_limits<int>::max();
        for (const auto* slot : gridSlots) {
            if (!slot->empty() && smallestStackSize > static_cast<int>(slot->count)) {
                smallestStackSize = static_cast<int>(slot->count);
            }
        }
        if (smallestStackSize != std::numeric_limits<int>::max()) {
            ++smallestStackSize;
        }
        return smallestStackSize;
    }
    return 1;
}

// ADV-0b 归一化边界③：按标识符找配方（placeRecipe 的入口）。界面/命令递进来的
// 可能是 vanilla 拼法。
[[nodiscard]] const CraftingRecipe* craftingRecipeByIdentifier(std::string_view identifier) {
    const std::string canonical = compat::canonicalContentId(identifier);
    const auto recipes = recipeTable().crafting();
    const auto found = std::ranges::find(recipes, canonical, &CraftingRecipe::identifier);
    return found == recipes.end() ? nullptr : &*found;
}

} // namespace

// ADV-0b 归一化边界②：配方书的**每一个**公开入口。两个集合里存的一律是
// `rebedrock:` 规范形，所以入口把 `minecraft:` 换掉、出口（known()/highlight()）
// 就只会吐规范形。命令、网络、存档、成就奖励走的都是这几个函数，逐个归一化才
// 堵得住——只在某一个调用点归一化是无效的（「同名 block/item 双端桥」的教训）。
bool RecipeBook::add(std::string_view identifier) {
    const std::string canonical = compat::canonicalContentId(identifier);
    return insertSorted(known_, canonical);
}

bool RecipeBook::contains(std::string_view identifier) const {
    if (!compat::isVanillaNamespaced(identifier)) {
        return containsSorted(known_, identifier); // 已是规范形：不分配
    }
    return containsSorted(known_, compat::canonicalContentId(identifier));
}

void RecipeBook::remove(std::string_view identifier) {
    const std::string canonical = compat::canonicalContentId(identifier);
    eraseSorted(known_, canonical);
    eraseSorted(highlight_, canonical);
}

void RecipeBook::removeHighlight(std::string_view identifier) {
    eraseSorted(highlight_, compat::canonicalContentId(identifier));
}

bool RecipeBook::highlighted(std::string_view identifier) const {
    if (!compat::isVanillaNamespaced(identifier)) {
        return containsSorted(highlight_, identifier);
    }
    return containsSorted(highlight_, compat::canonicalContentId(identifier));
}

int RecipeBook::addRecipes(std::span<const std::string_view> identifiers) {
    int added = 0;
    for (const auto identifier : identifiers) {
        const std::string canonical = compat::canonicalContentId(identifier);
        // `ServerRecipeBook.addRecipes`（:64-73）：认识的一条都不动。
        // ★ 这一关也是 `recipe_unlocked` 触发器回路的断点（ADV-1）：发了配方 ->
        // 成就完成 -> 又发同一条配方，第二次在这里被 contains 挡住。
        if (containsSorted(known_, canonical)) continue;
        static_cast<void>(insertSorted(known_, canonical));
        static_cast<void>(insertSorted(highlight_, canonical));
        ++added;
    }
    return added;
}

int RecipeBook::addRecipe(std::string_view identifier) {
    const std::array<std::string_view, 1U> one{identifier};
    return addRecipes(one);
}

void RecipeBook::load(std::vector<std::string> known, std::vector<std::string> highlight) {
    known_ = std::move(known);
    highlight_ = std::move(highlight);
    normalize(known_);
    normalize(highlight_);
}

void RecipeBook::clear() {
    known_.clear();
    highlight_.clear();
}

int awardRecipesForAcquiredStack(RecipeBook& book, const ItemStack& acquired) {
    if (acquired.empty()) return 0;
    int added = 0;
    for (const auto& recipe : recipeTable().crafting()) {
        if (book.contains(recipe.identifier)) continue;
        const bool isIngredient = std::ranges::any_of(
            recipe.ingredients, [&acquired](const RecipeIngredient& ingredient) {
                return ingredient.kind != IngredientKind::Empty &&
                    ingredientMatches(ingredient, acquired);
            });
        if (isIngredient) {
            added += book.addRecipe(recipe.identifier);
        }
    }
    for (const auto& recipe : recipeTable().furnace()) {
        if (book.contains(recipe.identifier)) continue;
        if (recipe.input.kind != IngredientKind::Empty &&
            ingredientMatches(recipe.input, acquired)) {
            added += book.addRecipe(recipe.identifier);
        }
    }
    return added;
}

int awardRecipesForInventory(RecipeBook& book, const Inventory& inventory) {
    int added = 0;
    for (const auto& stack : inventory.slots()) {
        added += awardRecipesForAcquiredStack(book, stack);
    }
    return added;
}

std::vector<RecipeBookEntry> recipeBookEntries(ContainerScreen screen,
                                               const Inventory& inventory,
                                               const RecipeBook& book) {
    std::vector<RecipeBookEntry> entries;
    if (screen == ContainerScreen::Furnace) {
        StackedItemContents contents;
        contents.accountInventory(inventory);
        for (const auto& recipe : recipeTable().furnace()) {
            RecipeBookEntry entry;
            entry.identifier = recipe.identifier;
            entry.result = recipe.output;
            entry.category = furnaceRecipeBookCategory(recipe.identifier);
            entry.craftable = std::ranges::any_of(
                inventory.slots(), [&recipe](const ItemStack& stack) {
                    return !stack.empty() && isUsableForCrafting(stack) &&
                        ingredientMatches(recipe.input, stack);
                });
            entry.unlocked = book.contains(recipe.identifier);
            entries.push_back(entry);
        }
    } else {
        const GridShape grid = gridShapeOf(screen);
        if (!grid.valid()) {
            return entries;
        }
        StackedItemContents contents;
        contents.accountInventory(inventory);
        for (const auto& recipe : recipeTable().crafting()) {
            if (!canDisplay(recipe, grid)) continue;
            RecipeBookEntry entry;
            entry.identifier = recipe.identifier;
            entry.result = recipe.output;
            entry.category = craftingRecipeBookCategory(recipe.identifier);
            entry.craftable = contents.canCraft(recipe, 1);
            entry.unlocked = book.contains(recipe.identifier);
            entries.push_back(entry);
        }
    }
    // 分类分组：分类之间按 RecipeBookCategories 的注册顺序，同一分类内保持配方表
    // 的顺序（stable，所以两次调用给出同一个列表）。
    std::ranges::stable_sort(entries, {}, [](const RecipeBookEntry& entry) {
        return static_cast<std::uint8_t>(entry.category);
    });
    return entries;
}

bool placeRecipe(Inventory& inventory, CraftingSystem& crafting, ContainerScreen screen,
                 std::string_view recipeIdentifier, bool maxStack) {
    const GridShape grid = gridShapeOf(screen);
    if (!grid.valid()) {
        return false;
    }
    const CraftingRecipe* recipe = craftingRecipeByIdentifier(recipeIdentifier);
    if (recipe == nullptr || !canDisplay(*recipe, grid)) {
        return false;
    }
    const auto gridSlots = gridSlotsOf(crafting, screen, grid);

    // `ServerPlaceRecipe.placeRecipe`（:40-42）静态入口的第一关：网格退不回背包
    // 就什么都不做。本作的一键填充永远走 allowDroppingItemsToClear=false 那一路
    // ——那是「宁可不填也不把玩家的东西扔到地上」。
    if (!testClearGrid(inventory, gridSlots)) {
        return false;
    }

    // `ServerPlaceRecipe.placeRecipe`（:44-46）：可用材料 = 背包 **加上** 网格里
    // 已经摆着的东西。
    StackedItemContents available;
    available.accountInventory(inventory);
    for (const auto* slot : gridSlots) {
        available.accountSimpleStack(*slot);
    }

    // `tryPlaceRecipe`（:68-78）。★ 如实偏离：vanilla 在做不出来时会先把网格清
    // 空（clearGrid）再返回 PLACE_GHOST_RECIPE 让界面画"幽灵配方"。本作这个接口
    // 的约定是「false = 什么都没动」，而幽灵配方是界面侧的事，所以这里做不出来就
    // 原样不动。
    if (!available.canCraft(*recipe, 1)) {
        return false;
    }

    const auto placed = gridContents(gridSlots);
    const bool recipeMatchesPlaced = craftingRecipeMatches(*recipe, placed, grid.width);
    const int biggest = available.biggestCraftableStack(*recipe);

    // `placeRecipe`（:93-100）：网格里已经是这条配方、而且再加一层就超过某一格的
    // 堆叠上限（或者超过做得出来的次数）时，什么都不做。
    if (recipeMatchesPlaced) {
        for (const auto* slot : gridSlots) {
            if (slot->empty()) continue;
            if (std::min(biggest, static_cast<int>(itemMaximumStackSize(*slot))) <
                static_cast<int>(slot->count) + 1) {
                return false;
            }
        }
    }

    const int amountToCraft =
        calculateAmountToCraft(biggest, recipeMatchesPlaced, maxStack, gridSlots);

    std::vector<ItemStack> itemsUsedPerIngredient;
    const auto collect = [&itemsUsedPerIngredient](const ItemStack& item) {
        itemsUsedPerIngredient.push_back(item);
    };
    if (!available.canCraft(*recipe, amountToCraft, collect)) {
        return false;
    }
    const int adjustedAmountToCraft = clampToMaxStackSize(amountToCraft, itemsUsedPerIngredient);
    if (adjustedAmountToCraft != amountToCraft) {
        itemsUsedPerIngredient.clear();
        if (!available.canCraft(*recipe, adjustedAmountToCraft, collect)) {
            return false;
        }
    }
    if (adjustedAmountToCraft <= 0) {
        return false;
    }

    clearGrid(inventory, gridSlots);

    const auto placement = recipePlacement(*recipe);
    // `PlaceRecipeHelper.placeRecipe`（:9-17）：有形状的按配方自己的宽高摆，
    // 无序的按整个网格摆。
    const int recipeWidth = recipe->shapeless ? static_cast<int>(grid.width)
                                              : static_cast<int>(recipe->width);
    const int recipeHeight = recipe->shapeless ? static_cast<int>(grid.height)
                                               : static_cast<int>(recipe->height);
    bool exhausted = false;
    placeRecipeIntoGrid(
        static_cast<int>(grid.width), static_cast<int>(grid.height), recipeWidth, recipeHeight,
        placement.slotsToIngredientIndex,
        [&](int ingredientIndex, int gridIndex) {
            if (exhausted || ingredientIndex == -1) return;
            ItemStack& targetSlot = *gridSlots[static_cast<std::size_t>(gridIndex)];
            const ItemStack& itemUsed =
                itemsUsedPerIngredient[static_cast<std::size_t>(ingredientIndex)];
            int remaining = adjustedAmountToCraft;
            while (remaining > 0) {
                remaining = moveItemToGrid(inventory, targetSlot, itemUsed, remaining);
                if (remaining == -1) {
                    exhausted = true;
                    return;
                }
            }
        });
    return true;
}

} // namespace mc::gameplay

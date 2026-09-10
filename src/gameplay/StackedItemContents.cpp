#include "gameplay/StackedItemContents.hpp"

#include <algorithm>
#include <cstddef>

namespace mc::gameplay {
namespace {

// 一格材料收不收这种物品。谓词本身住在 CraftingSystem（配方匹配的单一真相源），
// 这里只是把「键」当成一堆数量为 1 的物品递进去。
[[nodiscard]] bool acceptsItem(const RecipeIngredient& ingredient, const ItemStack& key) {
    return ingredientMatches(ingredient, key);
}

} // namespace

ItemStack stackedContentsKey(const ItemStack& stack) {
    ItemStack key{};
    if (isBlockStack(stack)) {
        // 方块堆按方块定身份，item 指针一律归一到它注册的 BlockItem（没注册的
        // 留空），这样「空哨兵写法」与「BlockItem 写法」折成同一个键。
        key.block = stack.block;
        key.item = blockItemFor(stack.block);
    } else {
        key.block = stack.block;
        key.item = stack.item;
    }
    key.count = 1U;
    return key;
}

bool StackedItemContents::hasAtLeast(const ItemStack& key, int count) const {
    const auto row = std::ranges::find(amounts_, key, &std::pair<ItemStack, int>::first);
    return row != amounts_.end() && row->second >= count;
}

void StackedItemContents::take(const ItemStack& key, int amount) {
    const auto row = std::ranges::find(amounts_, key, &std::pair<ItemStack, int>::first);
    const int previous = row == amounts_.end() ? 0 : row->second;
    if (previous < amount) {
        // `StackedContents.take`（StackedContents.java:22-27）在这里抛
        // IllegalStateException：拿走的比有的多是算法自己的 bug，不是输入问题。
        return;
    }
    row->second = previous - amount;
}

void StackedItemContents::put(const ItemStack& key, int count) {
    const auto row = std::ranges::find(amounts_, key, &std::pair<ItemStack, int>::first);
    if (row == amounts_.end()) {
        amounts_.emplace_back(key, count);
        return;
    }
    row->second += count;
}

void StackedItemContents::accountStack(const ItemStack& stack) {
    accountStack(stack, static_cast<int>(itemMaximumStackSize(stack)));
}

void StackedItemContents::accountStack(const ItemStack& stack, int maxCount) {
    if (stack.empty()) return;
    const int count = std::min(maxCount, static_cast<int>(stack.count));
    put(stackedContentsKey(stack), count);
}

void StackedItemContents::accountSimpleStack(const ItemStack& stack) {
    if (isUsableForCrafting(stack)) {
        accountStack(stack);
    }
}

void StackedItemContents::accountInventory(const Inventory& inventory) {
    for (const auto& stack : inventory.slots()) {
        accountSimpleStack(stack);
    }
}

void StackedItemContents::clear() { amounts_.clear(); }

int StackedItemContents::resultUpperBound(
    std::span<const RecipeIngredient> ingredients) const {
    // `StackedContents.getResultUpperBound`（StackedContents.java:72-100），包括
    // 那个 `label31` 的提前跳出：一旦这一格的 max 已经不小于当前的 min，这一格就
    // 不可能把 min 压得更低，直接跳到下一格（**不**更新 min）。
    int min = std::numeric_limits<int>::max();
    for (const auto& ingredient : ingredients) {
        int max = 0;
        bool skipped = false;
        for (const auto& [key, itemCount] : amounts_) {
            if (itemCount > max) {
                if (acceptsItem(ingredient, key)) {
                    max = itemCount;
                }
                if (max >= min) {
                    skipped = true;
                    break;
                }
            }
        }
        if (skipped) continue;
        min = max;
        if (min == 0) break;
    }
    return min;
}

// `StackedContents.RecipePicker`（StackedContents.java:112-402）。逐字移植：位集
// 的五段布局（visitedIngredient / visitedItem / satisfied / connection /
// residual）也照抄，因为 `getConnectionIndex` 与 `getResidualIndex` 都按
// `item * ingredientCount + ingredient` 编址，换一种存法就得重新推一遍下标。
class RecipePicker final {
  public:
    RecipePicker(StackedItemContents& contents, std::span<const RecipeIngredient> ingredients)
        : contents_(contents),
          ingredients_(ingredients),
          ingredientCount_(static_cast<int>(ingredients.size())),
          items_(uniqueAvailableIngredientItems(contents, ingredients)),
          itemCount_(static_cast<int>(items_.size())),
          data_(static_cast<std::size_t>(visitedIngredientCount() + visitedItemCount() +
                                         satisfiedCount() + connectionCount() +
                                         residualCount()),
                false) {
        setInitialConnections();
    }

    // `tryPick`（:141-191）
    [[nodiscard]] bool tryPick(int capacity, const StackedItemContents::Output& output) {
        if (capacity <= 0) {
            return true;
        }
        int satisfiedIngredientCount = 0;
        while (true) {
            if (!tryAssigningNewItem(capacity)) {
                const bool isValidAssignment = satisfiedIngredientCount == ingredientCount_;
                const bool hasOutput = isValidAssignment && static_cast<bool>(output);
                clearAllVisited();
                clearSatisfied();
                for (int ingredient = 0; ingredient < ingredientCount_; ++ingredient) {
                    for (int item = 0; item < itemCount_; ++item) {
                        if (isAssigned(item, ingredient)) {
                            unassign(item, ingredient);
                            contents_.put(items_[static_cast<std::size_t>(item)], capacity);
                            if (hasOutput) {
                                output(items_[static_cast<std::size_t>(item)]);
                            }
                            break;
                        }
                    }
                }
                return isValidAssignment;
            }
            const int assignedItem = path_.front();
            contents_.take(items_[static_cast<std::size_t>(assignedItem)], capacity);
            setSatisfied(path_.back());
            ++satisfiedIngredientCount;
            for (std::size_t index = 0; index + 1U < path_.size(); ++index) {
                if (isPathIndexItem(index)) {
                    assign(path_[index], path_[index + 1U]);
                } else {
                    unassign(path_[index + 1U], path_[index]);
                }
            }
        }
    }

    // `tryPickAll`（:382-402）：在 [0, min(maxSize, 上界)+1] 上二分 capacity。
    [[nodiscard]] int tryPickAll(int maxSize, const StackedItemContents::Output& output) {
        int min = 0;
        int max = std::min(maxSize, contents_.resultUpperBound(ingredients_)) + 1;
        while (true) {
            const int mid = (min + max) / 2;
            if (tryPick(mid, {})) {
                if (max - min <= 1) {
                    if (mid > 0) {
                        static_cast<void>(tryPick(mid, output));
                    }
                    return mid;
                }
                min = mid;
            } else {
                max = mid;
            }
        }
    }

  private:
    [[nodiscard]] static std::vector<ItemStack> uniqueAvailableIngredientItems(
        const StackedItemContents& contents, std::span<const RecipeIngredient> ingredients) {
        std::vector<ItemStack> result;
        for (const auto& [key, count] : contents.amounts()) {
            if (count > 0 && anyIngredientMatches(ingredients, key)) {
                result.push_back(key);
            }
        }
        return result;
    }

    [[nodiscard]] static bool anyIngredientMatches(
        std::span<const RecipeIngredient> ingredients, const ItemStack& key) {
        return std::ranges::any_of(ingredients, [&key](const RecipeIngredient& ingredient) {
            return acceptsItem(ingredient, key);
        });
    }

    void setInitialConnections() {
        for (int ingredient = 0; ingredient < ingredientCount_; ++ingredient) {
            const auto& info = ingredients_[static_cast<std::size_t>(ingredient)];
            for (int item = 0; item < itemCount_; ++item) {
                if (acceptsItem(info, items_[static_cast<std::size_t>(item)])) {
                    setConnection(item, ingredient);
                }
            }
        }
    }

    [[nodiscard]] static bool isPathIndexItem(std::size_t index) { return (index & 1U) == 0U; }

    // `tryAssigningNewItem`（:197-210）：找到一条增广路就留在 path_ 里返回 true。
    [[nodiscard]] bool tryAssigningNewItem(int capacity) {
        clearAllVisited();
        for (int item = 0; item < itemCount_; ++item) {
            if (contents_.hasAtLeast(items_[static_cast<std::size_t>(item)], capacity) &&
                findNewItemAssignmentPath(item)) {
                return true;
            }
        }
        return false;
    }

    // `findNewItemAssignmentPath`（:212-252）
    [[nodiscard]] bool findNewItemAssignmentPath(int startingItem) {
        path_.clear();
        visitItem(startingItem);
        path_.push_back(startingItem);
        while (!path_.empty()) {
            const std::size_t pathLength = path_.size();
            if (isPathIndexItem(pathLength - 1U)) {
                const int itemToAssign = path_.back();
                for (int ingredient = 0; ingredient < ingredientCount_; ++ingredient) {
                    if (!hasVisitedIngredient(ingredient) &&
                        hasConnection(itemToAssign, ingredient) &&
                        !isAssigned(itemToAssign, ingredient)) {
                        visitIngredient(ingredient);
                        path_.push_back(ingredient);
                        break;
                    }
                }
            } else {
                const int lastAssignedIngredient = path_.back();
                if (!isSatisfied(lastAssignedIngredient)) {
                    return true;
                }
                for (int item = 0; item < itemCount_; ++item) {
                    if (!hasVisitedItem(item) && isAssigned(item, lastAssignedIngredient)) {
                        visitItem(item);
                        path_.push_back(item);
                        break;
                    }
                }
            }
            if (path_.size() == pathLength) {
                path_.pop_back();
            }
        }
        return false;
    }

    [[nodiscard]] int visitedIngredientOffset() const { return 0; }
    [[nodiscard]] int visitedIngredientCount() const { return ingredientCount_; }
    [[nodiscard]] int visitedItemOffset() const {
        return visitedIngredientOffset() + visitedIngredientCount();
    }
    [[nodiscard]] int visitedItemCount() const { return itemCount_; }
    [[nodiscard]] int satisfiedOffset() const { return visitedItemOffset() + visitedItemCount(); }
    [[nodiscard]] int satisfiedCount() const { return ingredientCount_; }
    [[nodiscard]] int connectionOffset() const { return satisfiedOffset() + satisfiedCount(); }
    [[nodiscard]] int connectionCount() const { return ingredientCount_ * itemCount_; }
    [[nodiscard]] int residualOffset() const { return connectionOffset() + connectionCount(); }
    [[nodiscard]] int residualCount() const { return ingredientCount_ * itemCount_; }

    [[nodiscard]] bool bit(int index) const { return data_[static_cast<std::size_t>(index)]; }
    void setBit(int index) { data_[static_cast<std::size_t>(index)] = true; }
    void clearBit(int index) { data_[static_cast<std::size_t>(index)] = false; }
    void clearRange(int offset, int count) {
        for (int index = 0; index < count; ++index) clearBit(offset + index);
    }

    [[nodiscard]] bool isSatisfied(int ingredient) const {
        return bit(satisfiedOffset() + ingredient);
    }
    void setSatisfied(int ingredient) { setBit(satisfiedOffset() + ingredient); }
    void clearSatisfied() { clearRange(satisfiedOffset(), satisfiedCount()); }

    [[nodiscard]] int connectionIndex(int item, int ingredient) const {
        return connectionOffset() + item * ingredientCount_ + ingredient;
    }
    void setConnection(int item, int ingredient) { setBit(connectionIndex(item, ingredient)); }
    [[nodiscard]] bool hasConnection(int item, int ingredient) const {
        return bit(connectionIndex(item, ingredient));
    }

    [[nodiscard]] int residualIndex(int item, int ingredient) const {
        return residualOffset() + item * ingredientCount_ + ingredient;
    }
    [[nodiscard]] bool isAssigned(int item, int ingredient) const {
        return bit(residualIndex(item, ingredient));
    }
    void assign(int item, int ingredient) { setBit(residualIndex(item, ingredient)); }
    void unassign(int item, int ingredient) { clearBit(residualIndex(item, ingredient)); }

    void visitIngredient(int ingredient) { setBit(visitedIngredientOffset() + ingredient); }
    [[nodiscard]] bool hasVisitedIngredient(int ingredient) const {
        return bit(visitedIngredientOffset() + ingredient);
    }
    void visitItem(int item) { setBit(visitedItemOffset() + item); }
    [[nodiscard]] bool hasVisitedItem(int item) const { return bit(visitedItemOffset() + item); }
    void clearAllVisited() {
        clearRange(visitedIngredientOffset(), visitedIngredientCount());
        clearRange(visitedItemOffset(), visitedItemCount());
    }

    StackedItemContents& contents_;
    std::span<const RecipeIngredient> ingredients_;
    int ingredientCount_;
    std::vector<ItemStack> items_;
    int itemCount_;
    std::vector<bool> data_;
    std::vector<int> path_;
};

RecipePlacement recipePlacement(const CraftingRecipe& recipe) {
    RecipePlacement placement;
    int placementIndex = 0;
    for (const auto& ingredient : recipe.ingredients) {
        if (ingredient.kind == IngredientKind::Empty) {
            placement.slotsToIngredientIndex.push_back(-1);
            continue;
        }
        placement.ingredients.push_back(ingredient);
        placement.slotsToIngredientIndex.push_back(placementIndex);
        ++placementIndex;
    }
    return placement;
}

bool StackedItemContents::canCraft(const CraftingRecipe& recipe, int amount,
                                   const Output& used) {
    const auto placement = recipePlacement(recipe);
    if (placement.impossibleToPlace()) {
        return false;
    }
    RecipePicker picker{*this, placement.ingredients};
    return picker.tryPick(amount, used);
}

int StackedItemContents::biggestCraftableStack(const CraftingRecipe& recipe, int maxSize,
                                               const Output& used) {
    const auto placement = recipePlacement(recipe);
    if (placement.impossibleToPlace()) {
        return 0;
    }
    RecipePicker picker{*this, placement.ingredients};
    return picker.tryPickAll(maxSize, used);
}

bool canCraft(const Inventory& inventory, const CraftingRecipe& recipe, int amount) {
    StackedItemContents contents;
    contents.accountInventory(inventory);
    return contents.canCraft(recipe, amount);
}

int biggestCraftableStack(const Inventory& inventory, const CraftingRecipe& recipe) {
    StackedItemContents contents;
    contents.accountInventory(inventory);
    return contents.biggestCraftableStack(recipe);
}

} // namespace mc::gameplay

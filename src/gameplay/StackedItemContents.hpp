#pragma once

// 26.1 的 `world/entity/player/StackedItemContents.java` +
// `world/entity/player/StackedContents.java`：把一堆物品摊成「每种物品各有几个」
// 的计数表，再回答两个问题——
//   * `canCraft(配方, n)`：这堆材料够不够把这条配方做 n 次；
//   * `biggestCraftableStack(配方, 上限)`：最多能做几次。
//
// ★ 这**不是**「每种材料需要几个、拿背包数量整除、取最小值」。26.1 的
//   `StackedContents.RecipePicker`（StackedContents.java:112-402）是一个**二分图
//   最大匹配**：材料格（ingredient）一侧、物品种类（item）一侧，一条边表示这格收
//   这种物品；`tryPick(capacity)` 找增广路，看能不能给每个材料格都配上一种「至少
//   还剩 capacity 个」的物品。之所以必须是匹配而不是除法，是因为一格可以收多种
//   物品（`#planks` 那样的标签），两格抢同一种物品时谁让给谁会改变答案。
//   `tryPickAll`（:382-402）再在 [0, 上限] 上**二分**这个 capacity。
//
//   `getResultUpperBound`（:72-100）给二分一个上界：对每个材料格取「它收得下的
//   物品里数量最多的那个的数量」，再对所有格取最小值。它是上界不是答案——上界只
//   看单格，匹配才管两格抢同一种物品。
//
// 计数表的键：vanilla 键在 `Holder<Item>` 上（物品身份，不含组件），因为
// `Inventory.isUsableForCrafting`（Inventory.java:144-146）已经把「有损伤 /
// 有附魔 / 有自定义名」的堆挡在外面了。本作的一堆物品身份是 (block, item) 两个
// 字段，见 `stackedContentsKey`。

#include "gameplay/CraftingSystem.hpp"
#include "gameplay/Inventory.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <vector>

namespace mc::gameplay {

// `Inventory.isUsableForCrafting`（Inventory.java:144-146）：`!isDamaged() &&
// !isEnchanted() && !has(CUSTOM_NAME)`。一把用旧的镐、一件附了魔的胸甲、一个改过
// 名的方块都不会被配方书当成原料——它们进了网格就不再是「同一个物品」，一键填充
// 也就不该把它们搬进去。
[[nodiscard]] constexpr bool isUsableForCrafting(const ItemStack& stack) {
    return stack.damage == 0U && stack.enchantmentCount == 0U && stack.customNameId == 0U;
}

// 计数表的键：同一种物品的两种写法（方块堆的 item 指针可能是空哨兵，也可能是它
// 自己的 BlockItem）必须折成同一个键，否则「8 个圆石」会被记成两种各 4 个。
[[nodiscard]] ItemStack stackedContentsKey(const ItemStack& stack);

class StackedItemContents final {
  public:
    // `StackedItemContents.accountStack`（:20-29）：数量按 `maxCount` 截断，默认
    // 是这堆的堆叠上限。
    void accountStack(const ItemStack& stack);
    void accountStack(const ItemStack& stack, int maxCount);
    // `accountSimpleStack`（:14-18）：只收 isUsableForCrafting 的堆。
    void accountSimpleStack(const ItemStack& stack);
    // `Inventory.fillStackedContents`（Inventory.java:514-518）：整个背包。
    void accountInventory(const Inventory& inventory);
    void clear();

    // 每种物品各有几个，调试与测试用。
    [[nodiscard]] std::span<const std::pair<ItemStack, int>> amounts() const {
        return amounts_;
    }

    // `canCraft`（StackedItemContents.java:31-50）。`used` 非空时，成功的那次会
    // 按**材料格顺序**回调每格实际用掉的是哪种物品——`ServerPlaceRecipe` 的
    // `itemsUsedPerIngredient` 就是这么攒出来的。
    using Output = std::function<void(const ItemStack&)>;
    [[nodiscard]] bool canCraft(const CraftingRecipe& recipe, int amount = 1,
                                const Output& used = {});

    // `getBiggestCraftableStack`（:52-58）：最多能做几次，上限 `maxSize`。
    [[nodiscard]] int biggestCraftableStack(
        const CraftingRecipe& recipe,
        int maxSize = std::numeric_limits<int>::max(),
        const Output& used = {});

    // `getResultUpperBound`（StackedContents.java:72-100）。二分的上界，单独暴露
    // 是因为它有自己的断言（它是上界，不是答案）。
    [[nodiscard]] int resultUpperBound(std::span<const RecipeIngredient> ingredients) const;

  private:
    friend class RecipePicker;
    [[nodiscard]] bool hasAtLeast(const ItemStack& key, int count) const;
    void take(const ItemStack& key, int amount);
    void put(const ItemStack& key, int count);

    std::vector<std::pair<ItemStack, int>> amounts_;
};

// `PlacementInfo.createFromOptionals`（PlacementInfo.java:24-45）：配方的材料格
// 列表里，非空的那些按原顺序排成 `ingredients`，而每个网格位置记下它用的是第几
// 个（空格记 -1）。一键填充两边都要。
struct RecipePlacement final {
    std::vector<RecipeIngredient> ingredients;
    // 长度 = 配方自己的 width*height（无序配方是材料个数）。
    std::vector<int> slotsToIngredientIndex;
    [[nodiscard]] bool impossibleToPlace() const { return slotsToIngredientIndex.empty(); }
};

[[nodiscard]] RecipePlacement recipePlacement(const CraftingRecipe& recipe);

// 便利入口：这份背包能不能把这条配方做 `amount` 次 / 最多做几次。
[[nodiscard]] bool canCraft(const Inventory& inventory, const CraftingRecipe& recipe,
                            int amount = 1);
[[nodiscard]] int biggestCraftableStack(const Inventory& inventory,
                                        const CraftingRecipe& recipe);

} // namespace mc::gameplay

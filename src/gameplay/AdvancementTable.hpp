#pragma once

// ADV-1：进程级的成就表 —— 生成出来的解锁底座 + 数据包覆盖，与 BlockTags /
// RecipeTable / LootTable 同一个两层形状。
//
// ★★ 底座是**生成**的，不是第二份手抄数据。
//
// vanilla 的 `data/minecraft/advancement/recipes/**.json`（1492 个）是**派生
// 产物**：解锁材料写在配方构建器的 `unlockedBy(...)` 上，`RecipeProvider` 在
// 数据生成期把它生成成 JSON。本作照同一条管线做，只是把「数据生成期」换成
// 「加载期」：解锁材料写在配方那一行（BakedCraftingRecipe::unlockedBy），
// 这里在 load 时按配方表逐条生成成就。逐字段与 vanilla 的文件同形：
//
//   id           rebedrock:recipes/<配方短名>
//   parent       rebedrock:recipes/root
//   criteria     has_the_recipe = recipe_unlocked(自己)
//                has_<材料>     = inventory_changed(unlockedBy)
//   requirements [["has_the_recipe", "has_<材料>"]]   一个 OR 组
//   rewards      {"recipes": ["<自己>"]}
//
// 所以「配方表与解锁表两份表述会不同步」这个风险根本不存在：只有一份表述。
// 漏填 unlockedBy 在**编译期**停下（RecipeTable.cpp 的 static_assert）。
//
// 与真 vanilla 数据包共存：vanilla 那 1492 个文件的 id 是
// `minecraft:recipes/<分类>/<名字>`，归一化后是
// `rebedrock:recipes/<分类>/<名字>`，与本作生成的 `rebedrock:recipes/<名字>`
// **不撞**（我们没有分类那一层目录）。两条成就都奖励同一条配方，但
// `RecipeBook::addRecipes` 的 known 判重让第二条什么也不发——与 vanilla 自己
// 「同一条配方被两个成就奖励」的行为一致。

#include "assets/ResourceProvider.hpp"
#include "data/AdvancementFile.hpp"
#include "gameplay/CraftingSystem.hpp"
#include "gameplay/RecipeTable.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::gameplay {

// 一条 criterion 在表里的位置。触发器分发按这个索引走，不必每次扫全表。
struct AdvancementCriterionRef final {
    std::size_t advancement = 0U;
    std::size_t criterion = 0U;
    [[nodiscard]] bool operator==(const AdvancementCriterionRef&) const = default;
};

// 解析好的 inventory_changed 谓词：一个运行期材料 + 「认不认得」。
struct ResolvedItemPredicate final {
    RecipeIngredient item;
    bool understood = true;
};

// 一条成就的运行期形态：数据层的 def 加上解析好的谓词。
struct ResolvedAdvancement final {
    data::AdvancementDef def;
    // 与 def.criteria 一一对应（下标相同）。只有 InventoryChanged 的那条用得上。
    std::vector<std::vector<ResolvedItemPredicate>> criterionItems;
};

class AdvancementTable final {
  public:
    // 只生成底座：按配方表逐条生成解锁成就，不解析任何文件。无数据包的安装、
    // 无头调用方拿到的就是这一份。
    void loadBuiltinDefaults(const RecipeTable& recipes);

    // 底座，然后叠数据包：`data/<ns>/advancement/**.json`。同 id 的文件替换底座
    // 那一条，新 id 追加。一个读不出来的文件被跳过，不会让整包挂掉。
    void load(const assets::ResourceProvider& resources, const RecipeTable& recipes);

    [[nodiscard]] std::span<const ResolvedAdvancement> all() const { return advancements_; }
    [[nodiscard]] const ResolvedAdvancement* find(std::string_view identifier) const;

    // 触发器索引：分发时只走相关的那几条 criterion。
    [[nodiscard]] std::span<const AdvancementCriterionRef> inventoryChangedCriteria() const {
        return inventoryChanged_;
    }
    [[nodiscard]] std::span<const AdvancementCriterionRef> tickCriteria() const {
        return tick_;
    }
    // `recipe_unlocked` 按配方 id 索引（id 已归一化）。
    [[nodiscard]] std::span<const AdvancementCriterionRef> recipeUnlockedCriteria(
        std::string_view recipeIdentifier) const;

    void clear();

  private:
    void applyOverlay(const assets::ResourceProvider& resources);
    // 把一条 def 并进表里（同 id 替换、新 id 追加），并解析它的谓词。
    void merge(data::AdvancementDef def);
    void rebuildIndex();

    std::vector<ResolvedAdvancement> advancements_;
    std::unordered_map<std::string, std::size_t> byIdentifier_;
    std::vector<AdvancementCriterionRef> inventoryChanged_;
    std::vector<AdvancementCriterionRef> tick_;
    std::unordered_map<std::string, std::vector<AdvancementCriterionRef>> recipeUnlocked_;
    static const std::vector<AdvancementCriterionRef> kNoCriteria;
};

// 从一条配方生成它的解锁成就，逐字段与 vanilla 的
// `advancement/recipes/**/<名>.json` 同形。公开出来是因为测试要拿它对着 vanilla
// 的真文件比。`unlockedBy` 为 Empty（只可能来自数据包配方）时返回 false。
[[nodiscard]] bool advancementForRecipeUnlock(std::string_view recipeIdentifier,
                                              const data::IngredientDef& unlockedBy,
                                              data::AdvancementDef& out);

// 生成出来的解锁成就共同的父节点 id，对着 vanilla 的 `minecraft:recipes/root`。
inline constexpr std::string_view kRecipeAdvancementRoot = "rebedrock:recipes/root";

// 进程级的表。首次取用时按配方底座生成，所以没有任何接线也有解锁链；
// DataPackStack 在包栈起来之后调 load 叠覆盖，与 recipeTable() 同一条路。
[[nodiscard]] AdvancementTable& advancementTable();

} // namespace mc::gameplay

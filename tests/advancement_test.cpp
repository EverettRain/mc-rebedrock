// ADV-1：最小成就链（只覆盖配方解锁）。
//
// ★ 这里的每个断言钉的都是 26.1 的源码或真数据包里的值，不是本作跑出来的值：
//
//   * `requirements` 的 AND/OR 语义 —— `advancements/AdvancementRequirements.java`
//     的 `test`（:40-52）：每一组必须有至少一个 criterion 满足（组内 OR），所有组
//     都满足才算完成（组间 AND）；空列表直接 return false（:41-43）。
//   * `requirements` 字段缺失时的默认 —— `Advancement.java:45,49` 的
//     `AdvancementRequirements.allOf(criteria.keySet())`（每个 criterion 自成一
//     组，即全部 AND）。★ 不是 anyOf，这是最容易想反的一处。
//   * `inventory_changed` 的三分支 ——
//     `criterion/InventoryChangeTrigger.java:83-108`：没有 items 恒真；**恰好
//     一个**谓词时只测「这次变的那一堆」；多于一个时每个谓词都要能被背包里某一
//     槽满足。
//   * 回路断点 —— `stats/ServerRecipeBook.java:61-80` 的
//     `if (!this.known.contains(id) …)`：只有真的新解锁才回打 RECIPE_UNLOCKED。
//   * 生成出来的解锁成就的形状 —— 逐字段对着真 26.1 数据包里那 1492 个
//     `data/minecraft/advancement/recipes/**.json`。本文件里 kVanillaStickJson 与
//     kVanillaChestJson 是从那两个文件**原样抄**下来的，不是本作写出来的。
//   * unlockedBy 的取值 —— 逐条抄 vanilla 那个文件里 `inventory_changed` 的
//     `items`（见 kVanillaUnlockMaterials 那张对照表）。

#include "data/AdvancementFile.hpp"
#include "gameplay/AdvancementTable.hpp"
#include "gameplay/ContentRegistry.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "gameplay/PlayerAdvancements.hpp"
#include "gameplay/RecipeBook.hpp"
#include "gameplay/RecipeTable.hpp"
#include "persistence/SaveRepository.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace mc::gameplay;
using mc::data::AdvancementDef;
using mc::data::AdvancementTriggerKind;
using mc::world::Block;

[[nodiscard]] ItemStack blockStack(Block block, std::uint8_t count) {
    return ItemStack{block, count, blockItemFor(block)};
}
[[nodiscard]] ItemStack itemStack(const Item* item, std::uint8_t count) {
    return ItemStack{Block::Air, count, item};
}

[[nodiscard]] const AdvancementDef& advancementDef(const AdvancementTable& table,
                                                   std::string_view identifier) {
    const ResolvedAdvancement* found = table.find(identifier);
    assert(found != nullptr);
    return found->def;
}

[[nodiscard]] const mc::data::AdvancementCriterionDef& criterionNamed(
    const AdvancementDef& advancement, std::string_view name) {
    const auto found = std::ranges::find(advancement.criteria, name,
                                         &mc::data::AdvancementCriterionDef::name);
    assert(found != advancement.criteria.end());
    return *found;
}

// --- 1. requirements 的 AND/OR 语义 ---------------------------------------

void testRequirementsAreAndOfOrs() {
    using Requirements = std::vector<std::vector<std::string>>;
    const auto met = [](const Requirements& requirements,
                        const std::vector<std::string>& satisfied) {
        return mc::data::advancementRequirementsMet(
            requirements, [&satisfied](std::string_view criterion) {
                return std::ranges::find(satisfied, criterion) != satisfied.end();
            });
    };

    // 组内 OR：一个组里任一 criterion 满足，这一组就满足。
    const Requirements oneGroup{{"a", "b"}};
    assert(met(oneGroup, {"a"}));
    assert(met(oneGroup, {"b"}));
    assert(met(oneGroup, {"a", "b"}));
    assert(!met(oneGroup, {}));
    assert(!met(oneGroup, {"c"}));

    // 组间 AND：**每**一组都要满足。
    const Requirements twoGroups{{"a", "b"}, {"c"}};
    assert(!met(twoGroups, {"a"}));      // 第二组没满足
    assert(!met(twoGroups, {"c"}));      // 第一组没满足
    assert(met(twoGroups, {"b", "c"}));

    // `AdvancementRequirements.test`（:41-43）：空列表 -> false，不是 true。
    assert(!met(Requirements{}, {"a"}));
    // 空的一组也永远满足不了（组内没有任何 criterion 可满足）。
    assert(!met(Requirements{{}}, {"a"}));
}

// --- 2. codec：JE 形状进来 ------------------------------------------------

// `<26.1>/data/minecraft/advancement/recipes/misc/stick.json` 原样抄。
constexpr std::string_view kVanillaStickJson = R"({
  "parent": "minecraft:recipes/root",
  "criteria": {
    "has_planks": {
      "conditions": { "items": [ { "items": "#minecraft:planks" } ] },
      "trigger": "minecraft:inventory_changed"
    },
    "has_the_recipe": {
      "conditions": { "recipe": "minecraft:stick" },
      "trigger": "minecraft:recipe_unlocked"
    }
  },
  "requirements": [ [ "has_the_recipe", "has_planks" ] ],
  "rewards": { "recipes": [ "minecraft:stick" ] }
})";

// `<26.1>/data/minecraft/advancement/recipes/decorations/chest.json` 原样抄。
// 它用的是 `slots` 条件——本作不实现的那一个键。
constexpr std::string_view kVanillaChestJson = R"({
  "parent": "minecraft:recipes/root",
  "criteria": {
    "has_lots_of_items": {
      "conditions": { "slots": { "occupied": { "min": 10 } } },
      "trigger": "minecraft:inventory_changed"
    },
    "has_the_recipe": {
      "conditions": { "recipe": "minecraft:chest" },
      "trigger": "minecraft:recipe_unlocked"
    }
  },
  "requirements": [ [ "has_the_recipe", "has_lots_of_items" ] ],
  "rewards": { "recipes": [ "minecraft:chest" ] }
})";

void testCodecReadsVanillaShape() {
    AdvancementDef def;
    assert(mc::data::Codec<AdvancementDef>::read(mc::core::Json::parse(kVanillaStickJson), def));
    assert(def.parent == "minecraft:recipes/root");
    // criteria 按名字升序（JE 那边是 map，顺序不该泄漏出来）。
    assert(def.criteria.size() == 2U);
    assert(def.criteria[0].name == "has_planks");
    assert(def.criteria[1].name == "has_the_recipe");
    assert(def.criteria[0].trigger == AdvancementTriggerKind::InventoryChanged);
    assert(def.criteria[0].items.size() == 1U);
    // `#minecraft:planks` -> 本作的「任意木板」组。
    assert(def.criteria[0].items[0].understood);
    assert(def.criteria[0].items[0].item.kind == mc::data::IngredientDefKind::Planks);
    assert(def.criteria[1].trigger == AdvancementTriggerKind::RecipeUnlocked);
    assert(def.criteria[1].recipe == "minecraft:stick");
    assert(def.requirements.size() == 1U);
    assert((def.requirements[0] == std::vector<std::string>{"has_the_recipe", "has_planks"}));
    assert((def.rewardRecipes == std::vector<std::string>{"minecraft:stick"}));

    // `slots` 是本作不实现的条件键 -> 那条 criterion 永不满足，但文件照常读进来
    // （★ 不是「忽略这个键、剩下的照判」——忽略一个约束等于把它当恒真，那会让这条
    // 成就一进游戏就完成）。
    AdvancementDef chest;
    assert(mc::data::Codec<AdvancementDef>::read(mc::core::Json::parse(kVanillaChestJson), chest));
    assert(!criterionNamed(chest, "has_lots_of_items").understood);
    assert(criterionNamed(chest, "has_the_recipe").understood);
}

void testCodecToleratesUnknownTriggers() {
    // 本作没实现的触发器（vanilla 有 30 来种）：那条 criterion 永不满足，
    // 但**不许**让整个文件读失败。
    constexpr std::string_view kJson = R"({
      "criteria": {
        "killed": {
          "trigger": "minecraft:player_killed_entity",
          "conditions": { "entity": { "type": "minecraft:zombie" } }
        }
      },
      "requirements": [ [ "killed" ] ]
    })";
    AdvancementDef def;
    assert(mc::data::Codec<AdvancementDef>::read(mc::core::Json::parse(kJson), def));
    assert(def.criteria.size() == 1U);
    assert(def.criteria[0].trigger == AdvancementTriggerKind::Unsupported);

    // criteria 为空是 vanilla 自己拒绝的（`Advancement.java:37-38`）。
    AdvancementDef empty;
    assert(!mc::data::Codec<AdvancementDef>::read(
        mc::core::Json::parse(R"({"criteria":{}})"), empty));
}

void testMissingRequirementsMeansAllOf() {
    // `Advancement.java:45,49`：`requirements` 字段缺失 -> allOf（每个 criterion
    // 自成一组，全部 AND）。★ 不是 anyOf。
    constexpr std::string_view kJson = R"({
      "criteria": {
        "a": { "trigger": "minecraft:tick" },
        "b": { "trigger": "minecraft:tick" }
      }
    })";
    AdvancementDef def;
    assert(mc::data::Codec<AdvancementDef>::read(mc::core::Json::parse(kJson), def));
    assert(def.requirements.size() == 2U);
    assert((def.requirements[0] == std::vector<std::string>{"a"}));
    assert((def.requirements[1] == std::vector<std::string>{"b"}));
}

// --- 3. 生成出来的底座与 vanilla 的文件同形 -------------------------------

void testGeneratedFloorMatchesVanillaShape() {
    AdvancementTable table;
    table.loadBuiltinDefaults(recipeTable());

    // vanilla `misc/stick.json` 对照：本作的配方 id 是 `rebedrock:sticks`
    // （vanilla 叫 `stick`），成就 id 因此是 `rebedrock:recipes/sticks`。
    const AdvancementDef& sticks = advancementDef(table, "rebedrock:recipes/sticks");
    assert(sticks.parent == kRecipeAdvancementRoot);
    assert(sticks.criteria.size() == 2U);
    // 一个 recipe_unlocked（名字逐字照抄 vanilla 的 `has_the_recipe`）+
    // 一个 inventory_changed。
    const auto& theRecipe = criterionNamed(sticks, "has_the_recipe");
    assert(theRecipe.trigger == AdvancementTriggerKind::RecipeUnlocked);
    assert(theRecipe.recipe == "rebedrock:sticks");
    const auto& material = criterionNamed(sticks, "has_planks");
    assert(material.trigger == AdvancementTriggerKind::InventoryChanged);
    assert(material.items.size() == 1U);
    // vanilla 这条用的就是 `#minecraft:planks`。
    assert(material.items[0].item.kind == mc::data::IngredientDefKind::Planks);
    // requirements：恰好一个 OR 组，`has_the_recipe` 在前 —— 与 vanilla 那 1492
    // 个文件**全部**一致的形状。
    assert(sticks.requirements.size() == 1U);
    assert((sticks.requirements[0] == std::vector<std::string>{"has_the_recipe", "has_planks"}));
    assert((sticks.rewardRecipes == std::vector<std::string>{"rebedrock:sticks"}));

    // 每一条内置配方都有一条解锁成就，而且形状全一样（vanilla 那 1492 个文件的
    // requirements 也**全部**是「恰好一个 OR 组」）。
    std::size_t generated = 0;
    for (const auto& advancement : table.all()) {
        if (advancement.def.identifier == kRecipeAdvancementRoot) continue;
        ++generated;
        assert(advancement.def.criteria.size() == 2U);
        assert(advancement.def.requirements.size() == 1U);
        assert(advancement.def.requirements[0].size() == 2U);
        assert(advancement.def.requirements[0][0] == "has_the_recipe");
        assert(advancement.def.rewardRecipes.size() == 1U);
        // 奖励的那条配方**确实存在**于配方表里——生成的 id 拼错了会在这里变红。
        const auto& reward = advancement.def.rewardRecipes[0];
        const bool known =
            std::ranges::any_of(recipeTable().crafting(),
                                [&reward](const CraftingRecipe& r) { return r.identifier == reward; }) ||
            std::ranges::any_of(recipeTable().furnace(),
                                [&reward](const FurnaceRecipe& r) { return r.identifier == reward; });
        assert(known);
    }
    assert(generated == recipeTable().crafting().size() + recipeTable().furnace().size());
}

// --- 4. unlockedBy 的取值逐条对着 vanilla ---------------------------------

// 从 `<26.1>/data/minecraft/advancement/recipes/**/<名>.json` 里那个
// `inventory_changed` 的 `items` 抄下来的对照表。标签在本作收窄到我们有的那一个
// （`#stone_tool_materials` = {cobblestone, blackstone, cobbled_deepslate}，本作
// 只有 cobblestone；`#iron_tool_materials` = {iron_ingot} 一对一）。
struct UnlockCase final {
    std::string_view recipe;   // 本作的配方 id
    std::string_view material; // 期望的解锁材料 id（Planks 组写 ""）
    IngredientKind kind;
};

constexpr std::array<UnlockCase, 14U> kVanillaUnlockMaterials{{
    // building_blocks/oak_planks.json: "#minecraft:oak_logs"
    {"rebedrock:oak_planks", "rebedrock:oak_log", IngredientKind::Block},
    // misc/stick.json: "#minecraft:planks"
    {"rebedrock:sticks", "", IngredientKind::AnyPlanks},
    // food/bread.json: "minecraft:wheat"
    {"rebedrock:bread", "rebedrock:wheat", IngredientKind::Item},
    // ★ tools/wooden_pickaxe.json: "minecraft:stick" —— 不是「第一个配料」
    //   （第一个配料是木板），vanilla 挑的是**区别性材料**。
    {"rebedrock:wooden_pickaxe", "rebedrock:stick", IngredientKind::Item},
    // ★ tools/stone_pickaxe.json: "#minecraft:stone_tool_materials" —— 换成
    //   圆石而不是木棍，同样说明规则不是「取第一个配料」。
    {"rebedrock:stone_pickaxe", "rebedrock:cobblestone", IngredientKind::Block},
    // tools/iron_pickaxe.json: "#minecraft:iron_tool_materials"
    {"rebedrock:iron_pickaxe", "rebedrock:iron_ingot", IngredientKind::Item},
    // tools/diamond_pickaxe.json: "#minecraft:diamond_tool_materials"
    {"rebedrock:diamond_pickaxe", "rebedrock:diamond", IngredientKind::Item},
    // ★ decorations/torch.json: "minecraft:stone_pickaxe" —— 石镐**根本不是**
    //   火把的配料（配料是煤+木棍）。任何「从配料里推」的规则都会在这里错。
    {"rebedrock:torch", "rebedrock:stone_pickaxe", IngredientKind::Item},
    // building_blocks/furnace.json: "#minecraft:stone_crafting_materials"
    {"rebedrock:furnace", "rebedrock:cobblestone", IngredientKind::Block},
    // ★ misc/anvil.json: "minecraft:iron_block"
    {"rebedrock:anvil", "rebedrock:iron_block", IngredientKind::Block},
    // decorations/white_bed.json: "minecraft:white_wool"
    {"rebedrock:white_bed", "rebedrock:white_wool", IngredientKind::Block},
    // misc/iron_ingot_from_smelting_iron_ore.json: "minecraft:iron_ore"
    {"rebedrock:iron_ingot_from_smelting", "rebedrock:iron_ore", IngredientKind::Block},
    // building_blocks/stone.json: "minecraft:cobblestone"
    {"rebedrock:stone_from_smelting", "rebedrock:cobblestone", IngredientKind::Block},
    // food/cooked_porkchop.json: "minecraft:porkchop"
    {"rebedrock:cooked_porkchop", "rebedrock:porkchop", IngredientKind::Item},
}};

void testUnlockMaterialsMatchVanilla() {
    const auto unlockOf = [](std::string_view identifier) -> RecipeUnlock {
        for (const auto& recipe : recipeTable().crafting()) {
            if (recipe.identifier == identifier) return recipe.unlockedBy;
        }
        for (const auto& recipe : recipeTable().furnace()) {
            if (recipe.identifier == identifier) return recipe.unlockedBy;
        }
        assert(false && "配方不存在");
        return {};
    };
    for (const auto& expected : kVanillaUnlockMaterials) {
        const RecipeUnlock unlock = unlockOf(expected.recipe);
        assert(unlock.kind == expected.kind);
        assert(unlock.identifier == expected.material);
    }
    // 全表：一条都不许漏填（RecipeTable.cpp 里另有编译期的 static_assert，这里
    // 是它在运行期的同位断言——两者都在，是因为 static_assert 只看烘焙表，这一条
    // 还盖住了「解析之后是不是真的解出来了」）。
    for (const auto& recipe : recipeTable().crafting()) {
        assert(recipe.unlockedBy.kind != IngredientKind::Empty);
    }
    for (const auto& recipe : recipeTable().furnace()) {
        assert(recipe.unlockedBy.kind != IngredientKind::Empty);
    }
}

// --- 5. 三个触发器 --------------------------------------------------------

class MemoryProvider final : public mc::assets::ResourceProvider {
  public:
    void add(std::string path, std::string body) {
        const mc::assets::ResourceLocation location{"minecraft", std::move(path),
                                                    mc::assets::PackType::ServerData};
        files_[location.toString()] = std::move(body);
    }
    [[nodiscard]] std::filesystem::path locate(const mc::assets::ResourceLocation&) const override {
        return {};
    }
    [[nodiscard]] bool exists(const mc::assets::ResourceLocation& location) const override {
        return files_.contains(location.toString());
    }
    [[nodiscard]] std::filesystem::path resourceRoot() const override { return {}; }
    [[nodiscard]] std::vector<std::byte> readBytes(
        const mc::assets::ResourceLocation& location) const override {
        const auto slot = files_.find(location.toString());
        if (slot == files_.end()) return {};
        std::vector<std::byte> bytes(slot->second.size());
        for (std::size_t index = 0; index < slot->second.size(); ++index) {
            bytes[index] = static_cast<std::byte>(slot->second[index]);
        }
        return bytes;
    }
    [[nodiscard]] std::vector<mc::assets::ResourceLocation> list(
        std::string_view space, std::string_view pathPrefix,
        mc::assets::PackType = mc::assets::PackType::ClientResources) const override {
        std::vector<mc::assets::ResourceLocation> found;
        for (const auto& [key, body] : files_) {
            static_cast<void>(body);
            auto location =
                mc::assets::ResourceLocation::parse(key, mc::assets::PackType::ServerData);
            if (location.space == space && location.path.size() >= pathPrefix.size() &&
                std::string_view{location.path}.substr(0, pathPrefix.size()) == pathPrefix) {
                found.push_back(std::move(location));
            }
        }
        std::ranges::sort(found, {}, &mc::assets::ResourceLocation::path);
        return found;
    }

  private:
    std::map<std::string, std::string> files_;
};


struct Fixture final {
    AdvancementTable table;
    PlayerAdvancements progress;
    RecipeBook book;
    Inventory inventory;
    Fixture() { table.loadBuiltinDefaults(recipeTable()); }
};

void testInventoryChangedUnlocksItsRecipe() {
    Fixture fixture;
    assert(!fixture.book.contains("rebedrock:oak_planks"));
    // 拿到橡木原木 -> oak_planks 的 `has_oak_log` 满足 -> 那一组（OR）满足 ->
    // 整条成就完成 -> rewards.recipes 发出 oak_planks。
    const int unlocked = fixture.progress.onInventoryChanged(
        fixture.table, fixture.inventory, blockStack(Block::OakLog, 1U), fixture.book);
    assert(unlocked >= 1);
    assert(fixture.book.contains("rebedrock:oak_planks"));
    assert(fixture.book.highlighted("rebedrock:oak_planks"));
    // 木板配方的解锁材料是原木不是木板，所以拿到原木不会顺带解锁「用木板做的」。
    assert(!fixture.book.contains("rebedrock:sticks"));
    assert(!fixture.book.contains("rebedrock:crafting_table"));

    // 再打一次同样的事件：一条都不新增（criterion 已记，成就已完成）。
    assert(fixture.progress.onInventoryChanged(fixture.table, fixture.inventory,
                                               blockStack(Block::OakLog, 1U), fixture.book) == 0);

    // 拿到木板才解锁用木板的那些（`#planks` 组）。
    static_cast<void>(fixture.progress.onInventoryChanged(
        fixture.table, fixture.inventory, blockStack(Block::OakPlanks, 1U), fixture.book));
    assert(fixture.book.contains("rebedrock:sticks"));
    assert(fixture.book.contains("rebedrock:crafting_table"));
}

void testInventoryChangedTestsOnlyTheChangedStackWhenSinglePredicate() {
    // `InventoryChangeTrigger.matches`（:105-107）：**恰好一个**谓词时只测这次变
    // 的那一堆，**不看**背包里有什么。所以背包里塞满原木、而变的那一堆是别的东西
    // 时，oak_planks 不该解锁。
    Fixture fixture;
    ItemStack logs = blockStack(Block::OakLog, 16U);
    assert(fixture.inventory.add(logs));
    const int unlocked = fixture.progress.onInventoryChanged(
        fixture.table, fixture.inventory, itemStack(&items::Coal, 1U), fixture.book);
    assert(!fixture.book.contains("rebedrock:oak_planks"));
    // 煤是火把的配料，但火把的解锁材料是**石镐**（vanilla 的选择），所以这一下
    // 也不该解锁火把。
    assert(!fixture.book.contains("rebedrock:torch"));
    static_cast<void>(unlocked);
}

void testTickTriggerFiresUnconditionally() {
    // `PlayerTrigger.trigger`（:20-22）：tick 实例无条件满足。本作生成的底座里
    // 没有 tick criterion（vanilla 只有 crafting_table 那条用 tick，本作的底座
    // 统一成 inventory_changed），所以用一份数据包成就来测——形状照抄 vanilla 的
    // `decorations/crafting_table.json` 的 `unlock_right_away`。
    MemoryProvider pack;
    pack.add("advancement/recipes/right_away.json",
             R"({"criteria":{"unlock_right_away":{"trigger":"minecraft:tick"},
                 "has_the_recipe":{"trigger":"minecraft:recipe_unlocked",
                                   "conditions":{"recipe":"minecraft:crafting_table"}}},
                 "requirements":[["has_the_recipe","unlock_right_away"]],
                 "rewards":{"recipes":["minecraft:crafting_table"]}})");
    AdvancementTable table;
    table.load(pack, recipeTable());
    PlayerAdvancements progress;
    RecipeBook book;
    assert(!book.contains("rebedrock:crafting_table"));
    // 一个 tick 就够：criterion 无条件满足，那一组（OR）随之满足，成就完成发奖。
    assert(progress.onTick(table, book) >= 1);
    assert(book.contains("rebedrock:crafting_table"));
    assert(progress.done(table, "rebedrock:recipes/right_away"));
    // 第二个 tick 一条都不新增。
    assert(progress.onTick(table, book) == 0);
}

void testInventoryChangedScansInventoryWhenSeveralPredicates() {
    // `InventoryChangeTrigger.matches`（:91-104）：**多于一个**谓词时改成扫整个
    // 背包，每个谓词都要被某一槽满足；这时 changedStack 是什么不重要。
    MemoryProvider pack;
    pack.add("advancement/test/two_items.json",
             R"({"criteria":{"has_both":{"trigger":"minecraft:inventory_changed",
                 "conditions":{"items":[{"items":"minecraft:coal"},
                                        {"items":"minecraft:wheat"}]}}},
                 "requirements":[["has_both"]],
                 "rewards":{"recipes":["minecraft:bread"]}})");
    AdvancementTable table;
    table.load(pack, recipeTable());
    PlayerAdvancements progress;
    RecipeBook book;
    Inventory inventory;

    // 只有煤：第二个谓词没人满足 -> 不完成。★ 单谓词分支会在这里错判为完成，
    // 所以这条断言分得出两个分支。
    ItemStack coal = itemStack(&items::Coal, 1U);
    assert(inventory.add(coal));
    static_cast<void>(progress.onInventoryChanged(table, inventory, coal, book));
    assert(!progress.done(table, "rebedrock:test/two_items"));

    // 加上小麦：两个谓词都被背包里的某一槽满足 -> 完成，即使这次「变的那一堆」
    // 是煤（不是刚加进去的小麦）。
    ItemStack wheat = itemStack(&items::Wheat, 1U);
    assert(inventory.add(wheat));
    static_cast<void>(progress.onInventoryChanged(table, inventory, coal, book));
    assert(progress.done(table, "rebedrock:test/two_items"));
    assert(book.contains("rebedrock:bread"));
}

void testRecipeUnlockedTriggerAndTheCycleBreak() {
    // ★★ 回路：`has_the_recipe` 是 recipe_unlocked(自己)，而这条成就的奖励又是
    // 自己那条配方。给了配方 -> 完成成就 -> 又发同一个配方。
    // `ServerRecipeBook.addRecipes`（:61-80）的 `if (!known.contains(id))` 是断
    // 点：第二遍发不出去，也就不再触发。这条测试跑得完（不栈溢出）本身就是断言。
    Fixture fixture;
    // 先用别的路子把配方塞进配方书（相当于 `/recipe give`），再打触发器。
    assert(fixture.book.addRecipe("rebedrock:oak_planks") == 1);
    const int unlocked = fixture.progress.onRecipeUnlocked(
        fixture.table, "rebedrock:oak_planks", fixture.book);
    // 那条成就现在完成了，但它的奖励（同一条配方）一条都发不出去。
    assert(unlocked == 0);
    assert(fixture.progress.completed("rebedrock:recipes/oak_planks", "has_the_recipe"));
    assert(fixture.progress.done(fixture.table, "rebedrock:recipes/oak_planks"));
    assert(fixture.book.known().size() == 1U);

    // 反向：从 inventory_changed 那一侧完成，奖励发出配方，配方又触发
    // recipe_unlocked 完成同一条成就的另一个 criterion —— 也必须收敛。
    Fixture second;
    static_cast<void>(second.progress.onInventoryChanged(
        second.table, second.inventory, blockStack(Block::OakLog, 1U), second.book));
    assert(second.book.contains("rebedrock:oak_planks"));
    assert(second.progress.completed("rebedrock:recipes/oak_planks", "has_oak_log"));
    // 奖励发配方时回打了 recipe_unlocked，所以第二个 criterion 也记上了。
    assert(second.progress.completed("rebedrock:recipes/oak_planks", "has_the_recipe"));
    assert(second.book.known().size() == 1U); // 只有一条，没有重复
}

void testUnsupportedCriterionNeverCompletes() {
    // 认不出的触发器 / 认不出的条件键 -> 那条 criterion 永不满足。
    // 本作生成的 `recipes/root` 就是这样一条（对着 vanilla 的
    // `minecraft:impossible`）：它永远完不成。
    Fixture fixture;
    for (int tick = 0; tick < 8; ++tick) {
        static_cast<void>(fixture.progress.onTick(fixture.table, fixture.book));
    }
    assert(!fixture.progress.done(fixture.table, kRecipeAdvancementRoot));
}

// --- 6. 每 tick 的槽位指纹 -------------------------------------------------

void testTickDrivesInventoryChangedFromSlotDiff() {
    Fixture fixture;
    std::vector<InventorySlotFingerprint> fingerprints;

    // 空背包：什么都不该发生。
    assert(tickPlayerAdvancements(fixture.table, fixture.inventory, fingerprints, fixture.progress,
                                  fixture.book) == 0);
    assert(fixture.book.known().empty());

    // 放进一堆原木 -> 下一 tick 的指纹比对发现那一槽变了 -> oak_planks 解锁。
    ItemStack logs = blockStack(Block::OakLog, 3U);
    assert(fixture.inventory.add(logs));
    const int unlocked = tickPlayerAdvancements(fixture.table, fixture.inventory, fingerprints,
                                                fixture.progress, fixture.book);
    assert(unlocked >= 1);
    assert(fixture.book.contains("rebedrock:oak_planks"));

    // 背包没再变：下一 tick 一条都不新增（而且不会遍历成就表——这里只能断言结果）。
    assert(tickPlayerAdvancements(fixture.table, fixture.inventory, fingerprints, fixture.progress,
                                  fixture.book) == 0);

    // 数量变了也算「变了」（vanilla 的 slotChanged 同样按槽内容变化打）。
    ItemStack more = blockStack(Block::OakLog, 5U);
    assert(fixture.inventory.add(more));
    static_cast<void>(tickPlayerAdvancements(fixture.table, fixture.inventory, fingerprints,
                                             fixture.progress, fixture.book));
}

// --- 7. 数据包覆盖 --------------------------------------------------------

void testDataPackOverlayLoadsVanillaFiles() {
    MemoryProvider pack;
    // 真 vanilla 的两个文件，原样放进 `advancement/recipes/…`（ADV-0a 修好的那个
    // 单数目录）。
    pack.add("advancement/recipes/misc/stick.json", std::string{kVanillaStickJson});
    pack.add("advancement/recipes/decorations/chest.json", std::string{kVanillaChestJson});
    // 一个用了本作没实现的触发器的文件：必须被读进来（而不是把整包带挂）。
    pack.add("advancement/story/mine_stone.json",
             R"({"criteria":{"get_stone":{"trigger":"minecraft:inventory_changed",
                 "conditions":{"items":[{"items":"#minecraft:base_stone_overworld"}]}}}})");

    AdvancementTable table;
    table.load(pack, recipeTable());

    // 文件路径给出 id，并归一化到 rebedrock:（ADV-0b）。
    const AdvancementDef& stick = advancementDef(table, "rebedrock:recipes/misc/stick");
    assert(stick.parent == "rebedrock:recipes/root");
    // 奖励里的配方 id 也归一化了，所以它发的是本作那条 `rebedrock:stick`。
    assert((stick.rewardRecipes == std::vector<std::string>{"rebedrock:stick"}));
    // 与本作生成的 `rebedrock:recipes/sticks` **不撞**（我们没有分类那层目录）。
    assert(table.find("rebedrock:recipes/sticks") != nullptr);

    // 认不出的物品标签 -> 谓词永不满足，但成就在表里。
    const ResolvedAdvancement* mineStone = table.find("rebedrock:story/mine_stone");
    assert(mineStone != nullptr);
    assert(!mineStone->criterionItems[0][0].understood);
    PlayerAdvancements progress;
    RecipeBook book;
    Inventory inventory;
    static_cast<void>(progress.onInventoryChanged(table, inventory, blockStack(Block::Stone, 1U),
                                                  book));
    assert(!progress.done(table, "rebedrock:story/mine_stone"));

    // `slots` 条件的那条也一样永不满足（背包里放 12 格东西也不完成）。
    for (int slot = 0; slot < 12; ++slot) {
        ItemStack stack = blockStack(Block::Stone, 1U);
        static_cast<void>(inventory.add(stack));
    }
    static_cast<void>(progress.onInventoryChanged(table, inventory, blockStack(Block::Stone, 1U),
                                                  book));
    assert(!progress.completed("rebedrock:recipes/decorations/chest", "has_lots_of_items"));
}

void testOverlayReplacesByIdentifier() {
    MemoryProvider pack;
    // 覆盖本作生成的那一条：同 id 的文件替换，不是追加。
    pack.add("advancement/recipes/oak_planks.json",
             R"({"criteria":{"has_the_recipe":{"trigger":"minecraft:recipe_unlocked",
                 "conditions":{"recipe":"minecraft:oak_planks"}}},
                 "requirements":[["has_the_recipe"]],
                 "rewards":{"recipes":["minecraft:bread"]}})");
    AdvancementTable table;
    const std::size_t before = [&] {
        AdvancementTable floor;
        floor.loadBuiltinDefaults(recipeTable());
        return floor.all().size();
    }();
    table.load(pack, recipeTable());
    assert(table.all().size() == before); // 替换，不是追加
    const AdvancementDef& planks = advancementDef(table, "rebedrock:recipes/oak_planks");
    assert(planks.criteria.size() == 1U);
    assert((planks.rewardRecipes == std::vector<std::string>{"rebedrock:bread"}));
}

// --- 8. 持久化 ------------------------------------------------------------

[[nodiscard]] std::filesystem::path scratchRoot(std::string_view name) {
    const auto root =
        std::filesystem::temp_directory_path() / ("mc_rebedrock_advancement_" + std::string{name});
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

void testProgressSurvivesASaveRoundTrip() {
    const auto root = scratchRoot("round_trip");
    mc::persistence::SaveRepository repository{root};
    mc::persistence::SaveGame game;
    game.summary.identifier = "world";
    game.summary.displayName = "world";

    Fixture fixture;
    static_cast<void>(fixture.progress.onInventoryChanged(
        fixture.table, fixture.inventory, blockStack(Block::OakLog, 1U), fixture.book));
    game.advancementProgress = fixture.progress.snapshot();
    assert(!game.advancementProgress.empty());
    repository.save(game);

    const auto loaded = repository.load("world");
    assert(loaded.advancementProgress == game.advancementProgress);

    PlayerAdvancements restored;
    restored.load(loaded.advancementProgress);
    assert(restored.done(fixture.table, "rebedrock:recipes/oak_planks"));
    std::filesystem::remove_all(root);
}

void testMissingBlockLoadsAsEmptyProgress() {
    // 成就层还不存在时写的存档没有 ADVP 块。缺块 = 空进度，别的字段照读。
    const auto root = scratchRoot("legacy");
    mc::persistence::SaveRepository repository{root};
    mc::persistence::SaveGame game;
    game.summary.identifier = "world";
    game.summary.displayName = "world";
    game.playerExperienceLevel = 5;
    repository.save(game); // advancementProgress 为空 -> 块里 count = 0

    const auto loaded = repository.load("world");
    assert(loaded.advancementProgress.empty());
    assert(loaded.playerExperienceLevel == 5);

    PlayerAdvancements progress;
    progress.load(loaded.advancementProgress);
    AdvancementTable table;
    table.loadBuiltinDefaults(recipeTable());
    assert(!progress.done(table, "rebedrock:recipes/oak_planks"));
    std::filesystem::remove_all(root);
}

void testLoadNormalizesAndMerges() {
    // 老存档里成就 id 可能是 `minecraft:` 拼法（ADV-0b 之前），读回来必须归一化；
    // 同一条成就两种拼法各存一次时要**合并**成一条，不是留下两条。
    PlayerAdvancements progress;
    progress.load({
        {"minecraft:recipes/oak_planks", {"has_the_recipe"}},
        {"rebedrock:recipes/oak_planks", {"has_oak_log"}},
    });
    const auto snapshot = progress.snapshot();
    assert(snapshot.size() == 1U);
    assert(snapshot[0].advancement == "rebedrock:recipes/oak_planks");
    assert((snapshot[0].criteria == std::vector<std::string>{"has_oak_log", "has_the_recipe"}));
    AdvancementTable table;
    table.loadBuiltinDefaults(recipeTable());
    assert(progress.done(table, "rebedrock:recipes/oak_planks"));
    // 用 vanilla 拼法查也查得到。
    assert(progress.completed("minecraft:recipes/oak_planks", "has_oak_log"));
}

void testSnapshotIsDeterministic() {
    // 落盘字节只由内容决定，不由完成顺序决定（RecipeBook 那条同样的规矩）。
    Fixture first;
    static_cast<void>(first.progress.onInventoryChanged(
        first.table, first.inventory, blockStack(Block::OakLog, 1U), first.book));
    static_cast<void>(first.progress.onInventoryChanged(
        first.table, first.inventory, itemStack(&items::Wheat, 1U), first.book));

    Fixture second;
    static_cast<void>(second.progress.onInventoryChanged(
        second.table, second.inventory, itemStack(&items::Wheat, 1U), second.book));
    static_cast<void>(second.progress.onInventoryChanged(
        second.table, second.inventory, blockStack(Block::OakLog, 1U), second.book));

    assert(first.progress.snapshot() == second.progress.snapshot());
}

} // namespace

int main() {
    static_cast<void>(contentRegistry());
    recipeTable().loadBuiltinDefaults();

    testRequirementsAreAndOfOrs();
    testCodecReadsVanillaShape();
    testCodecToleratesUnknownTriggers();
    testMissingRequirementsMeansAllOf();
    testGeneratedFloorMatchesVanillaShape();
    testUnlockMaterialsMatchVanilla();
    testInventoryChangedUnlocksItsRecipe();
    testInventoryChangedTestsOnlyTheChangedStackWhenSinglePredicate();
    testTickTriggerFiresUnconditionally();
    testInventoryChangedScansInventoryWhenSeveralPredicates();
    testRecipeUnlockedTriggerAndTheCycleBreak();
    testUnsupportedCriterionNeverCompletes();
    testTickDrivesInventoryChangedFromSlotDiff();
    testDataPackOverlayLoadsVanillaFiles();
    testOverlayReplacesByIdentifier();
    testProgressSurvivesASaveRoundTrip();
    testMissingBlockLoadsAsEmptyProgress();
    testLoadNormalizesAndMerges();
    testSnapshotIsDeterministic();
    return 0;
}

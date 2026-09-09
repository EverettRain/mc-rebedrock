#pragma once

// ADV-1：成就的数据形态 + 与 JE 26.1 兼容的 codec。
//
// 数据层只有数据：标识符字符串、触发器种类、谓词，没有 Item*/Block，也没有玩家
// 进度。解析（把 id 变成运行期材料）与判定都在 gameplay 那一侧。
//
// 对应 26.1：
//   * 结构 = `advancements/Advancement.java:28-52` 的 record
//     （parent / display / rewards / criteria / requirements / …）。本作只实现
//     配方解锁链要用到的四项：parent、criteria、requirements、rewards.recipes；
//     display / sends_telemetry_event 属于成就树与成就屏，归另一条线。
//   * `requirements` 的语义 = `advancements/AdvancementRequirements.java:40-52`
//     的 `test`：**组间 AND、组内 OR**（每个 List<String> 是一组，组内任一
//     criterion 满足即该组满足，所有组都满足才算完成）。
//     `requirements` 为空列表时 `test` 直接 return false（:41-43）——「一条也
//     不列」等于永不完成，不是「无条件完成」。
//   * `requirements` **字段缺失**时的默认值 = `Advancement.java:45,49` 的
//     `AdvancementRequirements.allOf(criteria.keySet())`：每个 criterion 自成
//     一组，也就是全部 AND。★ 不是 anyOf。
//   * criteria 不许为空（`Advancement.java:37-38` 的 CRITERIA_CODEC.validate）。
//
// 真 26.1 数据包里的实测（`data/minecraft/advancement/` 1617 个文件，其中
// `recipes/` 1492 个）：那 1492 个的触发器只有 5 种、conditions 的键只有 4 个、
// requirements **全部**是「恰好一个 OR 组」。本作实现其中 3 种触发器，见
// AdvancementTriggerKind。

#include "data/Codec.hpp"
#include "data/RecipeFile.hpp"

#include <algorithm>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mc::data {

// 本作实现的触发器。其余 30 来种（`advancements/criterion/` 下的其它类）一律落到
// `Unsupported`——那条 criterion **永不满足**，但**不会**让整个文件加载失败：
// 一份 vanilla 数据包里绝大多数成就用的是我们没做的触发器，把整包读挂比读进来
// 一批永不完成的成就糟得多。
enum class AdvancementTriggerKind : std::uint8_t {
    // `criterion/InventoryChangeTrigger.java`
    InventoryChanged,
    // `criterion/RecipeUnlockedTrigger.java`
    RecipeUnlocked,
    // `criterion/PlayerTrigger.java` 的 `tick()`（CriteriaTriggers.TICK）
    Tick,
    // 认不出的触发器：永不满足。
    Unsupported,
};

[[nodiscard]] inline AdvancementTriggerKind advancementTriggerFromId(std::string_view id) {
    // 两种命名空间都收（ADV-0b 的口径）。裸名也收：vanilla 自己的文件永远写全，
    // 但手写的数据包不一定。
    std::string_view name = id;
    if (const auto colon = name.find(':'); colon != std::string_view::npos) {
        const std::string_view space = name.substr(0, colon);
        if (space != "minecraft" && space != "rebedrock") {
            return AdvancementTriggerKind::Unsupported;
        }
        name = name.substr(colon + 1U);
    }
    if (name == "inventory_changed") return AdvancementTriggerKind::InventoryChanged;
    if (name == "recipe_unlocked") return AdvancementTriggerKind::RecipeUnlocked;
    if (name == "tick") return AdvancementTriggerKind::Tick;
    return AdvancementTriggerKind::Unsupported;
}

// `inventory_changed` 的 `items` 谓词里的一个元素。实测：那 1492 个文件里这个
// 元素**只有一个键** `items`，值是物品 id 或 `#tag`。
//
// 复用 IngredientDef 而不是另开一个类型：这样「这堆物品算不算这个材料」走的是
// `ingredientMatches` —— 配方匹配的单一真相源，不必再抄一份谓词，而 `#planks`
// 这类标签也已经有了表达（IngredientDefKind::Planks）。
struct AdvancementItemPredicateDef final {
    IngredientDef item;
    // 本作认不出的 tag（vanilla 有几百个物品标签，本作只有 `#planks` 这一个组）
    // 落在这里：谓词**永不满足**，但文件照常读进来。
    bool understood = true;

    [[nodiscard]] bool operator==(const AdvancementItemPredicateDef&) const = default;
};

struct AdvancementCriterionDef final {
    std::string name;
    AdvancementTriggerKind trigger = AdvancementTriggerKind::Unsupported;
    // trigger == InventoryChanged 时的 `conditions.items`。空表示「没有 items
    // 谓词」，照 `InventoryChangeTrigger.matches`（:87-89）那是**恒真**。
    std::vector<AdvancementItemPredicateDef> items;
    // trigger == RecipeUnlocked 时的 `conditions.recipe`。
    std::string recipe;
    // conditions 里出现了本作没实现的键（实测只有 `slots`，×1，在
    // `decorations/chest.json`）。★ 这时这条 criterion 也**永不满足**，而不是
    // 「忽略那个键、剩下的照判」——忽略一个约束等于把它当恒真，那会让
    // chest.json 这种「背包里放满 10 格」的成就一进游戏就完成。
    bool understood = true;

    [[nodiscard]] bool operator==(const AdvancementCriterionDef&) const = default;
};

struct AdvancementDef final {
    std::string identifier;
    std::string parent; // 空 = 根节点
    // 按名字升序存（不是 JSON 里的出现顺序）：criteria 在 JE 里是个 map，遍历
    // 顺序不该影响本作的任何输出，尤其是进度落盘的字节。
    std::vector<AdvancementCriterionDef> criteria;
    // 组间 AND、组内 OR。空 = 永不完成（AdvancementRequirements.java:41-43）。
    std::vector<std::vector<std::string>> requirements;
    // `rewards.recipes`。本作只实现 rewards 的这一项（experience/loot/function
    // 属于别的子树）。
    std::vector<std::string> rewardRecipes;

    [[nodiscard]] bool operator==(const AdvancementDef&) const = default;
};

// `requirements` 是否满足：组间 AND、组内 OR。
// 逐行对着 `AdvancementRequirements.test`（:40-52）：空列表 false；每一组都必须
// 有至少一个 criterion 让 `satisfied` 为真。
template <typename Predicate>
[[nodiscard]] bool advancementRequirementsMet(
    const std::vector<std::vector<std::string>>& requirements, Predicate satisfied) {
    if (requirements.empty()) {
        return false;
    }
    for (const auto& group : requirements) {
        bool any = false;
        for (const auto& criterion : group) {
            if (satisfied(std::string_view{criterion})) {
                any = true;
                break;
            }
        }
        if (!any) {
            return false;
        }
    }
    return true;
}

// `items` 谓词元素：`{"items": "<id 或 #tag>"}`。
//
// 写出去时按 JE 的形状写（谓词认不出的那种写回它原来的字符串是做不到的，所以
// understood=false 的谓词写成一个空对象——它本来就永不满足，round-trip 保持
// 「永不满足」这个语义即可）。
template <>
struct Codec<AdvancementItemPredicateDef> {
    static core::Json write(const AdvancementItemPredicateDef& predicate) {
        ObjectWriter writer;
        if (!predicate.understood) {
            return writer.take();
        }
        switch (predicate.item.kind) {
            case IngredientDefKind::Item:
            case IngredientDefKind::Block:
                writer.field("items", predicate.item.id);
                break;
            case IngredientDefKind::Planks:
                writer.field("items", std::string{"#minecraft:planks"});
                break;
            case IngredientDefKind::Empty:
                break;
        }
        return writer.take();
    }
    static bool read(const core::Json& json, AdvancementItemPredicateDef& out) {
        if (!json.isObject()) return false;
        out = AdvancementItemPredicateDef{};
        if (!json.contains("items")) {
            // 只有非 `items` 键的谓词（component 匹配之类）：认不出 -> 永不满足。
            out.understood = false;
            return true;
        }
        std::string value;
        if (!Codec<std::string>::read(json["items"], value)) return false;
        if (!value.empty() && value.front() == '#') {
            // 标签。本作只有 `#planks` 这一个物品组（IngredientDefKind::Planks）。
            const std::string_view tag{value.data() + 1, value.size() - 1U};
            if (tag == "minecraft:planks" || tag == "rebedrock:planks" || tag == "planks") {
                out.item.kind = IngredientDefKind::Planks;
                out.item.id.clear();
            } else {
                out.understood = false;
            }
            return true;
        }
        // 裸 id。是物品还是方块要问注册表，而数据层不许碰注册表——所以这里先记成
        // Item，由 gameplay 侧解析时按「先物品后方块」的既有口径定夺
        // （blockItemFor / itemFromIdentifier 的那条边界，见 RecipeTable）。
        out.item.kind = IngredientDefKind::Item;
        out.item.id = std::move(value);
        return true;
    }
};

template <>
struct Codec<AdvancementCriterionDef> {
    static core::Json write(const AdvancementCriterionDef& criterion) {
        ObjectWriter conditions;
        std::string trigger;
        switch (criterion.trigger) {
            case AdvancementTriggerKind::InventoryChanged:
                trigger = "minecraft:inventory_changed";
                conditions.field("items", criterion.items);
                break;
            case AdvancementTriggerKind::RecipeUnlocked:
                trigger = "minecraft:recipe_unlocked";
                conditions.field("recipe", criterion.recipe);
                break;
            case AdvancementTriggerKind::Tick:
                trigger = "minecraft:tick";
                break;
            case AdvancementTriggerKind::Unsupported:
                trigger = "rebedrock:unsupported";
                break;
        }
        core::Json::Object members;
        members.emplace_back("trigger", core::Json{trigger});
        members.emplace_back("conditions", conditions.take());
        return core::Json{std::move(members)};
    }
    static bool read(const core::Json& json, AdvancementCriterionDef& out) {
        if (!json.isObject()) return false;
        std::string trigger;
        ObjectReader reader{json};
        reader.field("trigger", trigger);
        if (!reader.ok()) return false;
        out.trigger = advancementTriggerFromId(trigger);
        const core::Json& conditions = json["conditions"];
        switch (out.trigger) {
            case AdvancementTriggerKind::InventoryChanged: {
                if (!conditions.isObject()) {
                    return true; // 没有 conditions：items 为空 -> 恒真谓词
                }
                // ★ 只认 `items`。conditions 里出现别的键（实测只有 `slots`）
                // 说明这条 criterion 带着本作不实现的约束，整条永不满足。
                for (const auto& [key, value] : conditions.asObject()) {
                    static_cast<void>(value);
                    if (key != "items") {
                        out.understood = false;
                        return true;
                    }
                }
                if (conditions.contains("items") &&
                    !Codec<std::vector<AdvancementItemPredicateDef>>::read(conditions["items"],
                                                                          out.items)) {
                    return false;
                }
                return true;
            }
            case AdvancementTriggerKind::RecipeUnlocked: {
                if (!conditions.isObject() || !conditions.contains("recipe")) {
                    out.understood = false;
                    return true;
                }
                return Codec<std::string>::read(conditions["recipe"], out.recipe);
            }
            case AdvancementTriggerKind::Tick:
            case AdvancementTriggerKind::Unsupported:
                return true;
        }
        return true;
    }
};

template <>
struct Codec<AdvancementDef> {
    static core::Json write(const AdvancementDef& advancement) {
        core::Json::Object criteria;
        criteria.reserve(advancement.criteria.size());
        for (const auto& criterion : advancement.criteria) {
            criteria.emplace_back(criterion.name,
                                  Codec<AdvancementCriterionDef>::write(criterion));
        }
        core::Json::Object rewards;
        rewards.emplace_back("recipes", Codec<std::vector<std::string>>::write(advancement.rewardRecipes));
        core::Json::Object members;
        if (!advancement.parent.empty()) {
            members.emplace_back("parent", core::Json{advancement.parent});
        }
        members.emplace_back("criteria", core::Json{std::move(criteria)});
        members.emplace_back(
            "requirements",
            Codec<std::vector<std::vector<std::string>>>::write(advancement.requirements));
        members.emplace_back("rewards", core::Json{std::move(rewards)});
        return core::Json{std::move(members)};
    }
    static bool read(const core::Json& json, AdvancementDef& out) {
        if (!json.isObject() || !json.contains("criteria")) return false;
        const core::Json& criteria = json["criteria"];
        if (!criteria.isObject() || criteria.asObject().empty()) {
            // `Advancement.java:37-38`：criteria 不许为空。
            return false;
        }
        out.criteria.clear();
        for (const auto& [name, body] : criteria.asObject()) {
            AdvancementCriterionDef criterion;
            criterion.name = name;
            if (!Codec<AdvancementCriterionDef>::read(body, criterion)) return false;
            out.criteria.push_back(std::move(criterion));
        }
        // 名字升序：JE 那边是 map，顺序不该泄漏到本作的任何输出里。
        std::sort(out.criteria.begin(), out.criteria.end(),
                  [](const AdvancementCriterionDef& a, const AdvancementCriterionDef& b) {
                      return a.name < b.name;
                  });

        ObjectReader reader{json};
        reader.optionalField("parent", out.parent);
        if (!reader.ok()) return false;

        out.requirements.clear();
        if (json.contains("requirements")) {
            if (!Codec<std::vector<std::vector<std::string>>>::read(json["requirements"],
                                                                    out.requirements)) {
                return false;
            }
        } else {
            // `Advancement.java:49`：字段缺失 = allOf，每个 criterion 自成一组。
            for (const auto& criterion : out.criteria) {
                out.requirements.push_back({criterion.name});
            }
        }

        out.rewardRecipes.clear();
        if (json.contains("rewards") && json["rewards"].isObject() &&
            json["rewards"].contains("recipes")) {
            if (!Codec<std::vector<std::string>>::read(json["rewards"]["recipes"],
                                                       out.rewardRecipes)) {
                return false;
            }
        }
        return true;
    }
};

} // namespace mc::data

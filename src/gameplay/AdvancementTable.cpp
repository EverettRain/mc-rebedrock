#include "gameplay/AdvancementTable.hpp"

#include "compat/ContentNamespace.hpp"
#include "core/Json.hpp"
#include "data/DataPackPaths.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "world/Block.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace mc::gameplay {

const std::vector<AdvancementCriterionRef> AdvancementTable::kNoCriteria{};

namespace {

// `rebedrock:oak_planks` -> `oak_planks`。命名空间之后那一段。
[[nodiscard]] std::string_view shortName(std::string_view identifier) {
    const auto colon = identifier.find(':');
    return colon == std::string_view::npos ? identifier : identifier.substr(colon + 1U);
}

// 把数据层的 items 谓词解析成运行期材料。
//
// 裸 id 是物品还是方块，codec 分不出来（数据层不许碰注册表），所以定夺在这里：
// **先物品后方块**——与 RecipeTable::resolveOutput 同一条口径（「同名 block/item
// 双端桥」那条：真物品优先于同名方块）。两边都查不到 -> 认不出，永不满足。
[[nodiscard]] ResolvedItemPredicate resolvePredicate(
    const data::AdvancementItemPredicateDef& predicate) {
    ResolvedItemPredicate resolved;
    if (!predicate.understood) {
        resolved.understood = false;
        return resolved;
    }
    switch (predicate.item.kind) {
        case data::IngredientDefKind::Planks:
            resolved.item = RecipeIngredient{IngredientKind::AnyPlanks, world::Block::Air, nullptr};
            return resolved;
        case data::IngredientDefKind::Empty:
            resolved.understood = false;
            return resolved;
        case data::IngredientDefKind::Item:
        case data::IngredientDefKind::Block:
            break;
    }
    if (const Item* item = itemFromIdentifier(predicate.item.id); item != nullptr) {
        resolved.item = RecipeIngredient{IngredientKind::Item, world::Block::Air, item};
        return resolved;
    }
    if (const auto block = world::blockFromIdentifier(predicate.item.id); block.has_value()) {
        resolved.item = RecipeIngredient{IngredientKind::Block, *block, nullptr};
        return resolved;
    }
    resolved.understood = false;
    return resolved;
}

// 生成的成就 id：`rebedrock:recipes/<配方短名>`。
[[nodiscard]] std::string recipeAdvancementId(std::string_view recipeIdentifier) {
    return std::string{compat::kOwnNamespace} + ":recipes/" + std::string{shortName(recipeIdentifier)};
}

} // namespace

bool advancementForRecipeUnlock(std::string_view recipeIdentifier,
                                const data::IngredientDef& unlockedBy,
                                data::AdvancementDef& out) {
    if (unlockedBy.kind == data::IngredientDefKind::Empty) {
        return false;
    }
    // criterion 的名字。vanilla 那 1492 个文件里是 `has_the_recipe` + `has_<材料>`
    // （`RecipeProvider` 生成时按材料取名）。前者逐字照抄；后者本作取的是材料的
    // **短名**，vanilla 取的是构建器里那个词，两者不总是同一个字（vanilla 的
    // oak_planks 用 `has_logs`，本作是 `has_oak_log`）。★ 登记为偏差：criterion
    // 名字只在 requirements 与进度存档里出现，不面向玩家，也不进网络协议，所以
    // 逐字对齐没有收益；形状（`has_the_recipe` + 一个 `has_*`）是对齐的。
    const std::string materialCriterion =
        unlockedBy.kind == data::IngredientDefKind::Planks
            ? std::string{"has_planks"}
            : "has_" + std::string{shortName(unlockedBy.id)};
    constexpr std::string_view kRecipeCriterion = "has_the_recipe";

    const std::string canonicalRecipe = compat::canonicalContentId(recipeIdentifier);

    data::AdvancementCriterionDef material;
    material.name = materialCriterion;
    material.trigger = data::AdvancementTriggerKind::InventoryChanged;
    material.items.push_back(data::AdvancementItemPredicateDef{unlockedBy, true});

    data::AdvancementCriterionDef recipe;
    recipe.name = std::string{kRecipeCriterion};
    recipe.trigger = data::AdvancementTriggerKind::RecipeUnlocked;
    recipe.recipe = canonicalRecipe;

    out = data::AdvancementDef{};
    out.identifier = recipeAdvancementId(canonicalRecipe);
    out.parent = std::string{kRecipeAdvancementRoot};
    // 名字升序（AdvancementDef 的约定）。
    if (materialCriterion < kRecipeCriterion) {
        out.criteria.push_back(std::move(material));
        out.criteria.push_back(std::move(recipe));
    } else {
        out.criteria.push_back(std::move(recipe));
        out.criteria.push_back(std::move(material));
    }
    // 一个 OR 组，顺序照 vanilla 的文件（`has_the_recipe` 在前）。
    out.requirements.push_back({std::string{kRecipeCriterion}, materialCriterion});
    out.rewardRecipes.push_back(canonicalRecipe);
    return true;
}

void AdvancementTable::clear() {
    advancements_.clear();
    byIdentifier_.clear();
    inventoryChanged_.clear();
    tick_.clear();
    recipeUnlocked_.clear();
}

void AdvancementTable::merge(data::AdvancementDef def) {
    ResolvedAdvancement resolved;
    resolved.criterionItems.reserve(def.criteria.size());
    for (const auto& criterion : def.criteria) {
        std::vector<ResolvedItemPredicate> predicates;
        predicates.reserve(criterion.items.size());
        for (const auto& predicate : criterion.items) {
            predicates.push_back(resolvePredicate(predicate));
        }
        resolved.criterionItems.push_back(std::move(predicates));
    }
    resolved.def = std::move(def);

    const auto slot = byIdentifier_.find(resolved.def.identifier);
    if (slot != byIdentifier_.end()) {
        advancements_[slot->second] = std::move(resolved);
        return;
    }
    byIdentifier_.emplace(resolved.def.identifier, advancements_.size());
    advancements_.push_back(std::move(resolved));
}

void AdvancementTable::rebuildIndex() {
    inventoryChanged_.clear();
    tick_.clear();
    recipeUnlocked_.clear();
    for (std::size_t advancement = 0; advancement < advancements_.size(); ++advancement) {
        const auto& criteria = advancements_[advancement].def.criteria;
        for (std::size_t index = 0; index < criteria.size(); ++index) {
            const AdvancementCriterionRef ref{advancement, index};
            switch (criteria[index].trigger) {
                case data::AdvancementTriggerKind::InventoryChanged:
                    inventoryChanged_.push_back(ref);
                    break;
                case data::AdvancementTriggerKind::Tick:
                    tick_.push_back(ref);
                    break;
                case data::AdvancementTriggerKind::RecipeUnlocked:
                    recipeUnlocked_[compat::canonicalContentId(criteria[index].recipe)]
                        .push_back(ref);
                    break;
                case data::AdvancementTriggerKind::Unsupported:
                    break; // 永不满足，不进任何索引
            }
        }
    }
}

void AdvancementTable::loadBuiltinDefaults(const RecipeTable& recipes) {
    clear();
    // 根节点：vanilla 的 `recipes/root` 用 `minecraft:impossible` 触发器（永不
    // 完成），只是给成就树当挂点。本作照样建一条，好让生成出来的成就有 parent
    // 可指——成就树是另一条线的事，这里只保证 parent 指得到一个真实存在的 id。
    data::AdvancementDef root;
    root.identifier = std::string{kRecipeAdvancementRoot};
    data::AdvancementCriterionDef impossible;
    impossible.name = "impossible";
    impossible.trigger = data::AdvancementTriggerKind::Unsupported;
    root.criteria.push_back(std::move(impossible));
    root.requirements.push_back({std::string{"impossible"}});
    merge(std::move(root));

    const auto unlockDef = [](const RecipeUnlock& unlock) {
        data::IngredientDef def;
        switch (unlock.kind) {
            case IngredientKind::AnyPlanks:
                def.kind = data::IngredientDefKind::Planks;
                break;
            case IngredientKind::Item:
                def.kind = data::IngredientDefKind::Item;
                def.id = std::string{unlock.identifier};
                break;
            case IngredientKind::Block:
                def.kind = data::IngredientDefKind::Block;
                def.id = std::string{unlock.identifier};
                break;
            case IngredientKind::Empty:
                break;
        }
        return def;
    };
    for (const auto& recipe : recipes.crafting()) {
        data::AdvancementDef def;
        if (advancementForRecipeUnlock(recipe.identifier, unlockDef(recipe.unlockedBy), def)) {
            merge(std::move(def));
        }
    }
    for (const auto& recipe : recipes.furnace()) {
        data::AdvancementDef def;
        if (advancementForRecipeUnlock(recipe.identifier, unlockDef(recipe.unlockedBy), def)) {
            merge(std::move(def));
        }
    }
    rebuildIndex();
}

void AdvancementTable::load(const assets::ResourceProvider& resources, const RecipeTable& recipes) {
    loadBuiltinDefaults(recipes);
    applyOverlay(resources);
    rebuildIndex();
}

void AdvancementTable::applyOverlay(const assets::ResourceProvider& resources) {
    for (const auto& location : resources.list("minecraft", data::pack::kAdvancementDir,
                                               assets::PackType::ServerData)) {
        const auto bytes = resources.readBytes(location);
        if (bytes.empty()) {
            continue;
        }
        core::Json root;
        try {
            root = core::Json::parse(std::string_view{
                reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        } catch (const std::exception&) {
            continue; // 一个坏文件不许把整包带下水
        }
        data::AdvancementDef def;
        if (!data::Codec<data::AdvancementDef>::read(root, def)) {
            continue;
        }
        // 文件路径给出 id：`advancement/recipes/misc/stick.json` ->
        // `minecraft:recipes/misc/stick`，再归一化到 `rebedrock:`（ADV-0b）。
        std::string_view path = location.path;
        const std::string_view prefix = data::pack::kAdvancementDir;
        if (path.size() > prefix.size() && path.substr(0, prefix.size()) == prefix &&
            path[prefix.size()] == '/') {
            path.remove_prefix(prefix.size() + 1U);
        }
        if (path.size() >= 5U && path.substr(path.size() - 5U) == ".json") {
            path.remove_suffix(5U);
        }
        def.identifier = compat::canonicalContentId(location.space + ":" + std::string{path});
        if (!def.parent.empty()) {
            def.parent = compat::canonicalContentId(def.parent);
        }
        for (auto& recipe : def.rewardRecipes) {
            recipe = compat::canonicalContentId(recipe);
        }
        merge(std::move(def));
    }
}

const ResolvedAdvancement* AdvancementTable::find(std::string_view identifier) const {
    const auto slot = byIdentifier_.find(compat::canonicalContentId(identifier));
    return slot == byIdentifier_.end() ? nullptr : &advancements_[slot->second];
}

std::span<const AdvancementCriterionRef> AdvancementTable::recipeUnlockedCriteria(
    std::string_view recipeIdentifier) const {
    const auto slot = recipeUnlocked_.find(compat::canonicalContentId(recipeIdentifier));
    return slot == recipeUnlocked_.end() ? std::span<const AdvancementCriterionRef>{kNoCriteria}
                                         : std::span<const AdvancementCriterionRef>{slot->second};
}

AdvancementTable& advancementTable() {
    static AdvancementTable table = [] {
        AdvancementTable defaults;
        defaults.loadBuiltinDefaults(recipeTable());
        return defaults;
    }();
    return table;
}

} // namespace mc::gameplay

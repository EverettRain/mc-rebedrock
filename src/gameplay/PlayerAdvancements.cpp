#include "gameplay/PlayerAdvancements.hpp"

#include "compat/ContentNamespace.hpp"

#include <algorithm>
#include <span>
#include <string>
#include <utility>

namespace mc::gameplay {
namespace {

[[nodiscard]] bool insertSorted(std::vector<std::string>& sorted, std::string_view value) {
    const auto position = std::ranges::lower_bound(sorted, value, std::less<>{});
    if (position != sorted.end() && *position == value) {
        return false;
    }
    sorted.emplace(position, value);
    return true;
}

// `InventoryChangeTrigger.TriggerInstance.matches`（:83-108），逐分支照抄：
//   * 没有 items 谓词 -> 恒真（:87-89）；
//   * **恰好一个**谓词 -> 只测**这次变的那一堆**，不是整个背包（:105-107）；
//   * 多于一个 -> 每个谓词都要能被背包里的某一槽满足（:91-104）。
// 认不出的谓词（本作没有的 tag / 没有的 id）一律不满足。
[[nodiscard]] bool inventoryChangeMatches(const data::AdvancementCriterionDef& criterion,
                                          std::span<const ResolvedItemPredicate> predicates,
                                          const Inventory& inventory,
                                          const ItemStack& changedStack) {
    if (!criterion.understood) {
        return false; // conditions 里有本作不实现的键（slots）：整条永不满足
    }
    if (predicates.empty()) {
        return true;
    }
    if (predicates.size() == 1U) {
        const auto& predicate = predicates.front();
        return predicate.understood && !changedStack.empty() &&
               ingredientMatches(predicate.item, changedStack);
    }
    for (const auto& predicate : predicates) {
        if (!predicate.understood) {
            return false;
        }
        const bool any = std::ranges::any_of(inventory.slots(), [&predicate](const ItemStack& slot) {
            return !slot.empty() && ingredientMatches(predicate.item, slot);
        });
        if (!any) {
            return false;
        }
    }
    return true;
}

} // namespace

bool PlayerAdvancements::markCompleted(std::string_view advancementId,
                                       std::string_view criterionName) {
    const std::string canonical = compat::canonicalContentId(advancementId);
    const auto position = std::ranges::lower_bound(entries_, canonical, std::less<>{},
                                                   &AdvancementProgressEntry::advancement);
    if (position == entries_.end() || position->advancement != canonical) {
        AdvancementProgressEntry entry;
        entry.advancement = canonical;
        entry.criteria.emplace_back(criterionName);
        entries_.emplace(position, std::move(entry));
        return true;
    }
    return insertSorted(position->criteria, criterionName);
}

bool PlayerAdvancements::completed(std::string_view advancementId,
                                   std::string_view criterionName) const {
    // 归一化的快路：已经是规范形（触发器分发递进来的全是）就不分配。这条查询在
    // 背包变化的那一 tick 会被每条 inventory_changed criterion 各问一次。
    const std::string owned =
        compat::isVanillaNamespaced(advancementId) ? compat::canonicalContentId(advancementId)
                                                   : std::string{};
    const std::string_view canonical = owned.empty() ? advancementId : std::string_view{owned};
    const auto position = std::ranges::lower_bound(entries_, canonical, std::less<>{},
                                                   &AdvancementProgressEntry::advancement);
    if (position == entries_.end() || position->advancement != canonical) {
        return false;
    }
    return std::ranges::binary_search(position->criteria, criterionName, std::less<>{});
}

bool PlayerAdvancements::done(const AdvancementTable& table,
                              std::string_view advancementId) const {
    const ResolvedAdvancement* advancement = table.find(advancementId);
    if (advancement == nullptr) {
        return false;
    }
    return data::advancementRequirementsMet(
        advancement->def.requirements, [&](std::string_view criterion) {
            return completed(advancement->def.identifier, criterion);
        });
}

int PlayerAdvancements::grantRewards(const AdvancementTable& table,
                                     const data::AdvancementDef& advancement, RecipeBook& book) {
    int unlocked = 0;
    for (const auto& recipe : advancement.rewardRecipes) {
        // `ServerRecipeBook.addRecipes`（:61-80）：已经认识的一条都不动，返回 0。
        // 这正是 recipe_unlocked 回路的断点——第二遍进来发不出去，也就不再触发。
        if (book.addRecipe(recipe) == 0) {
            continue;
        }
        ++unlocked;
        // 真的新解锁了才回打触发器（vanilla 的 CriteriaTriggers.RECIPE_UNLOCKED
        // 也在那个 if 里面）。
        unlocked += onRecipeUnlocked(table, recipe, book);
    }
    return unlocked;
}

int PlayerAdvancements::award(const AdvancementTable& table, std::string_view advancementId,
                              std::string_view criterionName, RecipeBook& book) {
    const ResolvedAdvancement* advancement = table.find(advancementId);
    if (advancement == nullptr) {
        return 0;
    }
    const bool wasDone = done(table, advancement->def.identifier);
    if (!markCompleted(advancement->def.identifier, criterionName)) {
        return 0; // 这条 criterion 早就记过了
    }
    if (wasDone || !done(table, advancement->def.identifier)) {
        return 0; // 还没完成，或者本来就完成了：不发奖
    }
    return grantRewards(table, advancement->def, book);
}

int PlayerAdvancements::onInventoryChanged(const AdvancementTable& table,
                                           const Inventory& inventory,
                                           const ItemStack& changedStack, RecipeBook& book) {
    int unlocked = 0;
    const auto all = table.all();
    // 索引是快照：award 不会改表，但会改进度，所以这里按 ref 逐条查，安全。
    for (const auto& ref : table.inventoryChangedCriteria()) {
        const auto& advancement = all[ref.advancement];
        const auto& criterion = advancement.def.criteria[ref.criterion];
        if (completed(advancement.def.identifier, criterion.name)) {
            continue;
        }
        if (!inventoryChangeMatches(criterion, advancement.criterionItems[ref.criterion], inventory,
                                    changedStack)) {
            continue;
        }
        unlocked += award(table, advancement.def.identifier, criterion.name, book);
    }
    return unlocked;
}

int PlayerAdvancements::onRecipeUnlocked(const AdvancementTable& table,
                                         std::string_view recipeIdentifier, RecipeBook& book) {
    int unlocked = 0;
    const auto all = table.all();
    // ★ 索引返回的是表内的 vector 的 span，而 award 只改进度不改表，所以边遍历
    // 边 award 是安全的。复制一份是为了递归（grantRewards -> onRecipeUnlocked）
    // 时不依赖上一层的 span 还活着——它其实活着，但让这条不变量靠值而不是靠推理。
    const std::vector<AdvancementCriterionRef> refs = [&] {
        const auto span = table.recipeUnlockedCriteria(recipeIdentifier);
        return std::vector<AdvancementCriterionRef>{span.begin(), span.end()};
    }();
    for (const auto& ref : refs) {
        const auto& advancement = all[ref.advancement];
        const auto& criterion = advancement.def.criteria[ref.criterion];
        if (completed(advancement.def.identifier, criterion.name)) {
            continue;
        }
        unlocked += award(table, advancement.def.identifier, criterion.name, book);
    }
    return unlocked;
}

int PlayerAdvancements::onTick(const AdvancementTable& table, RecipeBook& book) {
    int unlocked = 0;
    const auto all = table.all();
    for (const auto& ref : table.tickCriteria()) {
        const auto& advancement = all[ref.advancement];
        const auto& criterion = advancement.def.criteria[ref.criterion];
        if (completed(advancement.def.identifier, criterion.name)) {
            continue;
        }
        unlocked += award(table, advancement.def.identifier, criterion.name, book);
    }
    return unlocked;
}

void PlayerAdvancements::load(std::vector<AdvancementProgressEntry> entries) {
    entries_ = std::move(entries);
    for (auto& entry : entries_) {
        if (compat::isVanillaNamespaced(entry.advancement)) {
            entry.advancement = compat::canonicalContentId(entry.advancement);
        }
        std::ranges::sort(entry.criteria);
        const auto duplicates = std::ranges::unique(entry.criteria);
        entry.criteria.erase(duplicates.begin(), duplicates.end());
    }
    std::ranges::sort(entries_, {}, &AdvancementProgressEntry::advancement);
    // 归一化之后可能出现两条同 id 的记录（老存档里 `minecraft:` 与 `rebedrock:`
    // 各存了一次）：合并它们的 criterion，而不是留下两条。
    std::vector<AdvancementProgressEntry> merged;
    for (auto& entry : entries_) {
        if (!merged.empty() && merged.back().advancement == entry.advancement) {
            for (const auto& criterion : entry.criteria) {
                static_cast<void>(insertSorted(merged.back().criteria, criterion));
            }
            continue;
        }
        merged.push_back(std::move(entry));
    }
    entries_ = std::move(merged);
}

void PlayerAdvancements::clear() { entries_.clear(); }

int tickPlayerAdvancements(const AdvancementTable& table, const Inventory& inventory,
                           std::vector<InventorySlotFingerprint>& fingerprints,
                           PlayerAdvancements& progress, RecipeBook& book) {
    const auto slots = inventory.slots();
    fingerprints.resize(slots.size());
    int unlocked = 0;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        const ItemStack& stack = slots[index];
        const InventorySlotFingerprint current{stack.item, stack.block, stack.count};
        if (current == fingerprints[index]) {
            continue;
        }
        fingerprints[index] = current;
        if (stack.empty()) {
            continue; // 变成空的那一槽不是「获得了什么」
        }
        unlocked += progress.onInventoryChanged(table, inventory, stack, book);
    }
    unlocked += progress.onTick(table, book);
    return unlocked;
}

} // namespace mc::gameplay

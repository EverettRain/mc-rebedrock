#pragma once

// ADV-1：一个玩家的成就进度 + 三个触发器的分发 + 奖励发放。
//
// 对应 26.1：
//   * 进度 = `server/PlayerAdvancements.java` 的 `progress`（每条成就一份
//     AdvancementProgress，里面是「哪些 criterion 已完成」）。
//   * 完成判定 = `AdvancementProgress.isDone()` -> `AdvancementRequirements.test`
//     （:40-52，组间 AND、组内 OR）。
//   * 发奖时机 = `PlayerAdvancements.award`（:167-190）：`grantProgress` 成功、
//     且**这次**才变成完成（`!wasDone && progress.isDone()`）时才 grant，
//     所以奖励只发一次。
//   * 奖励里的配方 = `AdvancementRewards.grant`（:78-80）->
//     `ServerPlayer.awardRecipesByKey` -> `ServerRecipeBook.addRecipes`。
//
// ★★ 回路。`ServerRecipeBook.addRecipes`（:61-80）在**真的新解锁**一条配方之后
// 会回头触发 `RECIPE_UNLOCKED`，而那条触发器又可能完成一条奖励同一批配方的成就
// ——给了配方 -> 完成那条成就 -> 又发同一个配方。vanilla 的断点是那句
// `if (!this.known.contains(id) …)`：第二遍进来时配方已经在 known 里，什么都不
// 发，也就不再触发。本作照抄：`RecipeBook::addRecipes` 返回的是**新**解锁的条数，
// 这里只对新解锁的那些回打 recipe_unlocked。

#include "data/AdvancementFile.hpp"
#include "gameplay/AdvancementTable.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/RecipeBook.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mc::gameplay {

// 一条成就上已完成的 criterion。按成就 id 升序、组内按 criterion 名升序存
// ——落盘字节必须只由内容决定，不由完成顺序决定（RecipeBook 那条同样的规矩）。
struct AdvancementProgressEntry final {
    std::string advancement;
    std::vector<std::string> criteria;

    [[nodiscard]] bool operator==(const AdvancementProgressEntry&) const = default;
};

class PlayerAdvancements final {
  public:
    // `PlayerAdvancements.award`（:167-190）。记下这条 criterion；如果这次让整条
    // 成就从「未完成」变成「完成」，就发奖（rewards.recipes -> 配方书）。
    // 返回这次**新**解锁了几条配方（回路断点见头注）。
    int award(const AdvancementTable& table, std::string_view advancementId,
              std::string_view criterionName, RecipeBook& book);

    [[nodiscard]] bool completed(std::string_view advancementId,
                                 std::string_view criterionName) const;
    // 这条成就的 requirements 是否已满足。
    [[nodiscard]] bool done(const AdvancementTable& table, std::string_view advancementId) const;

    // --- 三个触发器 ------------------------------------------------------
    //
    // `InventoryChangeTrigger.trigger`（:23-46）：vanilla 是**每次背包槽变化**打
    // 一次，带上那一槽的新内容（ServerPlayer.java:317-327 的 containerListener）。
    // 本作没有 container listener，等价做法是每个权威 tick 与上一 tick 的槽位
    // 指纹比一遍，变了的槽逐个打一次——见 GameSession 的调用点。
    int onInventoryChanged(const AdvancementTable& table, const Inventory& inventory,
                           const ItemStack& changedStack, RecipeBook& book);
    // `RecipeUnlockedTrigger.trigger`（:20-22）
    int onRecipeUnlocked(const AdvancementTable& table, std::string_view recipeIdentifier,
                         RecipeBook& book);
    // `PlayerTrigger.trigger`（:20-22）的 tick 实例：无条件满足。
    int onTick(const AdvancementTable& table, RecipeBook& book);

    // --- 存档 ------------------------------------------------------------
    [[nodiscard]] std::vector<AdvancementProgressEntry> snapshot() const { return entries_; }
    // 从存档装回来。会排序去重并把成就 id 归一化（ADV-0b 的边界），所以一份被
    // 手改乱序的存档读进来也是规范形。
    void load(std::vector<AdvancementProgressEntry> entries);
    void clear();

  private:
    // 记下 (成就, criterion)，返回是不是**新**记的。
    bool markCompleted(std::string_view advancementId, std::string_view criterionName);
    // rewards.recipes -> 配方书，然后对新解锁的那些回打 recipe_unlocked。
    int grantRewards(const AdvancementTable& table, const data::AdvancementDef& advancement,
                     RecipeBook& book);

    std::vector<AdvancementProgressEntry> entries_;
};

// 一个背包槽的「变了没有」指纹。ItemStack 整个比是错的成本（附魔表、自定义名是
// 堆上的），而这一层的谓词只问物品身份与数量——所以指纹只留这三样。
struct InventorySlotFingerprint final {
    const Item* item = nullptr;
    world::Block block = world::Block::Air;
    std::uint8_t count = 0U;

    [[nodiscard]] bool operator==(const InventorySlotFingerprint&) const = default;
};

// 每个权威 tick 跑一次：与上一 tick 的指纹比，变了的槽逐个打 inventory_changed，
// 然后打一次 tick 触发器。返回这一 tick 新解锁了几条配方。
//
// ★ 第一次调用（指纹表还是空的）会把玩家当前**已经**拿着的东西全当成「刚变的」
// 打一遍。这是有意的：老存档（成就层还不存在时写的）里玩家手里的材料本来就该
// 解锁对应配方，而 criterion 一旦记下就是幂等的，重复打不会有第二次效果。
int tickPlayerAdvancements(const AdvancementTable& table, const Inventory& inventory,
                           std::vector<InventorySlotFingerprint>& fingerprints,
                           PlayerAdvancements& progress, RecipeBook& book);

} // namespace mc::gameplay

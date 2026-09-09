#pragma once

// 配方书的后端：每个玩家一份「已解锁」与「待高亮」两个集合，加上配方书那一屏要
// 的三件事——列出这一屏能显示哪些配方、这份背包做不做得出来、一键把材料填进网格。
// 界面一个字都不在这里。
//
// 对应 26.1：
//   * 两个集合 = `stats/ServerRecipeBook.java:32-34` 的 `known` / `highlight`；
//     增删与「新解锁的要高亮」的规矩在 `addRecipes`（:61-80）/ `remove`（:48-51）。
//     `stats/RecipeBook.java` 本身只管每个页签的开关与过滤，那是界面状态，不在这层。
//   * 一键填充 = `recipebook/ServerPlaceRecipe.java` 全文 + `PlaceRecipeHelper.java`。
//   * 「够不够做」= `world/entity/player/StackedItemContents.java`，见
//     gameplay/StackedItemContents.hpp。
//
// ★ 配方是**怎么被解锁**的（本作与 vanilla 的唯一实质差异，见 unlock 规则那段）。

#include "gameplay/CraftingSystem.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/RecipeBookCategory.hpp"
#include "gameplay/ScreenTypes.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mc::gameplay {

// 一个玩家的配方书。`known` 是已解锁，`highlight` 是「新解锁、还没被看过」——
// 26.1 里前者决定配方书里画不画得出这条，后者决定那条上有没有那个小闪光。
//
// 两个集合都存成**按标识符升序的 vector**，不是哈希集合：vanilla 用
// `Sets.newIdentityHashSet()`（ServerRecipeBook.java:32-34），遍历顺序是没定义
// 的，而这两个集合要落存档——落盘字节必须只由内容决定，不由插入顺序决定，否则
// 「什么都没改也重新写了一遍」。集合本身几百条，二分足够。
class RecipeBook final {
  public:
    // `ServerRecipeBook.add`（:40-42）：只进 known，不高亮。已经在里面就返回 false。
    bool add(std::string_view identifier);
    // `ServerRecipeBook.contains`（:44-46）
    [[nodiscard]] bool contains(std::string_view identifier) const;
    // `ServerRecipeBook.remove`（:48-51）：两个集合一起删。
    void remove(std::string_view identifier);
    // `ServerRecipeBook.removeHighlight`（:53-55）：玩家看过了。
    void removeHighlight(std::string_view identifier);
    [[nodiscard]] bool highlighted(std::string_view identifier) const;

    // `ServerRecipeBook.addRecipes`（:61-80）：还不认识的才收，收的同时点亮高亮；
    // 返回这次**新**解锁了几条。已经认识的一条都不动（也不会重新点亮）。
    int addRecipes(std::span<const std::string_view> identifiers);
    // 单条形式，上面那条的常用入口。
    int addRecipe(std::string_view identifier);

    [[nodiscard]] std::span<const std::string> known() const { return known_; }
    [[nodiscard]] std::span<const std::string> highlight() const { return highlight_; }

    // `ServerRecipeBook.loadUntrusted`（:139-143）的位置：从存档装回来。两个集合
    // 都会被排序去重，所以一份被手改乱序的存档读进来也是规范形。
    void load(std::vector<std::string> known, std::vector<std::string> highlight);
    void clear();

  private:
    std::vector<std::string> known_;
    std::vector<std::string> highlight_;
};

// ★★ 解锁规则。
//
// 26.1 里配方**主要**不是靠代码解锁的，是靠成就（advancement）：vanilla 数据包
// 给每一条配方生成一个 `recipes/<分组>/<名字>.json` 成就，条件是
// `minecraft:inventory_changed`「背包里出现了某个材料」，奖励是
// `{"rewards": {"recipes": [...]}}`；发奖走
// `advancements/AdvancementRewards.java` -> `ServerPlayer.awardRecipes`
// （ServerPlayer.java:1485-1488）-> `ServerRecipeBook.addRecipes`。
// 代码里直接解锁的只有三条小路：合成/烧炼**用过**这条配方
// （`world/inventory/RecipeCraftingHolder.java:17-26` 的 `awardUsedRecipes`）、
// 知识之书（`KnowledgeBookItem`）、`/recipe give`（`RecipeCommand`）。
//
// 本作**没有成就系统**（`grep -rn "Advancement" src/` 零命中），所以那条主路没有
// 落脚点。这里选的简化规则是：
//
//   **玩家背包里出现某条配方的任一材料时，解锁这条配方。**
//
// 它对着 vanilla 那个成就的触发器（`inventory_changed` + `has_<材料>`）来，只是
// 把「配方作者挑的那一个材料」放宽成「任意一个材料」——vanilla 的 requirements 是
// 一个 OR 数组，本来就是「任一条满足即可」，只是数组里通常只列了一个材料。
// 登记为偏差：本作解锁得比 vanilla **早**（例：木镐在 vanilla 只由 `has_stick`
// 触发，本作拿到木板也会解锁），且 vanilla 里少数「一进游戏就给」的配方
// （`recipes/decorations/crafting_table.json` 的 `unlock_right_away` 用的是
// `minecraft:tick` 触发器）在本作要等到拿到木板才解锁。
[[nodiscard]] int awardRecipesForAcquiredStack(RecipeBook& book, const ItemStack& acquired);
// 整个背包扫一遍（拾取/合成/给予之后调一次就够）。返回新解锁了几条。
[[nodiscard]] int awardRecipesForInventory(RecipeBook& book, const Inventory& inventory);

// 配方书里的一条。
struct RecipeBookEntry final {
    std::string_view identifier;
    ItemStack result;
    RecipeBookCategory category = RecipeBookCategory::CraftingMisc;
    // 当前背包做得出来吗。★ 只看背包，不看网格里已经摆着的东西——26.1 的
    // `RecipeBookComponent` 是把背包**和**合成格一起喂给 StackedItemContents 的
    // （`ServerPlaceRecipe.java:44-46`），本作这一层拿不到网格，所以这里偏保守：
    // 网格里已经摆了材料时可能显示为做不出来。一键填充自己会把网格算进去。
    bool craftable = false;
    bool unlocked = false;
};

// 这一屏能显示哪些配方，按 26.1 的分类分好组（同一分类的排在一起，分类之间按
// `RecipeBookCategories.java` 的注册顺序）。
//
// 哪些配方进得来，照 `client/.../CraftingRecipeBookComponent.java:44-55` 的
// `canDisplay`：有形状的要网格装得下它的宽高，无序的要格数够放下所有材料。
// 熔炉屏给熔炉配方；箱子/附魔台/铁砧没有配方书（那三个菜单在 26.1 里根本不是
// `RecipeBookMenu`），返回空。
[[nodiscard]] std::vector<RecipeBookEntry> recipeBookEntries(
    ContainerScreen screen, const Inventory& inventory, const RecipeBook& book);

// 一键填充。`maxStack` = Shift 点击（尽量多放），否则放一组。
// 返回 false 表示**什么都没动**（材料不够 / 找不到配方 / 网格退不回背包 /
// 这一屏没有合成网格）。
//
// 算法照 `ServerPlaceRecipe.java`；两处如实偏离，各自在实现里标了。
[[nodiscard]] bool placeRecipe(Inventory& inventory, CraftingSystem& crafting,
                               ContainerScreen screen, std::string_view recipeIdentifier,
                               bool maxStack);

} // namespace mc::gameplay

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
//
// ★★ ADV-0b 命名空间口径。本作的底座 id 一律是 `rebedrock:`（RecipeBakedData.inc
// 与 RecipeBookCategoryData.inc），而**每一个解析边界**同时接受 `minecraft:` 与
// `rebedrock:`，两者指同一条配方。归一化只有一处实现：
// compat/ContentNamespace.hpp 的 `canonicalContentId`。
//
// 配方 id 的解析边界清单（加新边界要同步补进来，并在 recipe_book_test 的
// testVanillaNamespaceAcceptedAtEveryBoundary 里各走一遍）：
//   ① RecipeTable::applyOverlay —— 数据包文件名给出的 id（RecipeTable.cpp）。
//      vanilla 包的 `minecraft:oak_planks` 必须**覆盖**我们的同名配方，而不是
//      追加成第二条。
//   ② RecipeBook 的每一个公开入口 —— add / contains / remove / removeHighlight /
//      highlighted / addRecipes / addRecipe / load。RCPB 存档块、命令、成就奖励
//      走的都是这几个函数。
//   ③ craftingRecipeByIdentifier（placeRecipe 的入口）。
//   ④ craftingRecipeBookCategory / furnaceRecipeBookCategory 的二分查表
//      （RecipeBookCategory.cpp）——★ 最易漏：查不到不报错，只会静默落到 Misc。
// 本仓在「同名 block/item 双端桥」那次栽过一模一样的坑：codec 层不改、只改命令
// 层是无效的，因为另一条入口绕过了修好的那一处。

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

// ★★ 解锁规则：**成就链**，见 gameplay/PlayerAdvancements.hpp。
//
// 26.1 里配方主要不是靠代码解锁的，是靠成就：vanilla 数据包给每一条配方生成一个
// `recipes/<分组>/<名字>.json`，条件是 `inventory_changed`「背包里出现了某个
// 材料」或 `recipe_unlocked`，奖励是 `{"rewards": {"recipes": [...]}}`；发奖走
// `advancements/AdvancementRewards.java` -> `ServerPlayer.awardRecipes`
// （ServerPlayer.java:1485-1488）-> `ServerRecipeBook.addRecipes`。本作 ADV-1
// 把这条链整条做了出来，底座在加载期从配方表生成（AdvancementTable.hpp）。
//
// ADV-1 之前这里有一条简化规则（`awardRecipesForAcquiredStack` /
// `awardRecipesForInventory`：背包里出现某条配方的**任一**材料就解锁它）。它连同
// GameSession 的调用点一起删了——成就链取代它，两条路并存就是两个口径。
//
// 代码里直接解锁的三条小路（合成/烧炼用过这条配方、知识之书、`/recipe give`）
// 与 vanilla 一样仍然绕过成就，走 addRecipes。

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

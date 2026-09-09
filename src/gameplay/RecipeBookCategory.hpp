#pragma once

// 配方书的分类（页签）。26.1 的注册表在
// `world/item/crafting/RecipeBookCategories.java:7-19`，十三个分类按那里的注册
// 顺序排；页签怎么把它们分组是界面侧的事（`client/.../SearchRecipeBookCategory.java:9-17`
// 与 `CraftingRecipeBookComponent.java:27-33`），不在这一层。
//
// 一条配方落在哪个分类，vanilla 是**数据**不是推导：`CraftingRecipe.java:38-45`
// 把配方 JSON 的 `category` 字段（building/redstone/equipment/misc）switch 成
// RecipeBookCategories 的常量，`SmeltingRecipe.java:41-48` 对 blocks/food/misc
// 做同一件事。本作的配方数据层（data::CraftingRecipeDef）没有这个字段，所以那份
// 数据在 RecipeBookCategoryData.inc 里——**从 vanilla 自己的 recipe JSON 抄出来
// 的一份表**，不是本作跑出来的值，也不是从配方内容猜出来的规则。
//
// 表里没有的标识符落到 Misc：这正是 vanilla codec 的默认值
// （`CraftingRecipe.java:47-49` 的 `mapCodec(..., CraftingBookCategory.MISC, ...)`
// 与 `AbstractCookingRecipe` 的 `CookingBookCategory.MISC`），所以一条 datapack
// 追加的新配方在这里和在 vanilla 里落到同一格。

#include <cstdint>
#include <string_view>

namespace mc::gameplay {

// 顺序照抄 26.1 `RecipeBookCategories.java:7-19` 的注册顺序。★ 后八个分类本作
// 一条配方都产不出来（没有高炉/烟熏炉/切石机/锻造台/营火这五种配方类型），
// 但仍然按 vanilla 的位置占着——补上高炉那天不会把前面七个重新编号。
enum class RecipeBookCategory : std::uint8_t {
    CraftingBuildingBlocks,
    CraftingRedstone,
    CraftingEquipment,
    CraftingMisc,
    FurnaceFood,
    FurnaceBlocks,
    FurnaceMisc,
    BlastFurnaceBlocks,
    BlastFurnaceMisc,
    SmokerFood,
    Stonecutter,
    Smithing,
    Campfire,

    // 哨兵，值等于分类个数。**不过线**、不许被序列化——只给"覆盖了每一个分类"
    // 这类断言用。追加新分类放在它**之前**。
    Count,
};

// 一条合成配方的分类。未登记的标识符（datapack 追加的新配方）落到
// CraftingMisc，与 vanilla codec 的默认值一致。
[[nodiscard]] RecipeBookCategory craftingRecipeBookCategory(std::string_view identifier);

// 一条熔炉配方的分类，未登记的落到 FurnaceMisc（同样是 vanilla 的默认值）。
[[nodiscard]] RecipeBookCategory furnaceRecipeBookCategory(std::string_view identifier);

} // namespace mc::gameplay

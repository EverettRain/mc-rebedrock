#pragma once

// ADV-0：数据包目录名的单一真相源。
//
// 为什么要有这个文件：这些目录名是**外部约定**——它们必须逐字对上 Java 26.1
// 的 `data/<ns>/…` 布局，否则 `ResourceProvider::list` 一条都枚举不到，而且
// **不会报错**：没有文件就是"这个包没带配方"，与"我们把目录名拼错了"在运行期
// 完全同形。本仓已经因此静默失效过两处（配方与方块战利品表，见下）。
//
// 26.1 的注册表元素目录名是**单数**——`Registries.elementsDirPath()`
// （core/registries/Registries.java:325）拿的是注册表 id 的路径段本身，而注册表
// id 是 `minecraft:recipe` / `minecraft:loot_table` / `minecraft:function` /
// `minecraft:advancement`，都是单数。JE 是在 1.21（pack_format 48）那次把
// `recipes`/`loot_tables`/`advancements`/`functions` 全改成单数的。
//
// ★ 要不要同时接受复数（旧版本的拼法）？**不接受。**判断依据：
//   1. 本作对标 26.1 且已与 1.16.1 脱钩（见 wiki 与 docs），我们既不读也不写
//      pack_format < 48 的数据包，复数分支从第一天起就没有真实调用方；
//   2. `list()` 是前缀枚举，两个前缀都扫等于把"目录名拼错"这个缺陷变成
//      **永远抓不到**——本次这两个 bug 正是靠"扫不到"才被发现的；
//   3. 真要支持旧包，正确的位置是 pack.mcmeta 的 `pack_format` 分支（按包的
//      自述版本选布局），不是在每个 loader 里盲扫两遍。
// 这条判断由 datapack_paths_test 的「复数路径必须扫不到」断言钉住。

#include <string_view>

namespace mc::data::pack {

// data/<ns>/recipe/**.json —— 26.1 真实目录，1515 条。
// 曾经写成 "recipes"（RecipeTable.cpp）：零命中，vanilla 数据包的配方一条都没被
// 读进来过。
inline constexpr std::string_view kRecipeDir = "recipe";

// data/<ns>/loot_table/blocks/**.json —— 26.1 真实目录，1085 条。
// 注意 `loot_table` 是注册表名（单数），`blocks` 是它下面的**子目录**（复数），
// 两半的单复数不同是 vanilla 自己的形状，不是笔误。
// 曾经写成 "loot_tables/blocks"（LootTable.cpp）：同样零命中。
inline constexpr std::string_view kBlockLootDir = "loot_table/blocks";

// data/<ns>/loot_table/chests/**.json —— 这一条本来就是对的。
inline constexpr std::string_view kChestLootDir = "loot_table/chests";

// data/<ns>/function/**.mcfunction —— `ServerFunctionLibrary.java:35-38` 的
// `TYPE_KEY = Identifier.withDefaultNamespace("function")` 喂给
// `Registries.elementsDirPath`。曾经写成 "functions"：零命中。
inline constexpr std::string_view kFunctionDir = "function";

// data/<ns>/advancement/**.json —— ADV-1 的成就底座从这里读。
// `Registries.ADVANCEMENT` 的 id 是 `minecraft:advancement`。
inline constexpr std::string_view kAdvancementDir = "advancement";

// 标签目录：`Registries.tagsDirPath()` = `tags/<注册表 id 的路径段>`，所以同样
// 是单数的注册表名。这两条本作原本就写对了（BlockTags.cpp）/写错了
// （FunctionManager.cpp 的 tags/functions）。
inline constexpr std::string_view kBlockTagDir = "tags/block";
inline constexpr std::string_view kFunctionTagDir = "tags/function";

// 编译期绊线：把任何一条改回 1.20 的复数拼法都在这里停下，不用等到运行期
// 「怎么一条都没读到」。datapack_paths_test 另有夹具级断言（单数扫得到、复数
// 扫不到），两者一起才盖住「改常量」与「绕开常量另写字面量」两种改法。
static_assert(kRecipeDir != "recipes", "26.1 的配方目录是单数 recipe");
static_assert(kBlockLootDir != "loot_tables/blocks", "26.1 的战利品表目录是单数 loot_table");
static_assert(kChestLootDir != "loot_tables/chests", "26.1 的战利品表目录是单数 loot_table");
static_assert(kFunctionDir != "functions", "26.1 的函数目录是单数 function");
static_assert(kAdvancementDir != "advancements", "26.1 的成就目录是单数 advancement");
static_assert(kBlockTagDir != "tags/blocks", "26.1 的方块标签目录是 tags/block");
static_assert(kFunctionTagDir != "tags/functions", "26.1 的函数标签目录是 tags/function");

} // namespace mc::data::pack

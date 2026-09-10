// ADV-0a：数据包目录名的护栏。
//
// 这条测试存在的理由是一个已经发生过两次的**静默失效**：loader 用错目录名时
// `ResourceProvider::list` 返回空表，而空表与「这个包没带这类文件」在运行期完全
// 同形——没有异常、没有日志、没有一条测试变红。修完（recipes→recipe、
// loot_tables/blocks→loot_table/blocks、functions→function、
// tags/functions→tags/function）必须留下一条能在有人改回去时变红的断言。
//
// 断言钉的是 **26.1 数据包自己的布局**，不是本作跑出来的值：目录名的出处写在
// src/data/DataPackPaths.hpp 的注释里（Registries.elementsDirPath /
// ServerFunctionLibrary.java:35-38），实测计数在真 26.1 数据包上是
// recipe=1515、loot_table/blocks=1085。
//
// 护栏分三层，缺一层就漏：
//   1. 常量层：DataPackPaths.hpp 里的 static_assert，改常量即编译失败。
//   2. 夹具层（本文件主体）：用 **真目录形状** 的 StandardPackResourceProvider
//      喂每个 loader —— 单数路径下的文件必须被读到，复数路径下的必须**读不到**。
//      ★ 必须用目录型 provider 而不是内存 map：内存 provider 的 list 是字符串
//      前缀匹配，"recipe" 会误命中 "recipes/…"，那样「复数扫不到」这条断言就成
//      了假绿。StandardPackResourceProvider::list 把 prefix 当**目录**看
//      （ResourceProvider.cpp:144-148 的 is_directory 门），与真数据包一致。
//   ★ 函数目录（function/ 与 tags/function/）没有在这里再摆一遍夹具：
//      function_manager_test 本来就是照真目录形状写盘的，改回 functions/ 会让
//      它整片变红。护栏在那边，不在这里重复一份。
//   3. 真包层（可选）：环境变量 MC_REBEDROCK_VANILLA_DATAPACK 指向一份真
//      26.1 数据包时，逐条断言这些目录在那份包里存在。没设就跳过——vanilla 资产
//      不入库（版权铁律），所以它不能是硬前置。

#include "assets/ResourceProvider.hpp"
#include "data/DataPackPaths.hpp"
#include "gameplay/BlockTags.hpp"
#include "gameplay/ChestLootTable.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "gameplay/LootTable.hpp"
#include "gameplay/RecipeTable.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

namespace pack = mc::data::pack;

// 一份一次性的磁盘数据包，形状与真 26.1 数据包相同（<root>/data/<ns>/<dir>/…）。
class TempPack final {
  public:
    TempPack() {
        root_ = std::filesystem::temp_directory_path() /
                ("mc_rebedrock_datapack_paths_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter_++));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }
    TempPack(const TempPack&) = delete;
    TempPack& operator=(const TempPack&) = delete;
    ~TempPack() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    // `relative` 是 data/<ns>/ 之下的路径，例如 "recipe/oak_planks.json"。
    void write(std::string_view space, std::string_view relative, std::string_view body) const {
        const auto file = root_ / "data" / std::string{space} / std::string{relative};
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out{file, std::ios::binary};
        out << body;
    }

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

  private:
    std::filesystem::path root_;
    static inline int counter_ = 0;
};

// 一条能被 RecipeTable 的 codec 读进来的最小配方，替换内置的 oak_planks，
// 把产出改成 7 —— 一个内置表里不会出现的数，所以「读到了」不可能是巧合。
constexpr std::string_view kOakPlanksOverlay =
    R"({"width":1,"height":1,"ingredients":[{"block":"minecraft:oak_log"}],
        "output":"minecraft:oak_planks","count":7})";

[[nodiscard]] const mc::gameplay::CraftingRecipe* findCrafting(
    const mc::gameplay::RecipeTable& table, std::string_view identifier) {
    for (const auto& recipe : table.crafting()) {
        if (recipe.identifier == identifier) return &recipe;
    }
    return nullptr;
}

// 1. 配方：单数 recipe/ 下的覆盖生效，复数 recipes/ 下的不生效。
void testRecipeDirectory() {
    {
        TempPack pack;
        pack.write("minecraft", std::string{pack::kRecipeDir} + "/oak_planks.json",
                   kOakPlanksOverlay);
        mc::assets::StandardPackResourceProvider provider{pack.root()};
        mc::gameplay::RecipeTable table;
        table.load(provider);
        const auto* planks = findCrafting(table, "rebedrock:oak_planks");
        assert(planks != nullptr);
        // 26.1 的 data/minecraft/recipe/ 是这个目录名；读到了就是 7。
        assert(planks->output.count == 7U);
    }
    {
        // 同一份文件放在 1.20 的复数目录下：**必须**读不到（我们不做单复数兼容，
        // 理由写在 DataPackPaths.hpp）。这条断言就是「有人把 recipe 改回
        // recipes」时变红的那一条：改回去以后这里会读到 7。
        TempPack pack;
        pack.write("minecraft", "recipes/oak_planks.json", kOakPlanksOverlay);
        mc::assets::StandardPackResourceProvider provider{pack.root()};
        mc::gameplay::RecipeTable table;
        table.load(provider);
        const auto* planks = findCrafting(table, "rebedrock:oak_planks");
        assert(planks != nullptr);
        assert(planks->output.count != 7U); // 内置底座的 4，覆盖没生效
    }
}

// 2. 方块战利品表：单数 loot_table/blocks/ 生效，复数 loot_tables/blocks/ 不生效。
void testBlockLootDirectory() {
    // 内置底座里石头掉的是圆石，所以「石头掉钻石」只可能来自覆盖。
    constexpr std::string_view kStoneDropsDiamond =
        R"({"drops":[{"id":"minecraft:diamond","count":1}]})";
    {
        TempPack pack;
        pack.write("minecraft", std::string{pack::kBlockLootDir} + "/stone.json",
                   kStoneDropsDiamond);
        mc::assets::StandardPackResourceProvider provider{pack.root()};
        mc::gameplay::LootTable table;
        table.load(provider);
        const auto* stone = table.find(mc::world::Block::Stone);
        assert(stone != nullptr && !stone->stacks.empty());
        assert(stone->stacks[0].item == &mc::gameplay::items::Diamond);
    }
    {
        TempPack pack;
        pack.write("minecraft", "loot_tables/blocks/stone.json", kStoneDropsDiamond);
        mc::assets::StandardPackResourceProvider provider{pack.root()};
        mc::gameplay::LootTable table;
        table.load(provider);
        const auto* stone = table.find(mc::world::Block::Stone);
        assert(stone != nullptr && !stone->stacks.empty());
        // 覆盖没生效：还是底座的圆石。
        assert(stone->stacks[0].block == mc::world::Block::Cobblestone);
    }
}

// 3. 箱子战利品表：单数 loot_table/chests/ 生效，复数不生效。
void testChestLootDirectory() {
    constexpr std::string_view kChestTable =
        R"({"pools":[{"rolls":1,"entries":[
            {"type":"minecraft:item","name":"minecraft:diamond"}]}]})";
    {
        TempPack pack;
        pack.write("minecraft", std::string{pack::kChestLootDir} + "/adv_probe.json", kChestTable);
        mc::assets::StandardPackResourceProvider provider{pack.root()};
        mc::gameplay::ChestLootTable table;
        table.load(provider);
        assert(table.find("minecraft:chests/adv_probe") != nullptr);
    }
    {
        TempPack pack;
        pack.write("minecraft", "loot_tables/chests/adv_probe.json", kChestTable);
        mc::assets::StandardPackResourceProvider provider{pack.root()};
        mc::gameplay::ChestLootTable table;
        table.load(provider);
        assert(table.find("minecraft:chests/adv_probe") == nullptr);
    }
}

// 4. 方块标签：tags/block/（本来就对的那条，钉住别被"顺手统一成复数"改坏）。
// `dataDriven` 直接回答「这个标签是包供的还是内置底座的」，正是这条护栏要问的。
void testBlockTagDirectory() {
    constexpr std::string_view kLeavesOnlyGlass =
        R"({"replace":true,"values":["minecraft:glass"]})";
    {
        TempPack pack;
        pack.write("minecraft", std::string{pack::kBlockTagDir} + "/leaves.json", kLeavesOnlyGlass);
        mc::assets::StandardPackResourceProvider provider{pack.root()};
        mc::gameplay::BlockTagTable tags;
        tags.load(provider);
        assert(tags.dataDriven(mc::gameplay::BlockTag::Leaves));
        assert(tags.has(mc::world::Block::Glass, mc::gameplay::BlockTag::Leaves));
    }
    {
        TempPack pack;
        pack.write("minecraft", "tags/blocks/leaves.json", kLeavesOnlyGlass);
        mc::assets::StandardPackResourceProvider provider{pack.root()};
        mc::gameplay::BlockTagTable tags;
        tags.load(provider);
        assert(!tags.dataDriven(mc::gameplay::BlockTag::Leaves));
        assert(!tags.has(mc::world::Block::Glass, mc::gameplay::BlockTag::Leaves));
    }
}

// 5. 可选层：真 26.1 数据包在手边时，逐条断言目录真的存在。
// 没设环境变量就跳过——vanilla 数据不入库，这不能是硬前置。
void testAgainstRealDataPackIfPresent() {
    const char* const rootEnvironment = std::getenv("MC_REBEDROCK_VANILLA_DATAPACK");
    if (rootEnvironment == nullptr || *rootEnvironment == '\0') {
        return;
    }
    const std::filesystem::path root{rootEnvironment};
    const std::filesystem::path namespaceRoot = root / "data" / "minecraft";
    if (!std::filesystem::is_directory(namespaceRoot)) {
        return;
    }
    for (const std::string_view directory :
         {pack::kRecipeDir, pack::kBlockLootDir, pack::kChestLootDir, pack::kAdvancementDir,
          pack::kBlockTagDir}) {
        assert(std::filesystem::is_directory(namespaceRoot / std::string{directory}));
    }
    // 反向：1.20 的复数拼法在 26.1 的包里**不存在**。
    for (const std::string_view directory :
         {"recipes", "loot_tables", "advancements", "functions", "tags/blocks"}) {
        assert(!std::filesystem::is_directory(namespaceRoot / std::string{directory}));
    }
}

} // namespace

int main() {
    testRecipeDirectory();
    testBlockLootDirectory();
    testChestLootDirectory();
    testBlockTagDirectory();
    testAgainstRealDataPackIfPresent();
    return 0;
}

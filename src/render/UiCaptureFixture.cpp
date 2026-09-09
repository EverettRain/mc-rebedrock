#include "render/UiCaptureFixture.hpp"

#include "gameplay/Enchantment.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/GameMode.hpp"
#include "render/WorldIcon.hpp"
#include "world/Block.hpp"

#include <cstdint>

namespace mc::render {
namespace {

using gameplay::ItemStack;

// 一个带附魔的物品堆。附魔以原始存储值入栈（ItemStack 刻意不包含 Enchantment.hpp，
// 见它自己的注释），这里是那次转换的唯一一处。
[[nodiscard]] ItemStack enchanted(ItemStack stack, gameplay::EnchantmentId id,
                                  std::uint8_t level) {
    stack.enchantments[0] = {static_cast<gameplay::EnchantmentIdStorage>(id), level};
    stack.enchantmentCount = 1U;
    return stack;
}

[[nodiscard]] ItemStack tool(const gameplay::Item& item, std::uint16_t damage) {
    ItemStack stack{world::Block::Air, 1U, &item};
    stack.damage = damage;
    return stack;
}

[[nodiscard]] ItemStack blocks(world::Block block, std::uint8_t count) {
    return ItemStack{block, count};
}

[[nodiscard]] ItemStack items(const gameplay::Item& item, std::uint8_t count) {
    return ItemStack{world::Block::Air, count, &item};
}

// 玩家自己那 36 格。每一屏都画它（箱子屏画在下半，创造背包画在自己的面板上），
// 所以它是唯一一份、不随目标变化的内容。
//
// 摆法覆盖三条绘制路径：0..8 是快捷栏（HUD 上也画一遍）、9..26 是主背包、
// 27..35 留空——**空格子也要有**，否则"空槽画成什么样"这条路径一张图都没拍到。
void fillPlayerInventory(gameplay::WorldSnapshot& snapshot) {
    auto& slots = snapshot.inventorySlots;
    slots[0] = tool(gameplay::items::DiamondPickaxe, 120U);
    slots[1] = enchanted(items(gameplay::items::IronSword, 1U),
                         gameplay::EnchantmentId::Sharpness, 3U);
    slots[2] = blocks(world::Block::Cobblestone, 64U);
    slots[3] = blocks(world::Block::OakPlanks, 32U);
    slots[4] = items(gameplay::items::Bread, 5U);
    slots[5] = items(gameplay::items::Coal, 12U);
    slots[6] = blocks(world::Block::Torch, 9U);
    slots[7] = items(gameplay::items::IronIngot, 7U);
    slots[8] = items(gameplay::items::Apple, 1U);
    slots[9] = blocks(world::Block::Dirt, 64U);
    slots[10] = blocks(world::Block::Stone, 48U);
    slots[11] = items(gameplay::items::Stick, 16U);
    slots[12] = tool(gameplay::items::IronAxe, 30U);
    slots[13] = items(gameplay::items::LapisLazuli, 3U);
    slots[14] = blocks(world::Block::Bricks, 21U);
    slots[18] = items(gameplay::items::Diamond, 2U);
    slots[19] = items(gameplay::items::Emerald, 1U);
    slots[26] = blocks(world::Block::OakLog, 10U);

    // 五个装备槽。下标是 gameplay::EquipmentSlot 的底层值（Offhand=0 … Head=4），
    // 与绘制侧 `equipmentSlots[static_cast<std::size_t>(equipmentSlotAt(index))]` 同一份。
    auto& worn = snapshot.equipmentSlots;
    worn[static_cast<std::size_t>(gameplay::EquipmentSlot::Head)] =
        enchanted(items(gameplay::items::IronHelmet, 1U),
                  gameplay::EnchantmentId::Protection, 2U);
    worn[static_cast<std::size_t>(gameplay::EquipmentSlot::Chest)] =
        items(gameplay::items::IronChestplate, 1U);
    worn[static_cast<std::size_t>(gameplay::EquipmentSlot::Legs)] =
        tool(gameplay::items::LeatherLeggings, 12U);
    worn[static_cast<std::size_t>(gameplay::EquipmentSlot::Feet)] =
        items(gameplay::items::IronBoots, 1U);
    worn[static_cast<std::size_t>(gameplay::EquipmentSlot::Offhand)] =
        blocks(world::Block::Torch, 16U);
}

} // namespace

gameplay::WorldSnapshot uiCaptureWorldSnapshot(const UiCaptureTarget& target,
                                              bool carryStack) {
    gameplay::WorldSnapshot snapshot;
    if (!target.container.has_value()) {
        // UI-8 / D26：**世界页也要有内容**。`game / pause / death` 三页画的是游戏内
        // HUD，而 A0-0 只给容器目标配了夹具——那三张图里玩家空血、空饥饿、空手，
        // 快捷栏一件东西都没有。一张不像游戏的 HUD 对照 26.1 时没有参考价值。
        //
        // ★ 判据是 `uiCapturePageShowsWorld`（世界画面看得见吗），不是"需不需要夹具"：
        //   `loading` 属于世界会话但画的是全景加一行进度，HUD 根本不可见，给它内容
        //   只会让一张看不见的东西参与比对。这两个谓词的差集就是 loading，
        //   而那正是它们分成两个函数的理由。
        if (uiCapturePageShowsWorld(target.page)) {
            fillPlayerInventory(snapshot);
        }
        // 前端页面（title / options / …）仍然拿一份**默认**快照：既有基线不变。
        return snapshot;
    }
    snapshot.openContainerScreen = *target.container;
    fillPlayerInventory(snapshot);
    if (carryStack) {
        // 光标上那一堆。画在**光标位置**，所以光标钉在画布外时它一个像素都不占——
        // 这是常规基线不受影响的原因。带 `--ui-cursor` 把光标钉进画布，它才现身，
        // 同时按 vanilla 的规则把提示框压住。
        snapshot.cursorStack = enchanted(items(gameplay::items::DiamondSword, 1U),
                                         gameplay::EnchantmentId::Sharpness, 5U);
    }

    switch (*target.container) {
    case gameplay::ContainerScreen::PlayerInventory:
        // 2x2 合成格只在生存屏存在（创造屏没有合成），所以它跟着 creative 走——
        // 与 ScreenHandler::appendContainerSlots 里那条同样的判断同源。
        if (!target.creative) {
            snapshot.playerCraftingGrid[0] = blocks(world::Block::OakPlanks, 1U);
            snapshot.playerCraftingGrid[1] = blocks(world::Block::OakPlanks, 1U);
            snapshot.playerCraftingGrid[2] = blocks(world::Block::OakPlanks, 1U);
            snapshot.playerCraftingGrid[3] = blocks(world::Block::OakPlanks, 1U);
            snapshot.playerCraftingOutput = blocks(world::Block::CraftingTable, 1U);
        }
        break;
    case gameplay::ContainerScreen::CraftingTable:
        // 一张 3x3 的镐子配方：上排三块石头，中列两根木棍。留白的四格是有意的——
        // 满格的网格看不出格子的边界。
        snapshot.tableCraftingGrid[0] = blocks(world::Block::Cobblestone, 1U);
        snapshot.tableCraftingGrid[1] = blocks(world::Block::Cobblestone, 1U);
        snapshot.tableCraftingGrid[2] = blocks(world::Block::Cobblestone, 1U);
        snapshot.tableCraftingGrid[4] = items(gameplay::items::Stick, 1U);
        snapshot.tableCraftingGrid[7] = items(gameplay::items::Stick, 1U);
        snapshot.tableCraftingOutput = items(gameplay::items::StonePickaxe, 1U);
        break;
    case gameplay::ContainerScreen::Furnace:
        snapshot.openFurnace = glm::ivec3{2, 64, 2};
        snapshot.furnaceInput = blocks(world::Block::IronOre, 8U);
        snapshot.furnaceFuel = items(gameplay::items::Coal, 4U);
        snapshot.furnaceOutput = items(gameplay::items::IronIngot, 3U);
        // 两条进度都取**非零且非满**：0 与 1 各自会走进"整条不画"或"整条画满"的
        // 退化分支，而中间值才画得出那两张部分裁切的精灵。
        snapshot.furnaceFuelProgress = 0.6F;
        snapshot.furnaceCookProgress = 0.35F;
        break;
    case gameplay::ContainerScreen::Chest: {
        snapshot.openChest = gameplay::ChestPosition{4, 64, 4};
        auto& chest = snapshot.chestItems;
        chest[0] = blocks(world::Block::Stone, 64U);
        chest[1] = blocks(world::Block::Dirt, 17U);
        chest[2] = items(gameplay::items::Coal, 33U);
        chest[4] = tool(gameplay::items::IronPickaxe, 60U);
        chest[8] = items(gameplay::items::Bone, 6U);
        chest[9] = items(gameplay::items::Feather, 24U);
        chest[13] = enchanted(items(gameplay::items::EnchantedBook, 1U),
                              gameplay::EnchantmentId::Efficiency, 4U);
        chest[18] = blocks(world::Block::Bricks, 5U);
        chest[26] = items(gameplay::items::GoldIngot, 2U);
        break;
    }
    case gameplay::ContainerScreen::EnchantingTable:
        snapshot.enchantingItem = items(gameplay::items::DiamondPickaxe, 1U);
        snapshot.enchantingLapis = items(gameplay::items::LapisLazuli, 3U);
        // 三条选项条：一条便宜、一条中等、一条贵。三条都非零，因为 0 是"死条"那一档，
        // 只拍死条等于没拍这一屏的主体。
        snapshot.enchantingRequiredLevels = {5, 12, 30};
        snapshot.enchantingClueIds = {
            static_cast<std::uint8_t>(gameplay::EnchantmentId::Efficiency),
            static_cast<std::uint8_t>(gameplay::EnchantmentId::Unbreaking),
            static_cast<std::uint8_t>(gameplay::EnchantmentId::Fortune),
        };
        snapshot.enchantingClueLevels = {1U, 2U, 3U};
        snapshot.enchantingBookshelfPower = 15;
        // 那串"标准银河字母"的乱码名是这个种子的函数。钉住它，图才可复现。
        snapshot.enchantingSeed = 20260908;
        break;
    case gameplay::ContainerScreen::Anvil:
        snapshot.anvilLeft = tool(gameplay::items::DiamondPickaxe, 200U);
        snapshot.anvilRight = enchanted(items(gameplay::items::EnchantedBook, 1U),
                                        gameplay::EnchantmentId::Unbreaking, 3U);
        snapshot.anvilResult = enchanted(tool(gameplay::items::DiamondPickaxe, 0U),
                                         gameplay::EnchantmentId::Unbreaking, 3U);
        snapshot.anvilCost = 7;
        break;
    case gameplay::ContainerScreen::Count:
        // 哨兵，不是一屏。它不会出现在目标表里（那张表的覆盖断言只遍历 Count 之前的值）。
        break;
    }
    return snapshot;
}

gameplay::PlayerTickSnapshot uiCapturePlayerSnapshot(const UiCaptureTarget& target) {
    gameplay::PlayerTickSnapshot snapshot;
    if (!target.container.has_value() && !uiCapturePageShowsWorld(target.page)) {
        // ★ **前端页面**（看不见世界的那些）一律拿默认值，这是它们的基线逐字节不变的
        //   保证，而且是构造上的保证、不是"应该不会变"：`ui::UiFrameData` 的每一个
        //   默认值与默认 `PlayerTickSnapshot` 的对应字段逐个相等（health 0 / food 0 /
        //   air 0 / ticksSinceDamage 1000 / 经验 0 / Survival / eating false / 空手 /
        //   快捷栏第 0 格），所以"从一份默认快照同步一次"与"从不同步"结果相同。
        //   UI-8 / D26 把**世界页**移出了这一支：它们画的是游戏内 HUD，空的没有意义。
        return snapshot;
    }
    snapshot.gameMode =
        target.creative ? gameplay::GameMode::Creative : gameplay::GameMode::Survival;
    // 生存状态条画在背包屏底下的 HUD 上。取的是**非满**值：满血满饥饿只画得出
    // "整排实心图标"这一档，半颗心与半块肉那两张精灵一张都进不了画。
    snapshot.health = 15.0F;
    snapshot.foodLevel = 13;
    snapshot.airTicks = 300;
    snapshot.ticksSinceDamage = 1000;
    snapshot.experienceLevel = 27;
    snapshot.experienceProgress = 0.4F;
    snapshot.selectedHotbarSlot = 2U;
    return snapshot;
}

namespace {

// 这三屏都读 `menuSystem.saveSummaries`：列表画行，编辑与删除确认画选中存档的名字。
[[nodiscard]] bool targetShowsSaveList(const UiCaptureTarget& target) {
    return target.page == ui::PageId::WorldList || target.page == ui::PageId::EditWorld ||
           target.page == ui::PageId::ConfirmDelete;
}

} // namespace

std::vector<persistence::SaveSummary> uiCaptureSaveSummaries(const UiCaptureTarget& target) {
    if (!targetShowsSaveList(target)) {
        return {};
    }
    // ★ 逐字段赋值而不是聚合初始化：`SaveSummary` 是持久化层的结构，随时会加字段，
    //   而聚合初始化在加字段时**不会报错**，只会让后面每一个值都错位一格。
    const auto save = [](std::string identifier, std::string displayName, std::uint64_t seed,
                         std::int64_t lastPlayed, gameplay::GameMode mode) {
        persistence::SaveSummary summary;
        summary.identifier = std::move(identifier);
        summary.displayName = std::move(displayName);
        summary.seed = seed;
        summary.lastPlayedUnixSeconds = lastPlayed;
        summary.gameMode = mode;
        // ★ 写死的字面量，**故意不取 `core::kVersion.name`**：夹具的全部意义是
        //   "只由目标决定"，而版本名每次发布都会变——取真值等于让每一次版本号变更
        //   都把这一屏的基线改掉。
        summary.versionName = "26.1";
        return summary;
    };
    std::vector<persistence::SaveSummary> saves;
    // 2024-01-02 03:04:05 UTC。截图通道把 TZ 钉成 UTC（applyUiCaptureDeterminism），
    // 所以这个数在任何机器上都渲染成同一串字。
    saves.push_back(save("new-world", "New World", 1234567890123ULL, 1704164645,
                         gameplay::GameMode::Survival));
    // 有自己的缩略图的那一支：另外两行走 26.1 的回落图标，一张图里两条路径都在。
    saves.front().hasIcon = true;
    // 没有"最后游玩"记录的那一支：第二行只有目录名，没有括号里的日期。
    // 顺带换一个游戏模式——第三行的模式名是两条不同的译文。
    saves.push_back(save("flat-testbed", "Flat Testbed", 0ULL, 0, gameplay::GameMode::Creative));
    // 长到要被裁的那一支（231 逻辑像素放不下）。
    saves.push_back(save("very-long-directory-name-for-clipping",
                         "A World Whose Name Is Far Too Long To Fit In One Row", 42ULL,
                         1704164645, gameplay::GameMode::Survival));
    return saves;
}

std::vector<std::uint8_t> uiCaptureWorldIcon() {
    // 一张 160x90 的合成"帧"：横向红、纵向绿、蓝恒定。裁剪窗口取的是中间那 90 列
    // （`GameRenderer.takeAutoScreenshot`：宽 > 高 时 x = (160-90)/2 = 35），
    // 所以画出来的图标左缘不是纯黑——那正好证明**裁的是中间**而不是从 0 开始。
    constexpr int kWidth = 160;
    constexpr int kHeight = 90;
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(kWidth * kHeight * 4));
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const auto offset = static_cast<std::size_t>((y * kWidth + x) * 4);
            frame[offset + 0U] = static_cast<std::uint8_t>(x * 255 / (kWidth - 1));
            frame[offset + 1U] = static_cast<std::uint8_t>(y * 255 / (kHeight - 1));
            frame[offset + 2U] = 96U;
            frame[offset + 3U] = 255U;
        }
    }
    return worldIconFromFrame(frame, kWidth, kHeight);
}

std::size_t uiCaptureSelectedWorldRow(const UiCaptureTarget& target) {
    if (!targetShowsSaveList(target)) {
        return static_cast<std::size_t>(-1);
    }
    // 第 1 行。选中的行有底、没选中的没有——一张图里两种都要有。
    return 1U;
}

} // namespace mc::render

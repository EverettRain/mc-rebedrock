#pragma once

// A1：「这个槽里现在是什么」——从世界快照按槽位身份取物品堆。
//
// ## 它收口的是什么
//
// 这个问题此前在**三处**各答一遍：
//   1. `VulkanRenderer::snapshotStackAt`（拖拽预览与落位数量）；
//   2. `HudRenderer::drawWorkContainer` 的 if/else 链（每一屏各自 `snap.chestItems[i]`
//      / `snap.tableCraftingGrid[i]` / `snap.furnaceInput` …）；
//   3. `drawHud` 里那段内联的生存背包，以及 `drawCreativeInventory`。
//
// 三份表述的差别不会让任何东西编译不过，只会让"拖拽预览显示的东西"与"槽位画出来的
// 东西"在某一格上不一致——而那种缺陷没有任何断言会红（README 护栏 18）。
//
// ★ 它是一个**纯函数**（快照 + 身份 → 物品堆），因此可以无头测试；而绘制侧遍历容器页
//   画槽位时，"取哪一格"这件事也就只有这一处。
//
// ## 两个刻意的取舍
//
// ★ **输出槽返回快照里的真值**（合成结果、熔炉产物、铁砧结果），而不是空堆。
//   `snapshotStackAt` 从前对两个合成输出返回空堆，理由是"输出槽不是拖拽目标，
//   预览不会来问它"——那对拖拽成立（`dragSlotAt` 按 `acceptsItems()` 过滤掉了输出槽），
//   但对**绘制**不成立：那一格是要画出来的。返回真值对两个消费者都正确。
//
// ★ **创造目录格不在这里**。那 45 格的内容来自 `gameplay::creativeCatalog` 那张只读
//   清单加上当前滚动行，根本不在世界快照里（见 `SlotKind::CreativeCatalog`）。
//   这个函数对它返回空堆，由绘制侧自己从目录里取。

#include "gameplay/EquipmentSlot.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/ScreenTypes.hpp"
#include "gameplay/WorldSnapshot.hpp"

#include <cstdint>

namespace mc::gameplay {

[[nodiscard]] inline const ItemStack& snapshotSlotStack(const WorldSnapshot& snapshot,
                                                        SlotKind kind, std::uint16_t index) {
    // 越界一律回空堆。下标来自槽位表，正常路径上不会越界；但"不会越界"这件事不该
    // 由调用方逐个保证——一次越界读的是相邻字段，画出来是一格随机物品。
    static const ItemStack kEmpty;
    switch (kind) {
    case SlotKind::PlayerInventory:
        return index < Inventory::kSlotCount ? snapshot.inventorySlots[index] : kEmpty;
    case SlotKind::ChestStorage:
        return index < ChestBlockEntity::kSlotCount ? snapshot.chestItems[index] : kEmpty;
    case SlotKind::TableCraftingGrid:
        return index < snapshot.tableCraftingGrid.size() ? snapshot.tableCraftingGrid[index]
                                                         : kEmpty;
    case SlotKind::TableCraftingOutput:
        return snapshot.tableCraftingOutput;
    case SlotKind::PlayerCraftingGrid:
        return index < snapshot.playerCraftingGrid.size() ? snapshot.playerCraftingGrid[index]
                                                          : kEmpty;
    case SlotKind::PlayerCraftingOutput:
        return snapshot.playerCraftingOutput;
    case SlotKind::FurnaceInput:  return snapshot.furnaceInput;
    case SlotKind::FurnaceFuel:   return snapshot.furnaceFuel;
    case SlotKind::FurnaceOutput: return snapshot.furnaceOutput;
    case SlotKind::EnchantingItem:  return snapshot.enchantingItem;
    case SlotKind::EnchantingLapis: return snapshot.enchantingLapis;
    // AR-M6: the merchant menu's three slots, read from the snapshot the same
    // way — including the result, which is derived on the simulation side and
    // published like any other stack.
    case SlotKind::TradePaymentA:   return snapshot.tradePaymentA;
    case SlotKind::TradePaymentB:   return snapshot.tradePaymentB;
    case SlotKind::TradeResult:     return snapshot.tradeResult;
    case SlotKind::AnvilLeft:   return snapshot.anvilLeft;
    case SlotKind::AnvilRight:  return snapshot.anvilRight;
    case SlotKind::AnvilOutput: return snapshot.anvilResult;
    case SlotKind::Equipment:
        // ★ `equipmentSlots` 按 **EquipmentSlot 的底层值**索引（Offhand=0 … Head=4），
        //   而 `index` 是**界面自己的绘制顺序**（0..3 = 头/胸/腿/脚，4 = 副手）。
        //   拿绘制序号直接索引那个数组，四件护甲会上下颠倒。
        return index < kEquipmentScreenSlotCount
                   ? snapshot.equipmentSlots[static_cast<std::size_t>(equipmentSlotAt(index))]
                   : kEmpty;
    case SlotKind::CreativeCatalog:
        // 无限货架，不在快照里——由绘制侧从目录清单取（见文件头）。
        return kEmpty;
    case SlotKind::Count:
        return kEmpty;   // 哨兵，不是一种槽
    }
    return kEmpty;
}

} // namespace mc::gameplay

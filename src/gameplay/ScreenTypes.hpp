#pragma once

// 界面身份的枚举，从 ScreenHandler.hpp 抽出来单独成篇。
//
// 抽出的理由有两条，都不是整理癖：
//   1. `ContainerScreen` 有**两类**消费者。一类要连带整套容器逻辑（ScreenHandler 的
//      槽位表与点击路由），另一类只要"是哪一屏"这个身份——截图通道的目标表就是后者。
//      让后者去包含 ScreenHandler.hpp，等于顺带拖进 ChestSystem / FurnaceSystem /
//      Inventory / Equipment / HudLayout 五个头，而它一个都用不上。
//   2. 它缺一个 `Count` 哨兵。凡是"覆盖了每一个取值"的断言，写"当时的最后一个枚举值"
//      都会在追加一个值时**静默通过**（README 护栏 25：`PageId` 就这么栽过一次）。
//      哨兵放在枚举自己身边，加值的人才看得见。
//
// ★ 这个枚举**过线**：它同时在世界快照与开容器事件里传输（`WorldSnapshot.hpp` 的
//   `openContainerScreen`、`GameEvents.hpp` 的开容器事件）。所以新值**只能追加在
//   `Count` 之前的尾部**，插入会让一个正在运行的客户端把 ChestStorage 认成别的屏。

#include "gameplay/EquipmentSlot.hpp"

#include <cstddef>
#include <cstdint>

namespace mc::gameplay {

// Which screen the player has open. This used to live in the renderer's
// HudTypes because the renderer was the only thing that knew about screens;
// the routing below is gameplay, so the enum belongs here and the renderer
// aliases it.
enum class ContainerScreen : std::uint8_t {
    PlayerInventory,
    CraftingTable,
    Furnace,
    Chest,
    // ENCH-2. Appended at the tail: this enum crosses the client/server wire in
    // the snapshot and the open-container event, so an inserted value would
    // renumber the four screens a running client already knows.
    EnchantingTable,
    // ENCH-3. Appended at the tail for the same reason: this enum crosses the
    // wire.
    Anvil,
    // AR-M6: the villager trade screen. Appended at the tail for the same
    // reason as the two above — this enum crosses the wire.
    Trading,

    // 哨兵，值等于 ContainerScreen 的个数。**不过线**，也不许被序列化或发布——
    // 它只给"覆盖了每一屏"这类编译期断言用。追加新屏放在它**之前**。
    Count,
};

// What a slot is, which is all the click router needs to know. 26.1 expresses
// the same thing by subclassing Slot (ResultSlot, FurnaceFuelSlot, …) and
// overriding mayPlace/onTake; a Kind plus a flag covers every distinction the
// screens in this game actually make, without a virtual call per slot per
// frame.
enum class SlotKind : std::uint8_t {
    // The player's own 36 slots, wherever they are drawn.
    PlayerInventory,
    // A crafting grid cell — the 2x2 in the player screen or the 3x3 in a table.
    PlayerCraftingGrid,
    TableCraftingGrid,
    // A crafting result. Never accepts items: clicking takes the craft.
    PlayerCraftingOutput,
    TableCraftingOutput,
    FurnaceInput,
    FurnaceFuel,
    // The smelted result. Like a crafting output, it only ever gives.
    FurnaceOutput,
    ChestStorage,
    // ENCH-2: the enchanting table's two inputs. Neither has a block entity
    // behind it — both live on the player's own EnchantingMenu, which is why
    // they are their own kinds rather than a reuse of the furnace's.
    EnchantingItem,
    EnchantingLapis,
    // ENCH-3: the anvil's two inputs and its output. Like the enchanting
    // table's, they live on the player's own menu, not a block entity. The
    // output never accepts an item — taking it is what pays the levels.
    AnvilLeft,
    AnvilRight,
    AnvilOutput,
    // EQ-1: one of the player's five equipment slots. `index` is the screen's
    // own draw order (0..3 = Head/Chest/Legs/Feet, 4 = Offhand — see
    // equipmentSlotAt below), not gameplay::EquipmentSlot's underlying value;
    // the click router converts.
    Equipment,
    // A0：创造目录那 45 格（9 列 x 5 行）。★ 它们背后**没有存储**——玩家格背后是
    // `Inventory::mutableSlot`、箱子格背后是方块实体，而目录格是一张**无限货架**。
    // 所以点它发的是 `ClickCreativeItem`（命令自带物品堆，因为服务端并不知道客户端
    // 滚到第几行、开着哪个页签），不是 `ClickSlot`；`resolveSlotStorage` 对它显式返回
    // nullptr，而它也**不进** `buildSlots`（有存储的那一版）——真进去了 `click()`
    // 会把货架当成真槽位。
    //
    // A0 之前它们不是任何一种 SlotKind：绘制侧自己一段循环画、交互侧自己另一段循环
    // 命中，两段都在 Widget 模型之外，所以菜单侧攒下的护栏对那 45 格一条都不生效。
    //
    // 追加在尾部：这个枚举**过线**（`ClickSlot.kind`），插入会让运行中的客户端把
    // ChestStorage 认成别的。
    CreativeCatalog,
    // AR-M6: the merchant menu's two payment slots and its result. Like the
    // enchanting table's and the anvil's, they live on the player's own
    // TradingMenu rather than behind a block entity — the "container" here is
    // a villager, and it holds no items of its own. The result never accepts an
    // item: taking it IS the trade, exactly as MerchantResultSlot#onTake is.
    TradePaymentA,
    TradePaymentB,
    TradeResult,

    // 哨兵，值等于 SlotKind 的个数。**不过线**、不许被序列化——只给"覆盖了每一种槽"
    // 这类编译期与测试断言用。追加新槽放在它**之前**。
    Count,
};

// 这种槽收不收东西。输出槽（合成结果、熔炉产物、铁砧结果）只给不收，
// QUICK_CRAFT 拖拽也跳过它们；创造目录格是无限货架，同样不是拖拽目标。
//
// ★ A2 抽成按 kind 的自由函数：`SlotView::acceptsItems()` 与界面侧的 `ui::Widget`
//   要问的是同一个问题，而 Widget 上没有 SlotView。两处各写一遍就是同一事实的
//   两份表述，而漏改的症状是"这一格拖拽预览画了、松手却什么都没发生"。
[[nodiscard]] constexpr bool slotAcceptsItems(SlotKind kind) {
    switch (kind) {
    case SlotKind::PlayerCraftingOutput:
    case SlotKind::TableCraftingOutput:
    case SlotKind::FurnaceOutput:
    case SlotKind::AnvilOutput:
    // AR-M6: taking the result IS the trade; it never takes an item.
    case SlotKind::TradeResult:
    case SlotKind::CreativeCatalog:
    case SlotKind::Count:
        return false;
    case SlotKind::PlayerInventory:
    case SlotKind::PlayerCraftingGrid:
    case SlotKind::TableCraftingGrid:
    case SlotKind::FurnaceInput:
    case SlotKind::FurnaceFuel:
    case SlotKind::ChestStorage:
    case SlotKind::EnchantingItem:
    case SlotKind::EnchantingLapis:
    case SlotKind::AnvilLeft:
    case SlotKind::AnvilRight:
    case SlotKind::TradePaymentA:
    case SlotKind::TradePaymentB:
    case SlotKind::Equipment:
        return true;
    }
    return false;
}

// EQ-1: the screen's armor-slot draw order (0..3 = Head/Chest/Legs/Feet, the
// GUI spec §10 top-to-bottom layout) plus offhand at 4, mapped to the
// gameplay::EquipmentSlot each index addresses. A SlotKind::Equipment index
// outside 0..4 has no slot; callers guard with the count below first.
inline constexpr std::size_t kEquipmentScreenSlotCount = 5U;

[[nodiscard]] constexpr EquipmentSlot equipmentSlotAt(std::size_t screenIndex) {
    switch (screenIndex) {
    case 0U: return EquipmentSlot::Head;
    case 1U: return EquipmentSlot::Chest;
    case 2U: return EquipmentSlot::Legs;
    case 3U: return EquipmentSlot::Feet;
    case 4U: return EquipmentSlot::Offhand;
    default: return EquipmentSlot::Offhand;
    }
}

} // namespace mc::gameplay

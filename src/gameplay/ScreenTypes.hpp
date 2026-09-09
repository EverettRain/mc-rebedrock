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

    // 哨兵，值等于 ContainerScreen 的个数。**不过线**，也不许被序列化或发布——
    // 它只给"覆盖了每一屏"这类编译期断言用。追加新屏放在它**之前**。
    Count,
};

} // namespace mc::gameplay

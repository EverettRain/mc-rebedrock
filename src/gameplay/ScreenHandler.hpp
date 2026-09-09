#pragma once

#include "gameplay/ChestSystem.hpp"
#include "gameplay/Equipment.hpp"
#include "gameplay/FurnaceSystem.hpp"
#include "gameplay/GameMode.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/ScreenTypes.hpp"
#include "ui/HudLayout.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace mc::gameplay {

class GameSession;

// `ContainerScreen`（这一屏是什么）住在 gameplay/ScreenTypes.hpp：它有一类只要身份、
// 不要整套容器逻辑的消费者（截图通道的目标表），而那里也是它的 `Count` 哨兵的家。

// `SlotKind`（这是个什么槽）与 `ContainerScreen` 一样住在 gameplay/ScreenTypes.hpp：
// 界面侧的 `ui::Widget` 只要这一个枚举，不要整套容器逻辑。

// One slot on the open screen: where it is, what it is, and the exact storage
// behind it.
//
// ★ `storage` **不是拖拽身份**。这里原来写的是"拖拽靠指针相等认它"，而实现从来
//   不是那样：跨帧身份是纯值 `gameplay::SlotRef`（kind + index），而渲染线程用的
//   `buildSlotLayout` **刻意把每个 storage 置空**，好让命中测试与拖拽预览够不着
//   模拟线程拥有的背包内存。那句注释是过期遗留物，连同它描述的
//   `slotForStorage`（生产代码 0 调用）一起删掉了（A0）。
//
// `storage` 只是"这个槽背后的那块存储"，给点击路由用；两个输出槽为空，它们在合成
// 发生之前没有自己的存储。
struct SlotView final {
    ui::UiRect rect;
    ItemStack* storage = nullptr;
    SlotKind kind = SlotKind::PlayerInventory;
    std::uint16_t index = 0U;

    // Output slots are not drag targets, and QUICK_CRAFT skips them.
    // A0：目录格同理——它今天根本不进这张表（它没有存储，走 ClickCreativeItem），
    // 列在这里是为了万一将来有人把它加进来时答案是对的，而不是靠"它不在表里"这个
    // 前提沉默地正确。
    [[nodiscard]] bool acceptsItems() const {
        return kind != SlotKind::PlayerCraftingOutput && kind != SlotKind::TableCraftingOutput &&
               kind != SlotKind::FurnaceOutput && kind != SlotKind::AnvilOutput &&
               kind != SlotKind::CreativeCatalog;
    }
};

// Everything about the open screen that is not geometry: which screen, which
// container instance, and the two view filters the creative inventory adds.
struct ScreenContext final {
    ContainerScreen screen = ContainerScreen::PlayerInventory;
    std::optional<ChestPosition> chest;
    FurnacePosition furnace{};
    GameMode gameMode = GameMode::Survival;
    // The creative screen shows either the full inventory tab or just the
    // hotbar under an item tab, and the two are drawn in different places.
    bool creativeInventoryTab = true;
    // ENCH-2: the table's cell. Unlike `chest`/`furnace` this addresses no
    // storage (the menu is on the player) — it is carried so the bookshelf
    // rescan knows which cell to scan around. Deliberately LAST: several call
    // sites build this aggregate positionally, and a field inserted above
    // `gameMode` silently shifts every one of them.
    glm::ivec3 enchantingTable{};
    // ENCH-3: the anvil's cell, carried for the same reason — the menu is on
    // the player, this only says which block the screen belongs to.
    glm::ivec3 anvil{};
};

// The screens' slot layout and click routing in one place.
//
// This replaces four parallel walks over "which slots does this screen have and
// where are they" — the click hit-test, the PICKUP_ALL gather, the drag
// hit-test and the drag rectangle lookup — each of which was its own chain of
// `containerScreen ==` branches in the renderer. Building the list once and
// answering all four questions from it is what 26.1's AbstractContainerMenu
// does with its `slots` list.
//
// Nothing here draws, and no rule below asks which screen is open: the routing
// is by SlotKind. That is the point — a gameplay decision that reads
// `containerScreen` is a decision in the wrong place.
class ScreenHandler final {
  public:
    // Builds the slot list for the open screen. Cheap enough to call per event:
    // one vector of PODs, reserved once, no allocation per slot.
    [[nodiscard]] static std::vector<SlotView> buildSlots(
        GameSession& session,
        const ScreenContext& context,
        const ui::HudLayout& layout);

    // Geometry-only form for the render thread. It intentionally leaves every
    // storage pointer null, so hit testing and drag previews cannot reach into
    // simulation-owned inventory/block-entity memory.
    [[nodiscard]] static std::vector<SlotView> buildSlotLayout(
        const ScreenContext& context,
        const ui::HudLayout& layout);

    // The storage a slot click targets, resolved from the open container
    // context by slot kind and index — the gameplay half of a ClickSlot
    // command. The renderer enqueues the intent; the interaction routes it.
    [[nodiscard]] static ItemStack* resolveSlotStorage(GameSession& session,
                                                       const ScreenContext& context,
                                                       SlotKind kind, std::uint16_t index);

    // The slot under the cursor, or nullptr.
    [[nodiscard]] static const SlotView* slotAt(
        const std::vector<SlotView>& slots,
        ui::UiPoint cursor);

    // Applies a click to a slot, including QUICK_MOVE between the main inventory
    // and hotbar, into an open container, or out of a creative-category hotbar.
    static void click(
        GameSession& session,
        const ScreenContext& context,
        const SlotView& slot,
        InventoryMouseButton button,
        bool shiftHeld);

    // QUICK_MOVE out of a player slot into whatever container is open. The
    // container decides where the stack lands. Player-inventory screens use
    // Inventory::clickSlot instead and never enter this helper.
    static void quickMoveToContainer(
        GameSession& session,
        const ScreenContext& context,
        ItemStack& stack);
};

} // namespace mc::gameplay

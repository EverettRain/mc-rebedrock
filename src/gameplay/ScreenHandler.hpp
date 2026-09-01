#pragma once

#include "gameplay/ChestSystem.hpp"
#include "gameplay/Equipment.hpp"
#include "gameplay/FurnaceSystem.hpp"
#include "gameplay/GameMode.hpp"
#include "gameplay/Inventory.hpp"
#include "ui/HudLayout.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace mc::gameplay {

class GameSession;

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
};

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

// One slot on the open screen: where it is, what it is, and the exact storage
// behind it.
//
// `storage` is the identity a drag uses — pointer equality against the real
// ItemStack — and is null for the two output slots, which have no storage of
// their own until the craft happens.
struct SlotView final {
    ui::UiRect rect;
    ItemStack* storage = nullptr;
    SlotKind kind = SlotKind::PlayerInventory;
    std::uint16_t index = 0U;

    // Output slots are not drag targets, and QUICK_CRAFT skips them.
    [[nodiscard]] bool acceptsItems() const {
        return kind != SlotKind::PlayerCraftingOutput && kind != SlotKind::TableCraftingOutput &&
               kind != SlotKind::FurnaceOutput && kind != SlotKind::AnvilOutput;
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

    // The slot a live drag is pointing at, found by the storage identity the
    // drag captured. Returns nullptr once that storage is no longer on screen.
    [[nodiscard]] static const SlotView* slotForStorage(
        const std::vector<SlotView>& slots,
        const ItemStack* storage);

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

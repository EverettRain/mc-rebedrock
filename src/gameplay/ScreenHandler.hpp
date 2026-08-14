#pragma once

#include "gameplay/ChestSystem.hpp"
#include "gameplay/FurnaceSystem.hpp"
#include "gameplay/GameMode.hpp"
#include "gameplay/Inventory.hpp"
#include "ui/HudLayout.hpp"

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
};

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
               kind != SlotKind::FurnaceOutput;
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

    // The slot under the cursor, or nullptr.
    [[nodiscard]] static const SlotView* slotAt(
        const std::vector<SlotView>& slots,
        ui::UiPoint cursor);

    // The slot a live drag is pointing at, found by the storage identity the
    // drag captured. Returns nullptr once that storage is no longer on screen.
    [[nodiscard]] static const SlotView* slotForStorage(
        const std::vector<SlotView>& slots,
        const ItemStack* storage);

    // Applies a click to a slot. Returns false for the one case the caller has
    // to handle itself: a player-inventory slot shift-clicked with no container
    // open, which vanilla routes to the crafting grid rather than anywhere this
    // class owns.
    static void click(
        GameSession& session,
        const ScreenContext& context,
        const SlotView& slot,
        InventoryMouseButton button,
        bool shiftHeld);

    // QUICK_MOVE out of a player slot into whatever container is open. The
    // container decides where the stack lands; with no container open this is a
    // no-op, exactly like vanilla shift-clicking in the survival inventory with
    // nothing to move to.
    static void quickMoveToContainer(
        GameSession& session,
        const ScreenContext& context,
        ItemStack& stack);
};

} // namespace mc::gameplay

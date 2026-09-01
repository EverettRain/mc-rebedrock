#include "gameplay/ScreenHandler.hpp"

#include "gameplay/CraftingSystem.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/Item.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mc::gameplay {
namespace {

// The container part of the screen — everything above the player's own 36
// slots. Appended first so the order matches the way the screens are drawn and
// the way vanilla numbers its slots.
void appendContainerSlots(
    std::vector<SlotView>& slots,
    GameSession* session,
    const ScreenContext& context,
    const ui::HudLayout& layout) {
    switch (context.screen) {
    case ContainerScreen::Chest: {
        if (!context.chest.has_value()) {
            return;
        }
        auto* chest = session != nullptr ? session->chestSystem().find(*context.chest) : nullptr;
        if (session != nullptr && chest == nullptr) return;
        for (std::size_t index = 0; index < ChestBlockEntity::kSlotCount; ++index) {
            slots.push_back({layout.chestSlot(index),
                             chest != nullptr ? &chest->items[index] : nullptr,
                             SlotKind::ChestStorage,
                             static_cast<std::uint16_t>(index)});
        }
        return;
    }
    case ContainerScreen::CraftingTable: {
        for (std::size_t index = 0; index < 9U; ++index) {
            slots.push_back({layout.tableCraftingSlot(index),
                             session != nullptr
                                 ? &session->craftingSystem().tableGridSlot(index)
                                 : nullptr,
                             SlotKind::TableCraftingGrid, static_cast<std::uint16_t>(index)});
        }
        slots.push_back(
            {layout.tableCraftingOutput(), nullptr, SlotKind::TableCraftingOutput, 0U});
        return;
    }
    case ContainerScreen::Furnace: {
        auto* furnace = session != nullptr
                            ? session->furnaceSystem().find(context.furnace)
                            : nullptr;
        if (session != nullptr && furnace == nullptr) return;
        slots.push_back({layout.furnaceInputSlot(),
                         furnace != nullptr ? &furnace->input : nullptr,
                         SlotKind::FurnaceInput, 0U});
        slots.push_back({layout.furnaceFuelSlot(),
                         furnace != nullptr ? &furnace->fuel : nullptr,
                         SlotKind::FurnaceFuel, 0U});
        slots.push_back({layout.furnaceOutputSlot(), nullptr, SlotKind::FurnaceOutput, 0U});
        return;
    }
    case ContainerScreen::EnchantingTable: {
        // ENCH-2: both slots live on the player's own menu, so unlike the chest
        // and the furnace there is nothing to look up and nothing that can be
        // missing — a table whose block was mined out from under the open
        // screen still shows the two stacks it holds, and closing hands them
        // back.
        auto* menu = session != nullptr ? &session->enchantingMenu() : nullptr;
        slots.push_back({layout.enchantingItemSlot(), menu != nullptr ? &menu->item : nullptr,
                         SlotKind::EnchantingItem, 0U});
        slots.push_back({layout.enchantingLapisSlot(), menu != nullptr ? &menu->lapis : nullptr,
                         SlotKind::EnchantingLapis, 0U});
        return;
    }
    case ContainerScreen::PlayerInventory: {
        // The 2x2 grid only exists in the survival screen; creative has no
        // crafting at all.
        if (context.gameMode != GameMode::Survival) {
            return;
        }
        for (std::size_t index = 0; index < 4U; ++index) {
            slots.push_back({layout.playerCraftingSlot(index),
                             session != nullptr
                                 ? &session->craftingSystem().playerGridSlot(index)
                                 : nullptr,
                             SlotKind::PlayerCraftingGrid, static_cast<std::uint16_t>(index)});
        }
        slots.push_back(
            {layout.playerCraftingOutput(), nullptr, SlotKind::PlayerCraftingOutput, 0U});
        return;
    }
    }
}

} // namespace

namespace {

// EQ-1: the four armor slots + offhand only exist on the player-inventory
// screen's own body — never in a chest/furnace/crafting-table screen, and (like
// the 2x2 crafting grid above) not under a creative item-category tab, which
// shows only the hotbar. `session` is null for the geometry-only render-thread
// form, exactly like appendContainerSlots' own null-session convention.
void appendEquipmentSlots(
    std::vector<SlotView>& slots,
    GameSession* session,
    const ScreenContext& context,
    const ui::HudLayout& layout) {
    if (context.screen != ContainerScreen::PlayerInventory) {
        return;
    }
    if (context.gameMode == GameMode::Creative && !context.creativeInventoryTab) {
        return;
    }
    const bool creative =
        context.gameMode == GameMode::Creative && context.creativeInventoryTab;
    for (std::size_t index = 0; index < kEquipmentScreenSlotCount; ++index) {
        const auto rect =
            index < 4U ? layout.armorSlot(index, creative) : layout.offhandSlot(creative);
        slots.push_back(
            {rect,
             session != nullptr ? &session->equipment().mutableSlot(equipmentSlotAt(index))
                                 : nullptr,
             SlotKind::Equipment, static_cast<std::uint16_t>(index)});
    }
}

} // namespace

std::vector<SlotView> ScreenHandler::buildSlots(
    GameSession& session,
    const ScreenContext& context,
    const ui::HudLayout& layout) {
    std::vector<SlotView> slots;
    slots.reserve(ChestBlockEntity::kSlotCount + Inventory::kSlotCount + kEquipmentScreenSlotCount);
    appendContainerSlots(slots, &session, context, layout);
    appendEquipmentSlots(slots, &session, context, layout);

    // The player's own slots follow, drawn wherever the open screen puts them.
    const bool creativeScreen = context.screen == ContainerScreen::PlayerInventory &&
                                context.gameMode == GameMode::Creative;
    for (std::size_t index = 0; index < Inventory::kSlotCount; ++index) {
        // Under a creative item tab only the hotbar is on screen; the rest of
        // the inventory is not drawn, so it is not a slot the player can hit.
        if (creativeScreen && !context.creativeInventoryTab &&
            index >= Inventory::kHotbarSize) {
            continue;
        }
        const auto rect =
            context.screen == ContainerScreen::Chest ? layout.chestInventorySlot(index)
            : creativeScreen ? (context.creativeInventoryTab ? layout.creativeInventorySlot(index)
                                                             : layout.creativeHotbarSlot(index))
                             : layout.inventorySlot(index);
        slots.push_back({rect, &session.inventory().mutableSlot(index), SlotKind::PlayerInventory,
                         static_cast<std::uint16_t>(index)});
    }
    return slots;
}

std::vector<SlotView> ScreenHandler::buildSlotLayout(
    const ScreenContext& context,
    const ui::HudLayout& layout) {
    std::vector<SlotView> slots;
    slots.reserve(ChestBlockEntity::kSlotCount + Inventory::kSlotCount + kEquipmentScreenSlotCount);
    appendContainerSlots(slots, nullptr, context, layout);
    appendEquipmentSlots(slots, nullptr, context, layout);

    const bool creativeScreen = context.screen == ContainerScreen::PlayerInventory &&
                                context.gameMode == GameMode::Creative;
    for (std::size_t index = 0; index < Inventory::kSlotCount; ++index) {
        if (creativeScreen && !context.creativeInventoryTab &&
            index >= Inventory::kHotbarSize) {
            continue;
        }
        const auto rect =
            context.screen == ContainerScreen::Chest ? layout.chestInventorySlot(index)
            : creativeScreen ? (context.creativeInventoryTab ? layout.creativeInventorySlot(index)
                                                             : layout.creativeHotbarSlot(index))
                             : layout.inventorySlot(index);
        slots.push_back(
            {rect, nullptr, SlotKind::PlayerInventory, static_cast<std::uint16_t>(index)});
    }
    return slots;
}

ItemStack* ScreenHandler::resolveSlotStorage(GameSession& session,
                                             const ScreenContext& context, SlotKind kind,
                                             std::uint16_t index) {
    switch (kind) {
    case SlotKind::PlayerInventory:
        return &session.inventory().mutableSlot(index);
    case SlotKind::Equipment:
        if (index >= kEquipmentScreenSlotCount) return nullptr;
        return &session.equipment().mutableSlot(equipmentSlotAt(index));
    case SlotKind::ChestStorage:
        if (context.chest.has_value()) {
            if (auto* chest = session.chestSystem().find(*context.chest); chest != nullptr) {
                return &chest->items[index];
            }
        }
        return nullptr;
    case SlotKind::PlayerCraftingGrid:
        return &session.craftingSystem().playerGridSlot(index);
    case SlotKind::TableCraftingGrid:
        return &session.craftingSystem().tableGridSlot(index);
    case SlotKind::FurnaceInput: {
        auto* furnace = session.furnaceSystem().find(context.furnace);
        return furnace != nullptr ? &furnace->input : nullptr;
    }
    case SlotKind::FurnaceFuel: {
        auto* furnace = session.furnaceSystem().find(context.furnace);
        return furnace != nullptr ? &furnace->fuel : nullptr;
    }
    case SlotKind::EnchantingItem:
        return &session.enchantingMenu().item;
    case SlotKind::EnchantingLapis:
        return &session.enchantingMenu().lapis;
    case SlotKind::PlayerCraftingOutput:
    case SlotKind::TableCraftingOutput:
    case SlotKind::FurnaceOutput:
        return nullptr;
    }
    return nullptr;
}

const SlotView* ScreenHandler::slotAt(const std::vector<SlotView>& slots, ui::UiPoint cursor) {
    const auto found = std::ranges::find_if(slots, [&](const SlotView& slot) {
        return slot.rect.contains(cursor.x, cursor.y);
    });
    return found == slots.end() ? nullptr : &*found;
}

const SlotView* ScreenHandler::slotForStorage(
    const std::vector<SlotView>& slots,
    const ItemStack* storage) {
    if (storage == nullptr) {
        return nullptr;
    }
    const auto found = std::ranges::find_if(
        slots, [&](const SlotView& slot) { return slot.storage == storage; });
    return found == slots.end() ? nullptr : &*found;
}

namespace {

// AbstractFurnaceBlockEntity#createExperience: the banked float splits into a
// guaranteed integer part plus a fractional coin flip — smelting one iron ore
// (0.7 experience) does not owe an orb every time, but does about 70% of the
// time, and a big batch converges on the same *total* either roll order would
// give. Drawn from the session's own experienceOrbRandom stream (the same one
// spawnExperienceOrbs' scatter velocity uses), so replaying the same seed and
// click sequence always cashes in the same amount.
void cashInFurnaceExperience(GameSession& session, FurnacePosition position) {
    const float banked = session.furnaceSystem().popExperience(position);
    if (banked <= 0.0F) {
        return;
    }
    const float whole = std::floor(banked);
    const float fraction = banked - whole;
    std::int32_t amount = static_cast<std::int32_t>(whole);
    if (fraction > 0.0F && session.experienceOrbRandom().nextFloat() < fraction) {
        ++amount;
    }
    if (amount <= 0) {
        return;
    }
    session.spawnExperienceOrbs(
        {static_cast<float>(position.x) + 0.5F, static_cast<float>(position.y) + 0.5F,
         static_cast<float>(position.z) + 0.5F},
        amount);
}

// EQ-1: Slot#mayPlace for an armor/offhand slot — the click still goes through
// Inventory::clickExternalSlot's ordinary pickup/place/merge machinery (so an
// armor slot behaves exactly like any other slot for stacking, half-stack
// right-click pickup, etc.), except that placing the cursor stack down is
// refused outright when it fails canEquip. Taking an item out (cursor empty,
// slot occupied) is always allowed regardless of what is worn — canEquip only
// gates what may go IN, matching vanilla's Slot#mayPlace (there is no
// corresponding mayPickup restriction on armor).
void clickEquipmentSlot(GameSession& session, EquipmentSlot equipmentSlot,
                        InventoryMouseButton button) {
    ItemStack& worn = session.equipment().mutableSlot(equipmentSlot);
    const ItemStack& cursor = session.inventory().cursorStack();
    // A right-click with an empty slot and a full cursor would place one item
    // (clickExternalSlot's pickup==0 branch simply falls through and does
    // nothing when clicked is empty and cursor is non-empty on Right — the
    // "place one" case); a left-click would place the whole stack. Either
    // way, if the cursor holds something this slot cannot equip and the slot
    // itself is not simply being emptied, refuse.
    if (!cursor.empty() && !canEquip(equipmentSlot, cursor)) {
        return;
    }
    session.inventory().clickExternalSlot(worn, button);
}

} // namespace

void ScreenHandler::click(
    GameSession& session,
    const ScreenContext& context,
    const SlotView& slot,
    InventoryMouseButton button,
    bool shiftHeld) {
    // The routing is by what the slot is, never by which screen is open. Adding
    // a screen that reuses a slot kind therefore needs nothing here.
    switch (slot.kind) {
    case SlotKind::ChestStorage:
        if (context.chest.has_value()) {
            session.chestSystem().clickSlot(*context.chest, slot.index, session.inventory(),
                                            button, shiftHeld);
        }
        break;
    case SlotKind::TableCraftingGrid:
        session.craftingSystem().clickTableSlot(session.inventory(), slot.index, button,
                                                shiftHeld);
        break;
    case SlotKind::PlayerCraftingGrid:
        session.craftingSystem().clickPlayerSlot(session.inventory(), slot.index, button,
                                                 shiftHeld);
        break;
    case SlotKind::TableCraftingOutput:
        // Shift-click is QUICK_MOVE: the whole result goes to the inventory
        // instead of the cursor, like vanilla's result slot.
        static_cast<void>(session.craftingSystem().craftTable(session.inventory(), shiftHeld));
        break;
    case SlotKind::PlayerCraftingOutput:
        static_cast<void>(session.craftingSystem().craftPlayer(session.inventory(), shiftHeld));
        break;
    case SlotKind::FurnaceInput:
        session.furnaceSystem().clickInput(context.furnace, session.inventory(), button,
                                           shiftHeld);
        break;
    case SlotKind::FurnaceFuel:
        session.furnaceSystem().clickFuel(context.furnace, session.inventory(), button, shiftHeld);
        break;
    case SlotKind::FurnaceOutput:
        if (session.furnaceSystem().clickOutput(context.furnace, session.inventory(), shiftHeld)) {
            // FurnaceResultSlot#onTake -> checkTakeAchievements ->
            // awardUsedRecipesAndPopExperience: cash in whatever this furnace
            // banked across every smelt since the last withdrawal, the instant
            // any amount actually left the result slot — never on the smelt
            // tick itself, so a furnace can queue dozens of items unattended
            // and pay out only once a hand reaches in for real.
            cashInFurnaceExperience(session, context.furnace);
        }
        break;
    case SlotKind::EnchantingItem:
    case SlotKind::EnchantingLapis: {
        // Both are plain storage slots owned by the player's menu, so an
        // ordinary external-slot click (pickup / place / half-stack / merge) is
        // the whole behaviour — except that the lapis slot refuses anything but
        // lapis, the way vanilla's `mayPlace` does. Shift-click sends the stack
        // back to the player's own inventory (Slot#mayPlace never gates a
        // takeaway).
        ItemStack& storage = slot.kind == SlotKind::EnchantingItem
                                 ? session.enchantingMenu().item
                                 : session.enchantingMenu().lapis;
        if (shiftHeld) {
            session.inventory().quickMoveInto(storage);
            break;
        }
        const ItemStack& cursor = session.inventory().cursorStack();
        if (slot.kind == SlotKind::EnchantingLapis && !cursor.empty() &&
            cursor.item != &items::LapisLazuli) {
            break;
        }
        session.inventory().clickExternalSlot(storage, button);
        break;
    }
    case SlotKind::Equipment:
        if (slot.index >= kEquipmentScreenSlotCount) break;
        if (shiftHeld) {
            // Shift-click OUT of an armor/offhand slot: QUICK_MOVE back into the
            // player's own inventory, the same direction a shift-click furnace
            // output or chest slot uses — Slot#mayPlace never gates a takeaway.
            session.inventory().quickMoveInto(
                session.equipment().mutableSlot(equipmentSlotAt(slot.index)));
        } else {
            clickEquipmentSlot(session, equipmentSlotAt(slot.index), button);
        }
        break;
    case SlotKind::PlayerInventory:
        if (shiftHeld) {
            if (context.screen == ContainerScreen::PlayerInventory) {
                // Vanilla's InventoryMenu#quickMoveStack: a shift-clicked armor
                // (or offhand-eligible) piece tries its own equipment slot
                // FIRST, before falling into the ordinary hotbar<->main swap —
                // "shift-click quick-equips", not "shift-click stores".
                const ItemStack& clicked = session.inventory().slot(slot.index);
                if (!clicked.empty() && isArmor(clicked.item)) {
                    const EquipmentSlot target = armorSlotOf(clicked.item);
                    ItemStack& wornSlot = session.equipment().mutableSlot(target);
                    if (wornSlot.empty()) {
                        wornSlot = session.inventory().mutableSlot(slot.index);
                        session.inventory().mutableSlot(slot.index) = {};
                        break;
                    }
                }
                if (context.gameMode == GameMode::Creative &&
                    !context.creativeInventoryTab) {
                    // In an item-category tab the only real player slots are
                    // the hotbar. QUICK_MOVE sends that stack into the creative
                    // catalogue, whose backing storage is infinite: the local
                    // stack is therefore deleted, matching vanilla.
                    session.inventory().mutableSlot(slot.index) = {};
                } else {
                    // With no external container, QUICK_MOVE swaps regions of
                    // the player's own inventory: hotbar -> main, main -> hotbar.
                    session.inventory().clickSlot(slot.index, button, true);
                }
            } else {
                quickMoveToContainer(
                    session, context, session.inventory().mutableSlot(slot.index));
            }
        } else {
            session.inventory().clickSlot(slot.index, button, false);
        }
        break;
    }
}

void ScreenHandler::quickMoveToContainer(
    GameSession& session,
    const ScreenContext& context,
    ItemStack& stack) {
    switch (context.screen) {
    case ContainerScreen::Chest:
        if (context.chest.has_value()) {
            static_cast<void>(session.chestSystem().moveInto(*context.chest, stack));
        }
        break;
    case ContainerScreen::CraftingTable:
        static_cast<void>(session.craftingSystem().moveTableInto(stack));
        break;
    case ContainerScreen::Furnace:
        static_cast<void>(session.furnaceSystem().moveInto(context.furnace, stack));
        break;
    case ContainerScreen::EnchantingTable:
        // ENCH-2: only lapis has a home here. Anything else shift-clicked from
        // the player's inventory stays put (vanilla's quickMoveStack refuses
        // the item slot for a stack it cannot enchant, and this project has no
        // per-slot stack limit to make a size-1 item slot safe — see
        // isEnchantable's note). The item slot is filled by an ordinary click.
        if (!stack.empty() && stack.item == &items::LapisLazuli) {
            ItemStack& lapis = session.enchantingMenu().lapis;
            if (lapis.empty()) {
                lapis = stack;
                stack = {};
            } else if (sameItem(lapis, stack)) {
                const std::uint8_t room =
                    static_cast<std::uint8_t>(itemMaximumStackSize(lapis) - lapis.count);
                const std::uint8_t moved = std::min<std::uint8_t>(room, stack.count);
                lapis.count = static_cast<std::uint8_t>(lapis.count + moved);
                stack.count = static_cast<std::uint8_t>(stack.count - moved);
                if (stack.count == 0U) {
                    stack = {};
                }
            }
        }
        break;
    case ContainerScreen::PlayerInventory:
        break;
    }
}

} // namespace mc::gameplay

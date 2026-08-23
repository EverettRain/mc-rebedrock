#include "gameplay/ScreenHandler.hpp"

#include "gameplay/CraftingSystem.hpp"
#include "gameplay/GameSession.hpp"

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

std::vector<SlotView> ScreenHandler::buildSlots(
    GameSession& session,
    const ScreenContext& context,
    const ui::HudLayout& layout) {
    std::vector<SlotView> slots;
    slots.reserve(ChestBlockEntity::kSlotCount + Inventory::kSlotCount + 1U);
    appendContainerSlots(slots, &session, context, layout);

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
    slots.reserve(ChestBlockEntity::kSlotCount + Inventory::kSlotCount + 1U);
    appendContainerSlots(slots, nullptr, context, layout);

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
    case SlotKind::PlayerInventory:
        if (shiftHeld) {
            if (context.screen == ContainerScreen::PlayerInventory) {
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
    case ContainerScreen::PlayerInventory:
        break;
    }
}

} // namespace mc::gameplay

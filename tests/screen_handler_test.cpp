#include "gameplay/ScreenHandler.hpp"

#include "gameplay/CraftingSystem.hpp"
#include "gameplay/GameSession.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

// The screens' slot layout and click routing, headless. Before the
// ScreenHandler these questions were four separate chains of
// `containerScreen ==` branches inside VulkanRenderer::Impl, where no test
// could reach them: opening a chest and shift-clicking was a thing only a human
// with a window could check.

namespace {

using namespace mc;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"screen_handler_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

[[nodiscard]] ui::HudLayout makeLayout() {
    return ui::HudLayout{1920.0F, 1080.0F, 0};
}

[[nodiscard]] std::size_t countKind(
    const std::vector<gameplay::SlotView>& slots,
    gameplay::SlotKind kind) {
    return static_cast<std::size_t>(std::ranges::count_if(
        slots, [&](const gameplay::SlotView& slot) { return slot.kind == kind; }));
}

[[nodiscard]] const gameplay::SlotView* firstOfKind(
    const std::vector<gameplay::SlotView>& slots,
    gameplay::SlotKind kind) {
    const auto found = std::ranges::find_if(
        slots, [&](const gameplay::SlotView& slot) { return slot.kind == kind; });
    return found == slots.end() ? nullptr : &*found;
}

[[nodiscard]] ui::UiPoint centreOf(const ui::UiRect& rect) {
    return {rect.x + rect.width * 0.5F, rect.y + rect.height * 0.5F};
}

// The survival player screen: the 2x2 grid, its output, and the 36 inventory
// slots.
void testPlayerScreen() {
    gameplay::GameSession session;
    session.setGameMode(gameplay::GameMode::Survival);
    const auto layout = makeLayout();
    const gameplay::ScreenContext context{gameplay::ContainerScreen::PlayerInventory,
                                          std::nullopt,
                                          {},
                                          gameplay::GameMode::Survival,
                                          true};
    const auto slots = gameplay::ScreenHandler::buildSlots(session, context, layout);
    REQUIRE(countKind(slots, gameplay::SlotKind::PlayerCraftingGrid) == 4U);
    REQUIRE(countKind(slots, gameplay::SlotKind::PlayerCraftingOutput) == 1U);
    REQUIRE(countKind(slots, gameplay::SlotKind::PlayerInventory) ==
            gameplay::Inventory::kSlotCount);
    // EQ-1: the four armor slots + offhand are part of this same screen's own
    // body now, alongside the 2x2 crafting grid and the 36 carried slots.
    REQUIRE(countKind(slots, gameplay::SlotKind::Equipment) ==
            gameplay::kEquipmentScreenSlotCount);
    REQUIRE(slots.size() == 4U + 1U + gameplay::kEquipmentScreenSlotCount +
                                gameplay::Inventory::kSlotCount);

    // The output slot has no storage of its own and never takes an item, which
    // is what keeps it out of the PICKUP_ALL gather and out of a drag.
    const auto* output = firstOfKind(slots, gameplay::SlotKind::PlayerCraftingOutput);
    REQUIRE(output != nullptr);
    REQUIRE(output->storage == nullptr);
    REQUIRE(!output->acceptsItems());

    // Every other slot points at the real storage, so a drag can identify it by
    // pointer the way the renderer's preview does.
    const auto* grid = firstOfKind(slots, gameplay::SlotKind::PlayerCraftingGrid);
    REQUIRE(grid != nullptr);
    REQUIRE(grid->storage == &session.craftingSystem().playerGridSlot(0));
    REQUIRE(grid->acceptsItems());
}

// Creative has no crafting grid at all, and under an item tab only the hotbar
// is on screen — the rest of the inventory is not drawn, so it is not a slot
// the player can hit.
void testCreativeTabs() {
    gameplay::GameSession session;
    session.setGameMode(gameplay::GameMode::Creative);
    const auto layout = makeLayout();
    gameplay::ScreenContext context{gameplay::ContainerScreen::PlayerInventory,
                                    std::nullopt,
                                    {},
                                    gameplay::GameMode::Creative,
                                    true};
    const auto inventoryTab = gameplay::ScreenHandler::buildSlots(session, context, layout);
    REQUIRE(countKind(inventoryTab, gameplay::SlotKind::PlayerCraftingGrid) == 0U);
    // EQ-1: the creative screen's own "Inventory" tab shows the armor row too
    // (the same tab the plain 36 carried slots are shown under); an item
    // category tab (below) shows neither.
    REQUIRE(countKind(inventoryTab, gameplay::SlotKind::Equipment) ==
            gameplay::kEquipmentScreenSlotCount);
    REQUIRE(inventoryTab.size() ==
            gameplay::kEquipmentScreenSlotCount + gameplay::Inventory::kSlotCount);

    context.creativeInventoryTab = false;
    const auto itemTab = gameplay::ScreenHandler::buildSlots(session, context, layout);
    // Only the hotbar is on screen under an item tab. The hotbar itself is drawn
    // in the same place in both tabs (creativeInventorySlot delegates to
    // creativeHotbarSlot below nine), so the difference is which slots exist —
    // and a click on where the inventory rows would be must hit nothing.
    REQUIRE(countKind(itemTab, gameplay::SlotKind::Equipment) == 0U);
    REQUIRE(itemTab.size() == gameplay::Inventory::kHotbarSize);
    // Compared by PlayerInventory-kind slot specifically, not .front() — the
    // full inventory tab's front slot is now an armor slot (EQ-1 prepends
    // equipment ahead of the 36 carried slots), so .front() alone no longer
    // names "the hotbar's own row" in that tab.
    REQUIRE(itemTab.front().rect.y ==
            firstOfKind(inventoryTab, gameplay::SlotKind::PlayerInventory)->rect.y);
    const auto inventoryRow = centreOf(layout.creativeInventorySlot(20));
    REQUIRE(gameplay::ScreenHandler::slotAt(inventoryTab, inventoryRow) != nullptr);
    REQUIRE(gameplay::ScreenHandler::slotAt(itemTab, inventoryRow) == nullptr);
}

// A chest screen: the container's 27 slots, then the player's 36 drawn at the
// chest screen's own offsets rather than the plain inventory ones.
void testChestScreen() {
    gameplay::GameSession session;
    const gameplay::ChestPosition position{3, 64, 5};
    REQUIRE(session.chestSystem().place(position));
    const auto layout = makeLayout();
    const gameplay::ScreenContext context{gameplay::ContainerScreen::Chest, position, {},
                                          gameplay::GameMode::Survival, true};
    const auto slots = gameplay::ScreenHandler::buildSlots(session, context, layout);
    REQUIRE(countKind(slots, gameplay::SlotKind::ChestStorage) ==
            gameplay::ChestBlockEntity::kSlotCount);
    REQUIRE(countKind(slots, gameplay::SlotKind::PlayerInventory) ==
            gameplay::Inventory::kSlotCount);
    // No crafting grid: the chest screen replaces the player screen's panel.
    REQUIRE(countKind(slots, gameplay::SlotKind::PlayerCraftingGrid) == 0U);
    const auto* player = firstOfKind(slots, gameplay::SlotKind::PlayerInventory);
    REQUIRE(player != nullptr);
    REQUIRE(player->rect.y == layout.chestInventorySlot(0).y);
    REQUIRE(player->rect.y != layout.inventorySlot(0).y);

    // A chest that has been broken while its screen is open contributes no
    // slots, rather than a list of dangling pointers.
    static_cast<void>(session.chestSystem().remove(position));
    const auto orphaned = gameplay::ScreenHandler::buildSlots(session, context, layout);
    REQUIRE(countKind(orphaned, gameplay::SlotKind::ChestStorage) == 0U);
    REQUIRE(orphaned.size() == gameplay::Inventory::kSlotCount);
}

void testFurnaceScreen() {
    gameplay::GameSession session;
    const gameplay::FurnacePosition position{1, 64, 1};
    REQUIRE(session.furnaceSystem().place(position));
    const auto layout = makeLayout();
    const gameplay::ScreenContext context{gameplay::ContainerScreen::Furnace, std::nullopt,
                                          position, gameplay::GameMode::Survival, true};
    const auto slots = gameplay::ScreenHandler::buildSlots(session, context, layout);
    REQUIRE(countKind(slots, gameplay::SlotKind::FurnaceInput) == 1U);
    REQUIRE(countKind(slots, gameplay::SlotKind::FurnaceFuel) == 1U);
    REQUIRE(countKind(slots, gameplay::SlotKind::FurnaceOutput) == 1U);
    const auto* result = firstOfKind(slots, gameplay::SlotKind::FurnaceOutput);
    REQUIRE(result != nullptr && !result->acceptsItems());
    const auto* input = firstOfKind(slots, gameplay::SlotKind::FurnaceInput);
    REQUIRE(input != nullptr && input->storage == &session.furnaceSystem().find(position)->input);
}

// The two lookups the drag preview relies on: cursor to slot, and storage back
// to the rectangle it is drawn in.
void testLookups() {
    gameplay::GameSession session;
    const gameplay::ChestPosition position{0, 64, 0};
    REQUIRE(session.chestSystem().place(position));
    const auto layout = makeLayout();
    const gameplay::ScreenContext context{gameplay::ContainerScreen::Chest, position, {},
                                          gameplay::GameMode::Survival, true};
    const auto slots = gameplay::ScreenHandler::buildSlots(session, context, layout);

    const auto* fifth = &slots[5];
    const auto* hit = gameplay::ScreenHandler::slotAt(slots, centreOf(fifth->rect));
    REQUIRE(hit == fifth);
    // A point in no slot resolves to nothing rather than to the nearest one.
    REQUIRE(gameplay::ScreenHandler::slotAt(slots, {0.0F, 0.0F}) == nullptr);

    const auto* byStorage = gameplay::ScreenHandler::slotForStorage(slots, fifth->storage);
    REQUIRE(byStorage != nullptr);
    REQUIRE(byStorage->rect.x == fifth->rect.x && byStorage->rect.y == fifth->rect.y);
    // An output slot has no storage, so a null lookup must not match it.
    REQUIRE(gameplay::ScreenHandler::slotForStorage(slots, nullptr) == nullptr);
    gameplay::ItemStack elsewhere;
    REQUIRE(gameplay::ScreenHandler::slotForStorage(slots, &elsewhere) == nullptr);
}

// Renderer hit testing receives the same geometry and slot identities, but no
// pointer into simulation-owned inventory or block-entity storage.
void testGeometryOnlyLayout() {
    const auto layout = makeLayout();
    const gameplay::ChestPosition position{0, 64, 0};
    const gameplay::ScreenContext context{gameplay::ContainerScreen::Chest, position, {},
                                          gameplay::GameMode::Survival, true};
    const auto slots = gameplay::ScreenHandler::buildSlotLayout(context, layout);
    REQUIRE(countKind(slots, gameplay::SlotKind::ChestStorage) ==
            gameplay::ChestBlockEntity::kSlotCount);
    REQUIRE(countKind(slots, gameplay::SlotKind::PlayerInventory) ==
            gameplay::Inventory::kSlotCount);
    REQUIRE(std::ranges::all_of(slots, [](const gameplay::SlotView& slot) {
        return slot.storage == nullptr;
    }));
    const auto* hit = gameplay::ScreenHandler::slotAt(slots, centreOf(slots.front().rect));
    REQUIRE(hit != nullptr && hit->kind == gameplay::SlotKind::ChestStorage && hit->index == 0U);
}

// The routing itself: a click lands on the system that owns the slot's kind,
// and a shift-click in the player inventory hands the stack to whatever
// container is open.
void testClickRouting() {
    gameplay::GameSession session;
    const gameplay::ChestPosition position{2, 64, 2};
    REQUIRE(session.chestSystem().place(position));
    const auto layout = makeLayout();
    const gameplay::ScreenContext context{gameplay::ContainerScreen::Chest, position, {},
                                          gameplay::GameMode::Survival, true};

    // Put a stack in the player's first slot and shift-click it: it belongs to
    // the chest afterwards.
    session.inventory().mutableSlot(0) = {world::Block::Stone, 32U};
    {
        const auto slots = gameplay::ScreenHandler::buildSlots(session, context, layout);
        const auto* player = firstOfKind(slots, gameplay::SlotKind::PlayerInventory);
        REQUIRE(player != nullptr && player->index == 0U);
        gameplay::ScreenHandler::click(session, context, *player,
                                       gameplay::InventoryMouseButton::Left, true);
    }
    REQUIRE(session.inventory().slot(0).empty());
    const auto* chest = session.chestSystem().find(position);
    REQUIRE(chest != nullptr);
    REQUIRE(chest->items[0].block == world::Block::Stone);
    REQUIRE(chest->items[0].count == 32U);

    // A plain click on that chest slot picks the stack up onto the cursor.
    {
        const auto slots = gameplay::ScreenHandler::buildSlots(session, context, layout);
        const auto* stored = firstOfKind(slots, gameplay::SlotKind::ChestStorage);
        REQUIRE(stored != nullptr);
        gameplay::ScreenHandler::click(session, context, *stored,
                                       gameplay::InventoryMouseButton::Left, false);
    }
    REQUIRE(session.inventory().cursorStack().block == world::Block::Stone);
    REQUIRE(session.chestSystem().find(position)->items[0].empty());
}

// With no external container, QUICK_MOVE still has two regions to transfer
// between: the nine-slot hotbar and the 27-slot main inventory.
void testPlayerInventoryQuickMove() {
    gameplay::GameSession session;
    session.setGameMode(gameplay::GameMode::Survival);
    const auto layout = makeLayout();
    const gameplay::ScreenContext context{gameplay::ContainerScreen::PlayerInventory,
                                          std::nullopt,
                                          {},
                                          gameplay::GameMode::Survival,
                                          true};
    session.inventory().mutableSlot(3) = {world::Block::Dirt, 5U};
    const auto slots = gameplay::ScreenHandler::buildSlots(session, context, layout);
    const auto findPlayerSlot = [&](std::uint16_t index) {
        const auto found = std::ranges::find_if(
            slots, [index](const gameplay::SlotView& slot) {
                return slot.kind == gameplay::SlotKind::PlayerInventory && slot.index == index;
            });
        return found == slots.end() ? nullptr : &*found;
    };
    const auto* hotbar = findPlayerSlot(3U);
    REQUIRE(hotbar != nullptr);
    gameplay::ScreenHandler::click(session, context, *hotbar,
                                   gameplay::InventoryMouseButton::Left, true);
    REQUIRE(session.inventory().slot(3).empty());
    REQUIRE(session.inventory().slot(gameplay::Inventory::kHotbarSize).block ==
            world::Block::Dirt);

    const auto* main = findPlayerSlot(
        static_cast<std::uint16_t>(gameplay::Inventory::kHotbarSize));
    REQUIRE(main != nullptr);
    gameplay::ScreenHandler::click(session, context, *main,
                                   gameplay::InventoryMouseButton::Left, true);
    REQUIRE(session.inventory().slot(gameplay::Inventory::kHotbarSize).empty());
    REQUIRE(session.inventory().slot(0).block == world::Block::Dirt);
}

// Under a creative item category only the hotbar is backed by player storage.
// QUICK_MOVE returns its stack to the infinite catalogue, so the slot clears.
void testCreativeCatalogQuickDiscard() {
    gameplay::GameSession session;
    session.setGameMode(gameplay::GameMode::Creative);
    const auto layout = makeLayout();
    const gameplay::ScreenContext context{gameplay::ContainerScreen::PlayerInventory,
                                          std::nullopt,
                                          {},
                                          gameplay::GameMode::Creative,
                                          false};
    session.inventory().mutableSlot(4) = {world::Block::Stone, 64U};
    const auto slots = gameplay::ScreenHandler::buildSlots(session, context, layout);
    const auto found = std::ranges::find_if(
        slots, [](const gameplay::SlotView& slot) {
            return slot.kind == gameplay::SlotKind::PlayerInventory && slot.index == 4U;
        });
    REQUIRE(found != slots.end());
    gameplay::ScreenHandler::click(session, context, *found,
                                   gameplay::InventoryMouseButton::Left, true);
    REQUIRE(session.inventory().slot(4).empty());
}

} // namespace

int main() {
    testPlayerScreen();
    testCreativeTabs();
    testChestScreen();
    testFurnaceScreen();
    testLookups();
    testGeometryOnlyLayout();
    testClickRouting();
    testPlayerInventoryQuickMove();
    testCreativeCatalogQuickDiscard();
    return 0;
}

// AR-M6: the trade screen's BACKEND contract.
//
// The screen itself is not built here — this is the interface a UI plugs into,
// and this file is what says the interface holds. Every assertion below is a
// promise the document
// docs/content-dev/AR-content-realization/AR-M6-trading-backend-interface.md
// makes to whoever writes the screen:
//
//   * opening refuses what cannot be traded with, so a UI never opens on
//     nothing;
//   * selecting is total — every index is legal to send, including nonsense;
//   * the result slot is DERIVED and re-derived, never written by a click;
//   * taking the result is the only thing that changes the world, and it checks
//     everything itself;
//   * the payments come back on close, and on the villager going away;
//   * the snapshot carries everything the screen draws, so the UI never reaches
//     into gameplay.

#include "gameplay/GameCommand.hpp"
#include "gameplay/GameCommandCodec.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/ScreenHandler.hpp"
#include "gameplay/TradingMenu.hpp"
#include "gameplay/entities/BuiltinSpecies.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/Villager.hpp"
#include "ui/ContainerPage.hpp"
#include "ui/WidgetId.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <variant>
#include <string>

using namespace mc;
using gameplay::entities::VillagerProfession;

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"trading_backend_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

struct TestHost final : gameplay::SimulationHost {
    void submitWorldEdit(int, int, int, world::Block, std::uint8_t,
                         std::optional<world::BlockOrientation>) override {}
    void submitWorldStateEdit(int, int, int, world::BlockState) override {}
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(world::Block, glm::vec3) override {}
    void playBlockHit(world::Block, glm::vec3) override {}
    void playBlockPlace(world::Block, glm::vec3) override {}
    void playItemBreak(glm::vec3) override {}
    void playItemPickup(glm::vec3) override {}
    void playEat(glm::vec3) override {}
    void playPlayerHurt(glm::vec3) override {}
    void playPlayerFall(glm::vec3, bool) override {}
    void playBurp(glm::vec3) override {}
    void playExplode(glm::vec3) override {}
    void playCreatureHurt(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playFootstep(world::Block, glm::vec3, float) override {}
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, world::Block) override {}
    void spawnWaterSplash(glm::vec3) override {}
    void onPlayerDied() override {}
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {}
    void onEatingCancelled() override {}
};

[[nodiscard]] world::World makeFlatWorld() {
    world::World world;
    world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, world::Block::Stone);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

[[nodiscard]] const gameplay::entities::EntityType& villagerType() {
    const auto* type = gameplay::entities::entityTypeRegistry().byId("villager");
    if (type == nullptr) {
        throw std::runtime_error{"villager species not registered"};
    }
    return *type;
}

[[nodiscard]] std::size_t countHeld(const gameplay::Inventory& inventory,
                                    const gameplay::Item* item) {
    std::size_t total = 0U;
    for (const auto& slot : inventory.slots()) {
        if (slot.item == item) {
            total += slot.count;
        }
    }
    return total;
}

// A session with one employed villager standing next to the player, and the
// trade screen already open on it.
struct Fixture final {
    TestHost host;
    world::World world = makeFlatWorld();
    gameplay::GameSession session;
    std::uint64_t villagerId = 0U;

    Fixture() {
        session.setGameMode(gameplay::GameMode::Survival);
        session.player().setPosition({8.5F, 1.0F, 8.5F});
        session.worldEntities().spawn({9.0F, 1.001F, 8.5F}, villagerType(), 5U);
        villagerId = session.worldEntities().entities().front().id;
        session.worldEntities().byId(villagerId)->villagerProfession = VillagerProfession::Farmer;
    }

    [[nodiscard]] gameplay::SimpleEntity& villager() {
        return *session.worldEntities().byId(villagerId);
    }
    [[nodiscard]] gameplay::TradingMenu& menu() { return session.tradingMenu(); }
    void tick() { session.tick(world, host); }
};

// --- 1) opening ------------------------------------------------------------

void testOpeningRefusesWhatCannotBeTradedWith() {
    Fixture fixture;
    // An entity id nobody has.
    REQUIRE(!fixture.session.openTradingContainer(999999U));
    REQUIRE(!fixture.menu().open());
    // A creature that is not a villager.
    const auto* pig = gameplay::entities::entityTypeRegistry().byId("pig");
    REQUIRE(pig != nullptr);
    fixture.session.worldEntities().spawn({7.0F, 1.001F, 8.5F}, *pig, 3U);
    const std::uint64_t pigId = fixture.session.worldEntities().entities().back().id;
    REQUIRE(!fixture.session.openTradingContainer(pigId));
    REQUIRE(!fixture.menu().open());
    // An unemployed villager: no offers, so nothing to open.
    fixture.villager().villagerProfession = VillagerProfession::None;
    REQUIRE(!fixture.session.openTradingContainer(fixture.villagerId));
    REQUIRE(!fixture.menu().open());
    // And the employed one opens.
    fixture.villager().villagerProfession = VillagerProfession::Farmer;
    REQUIRE(fixture.session.openTradingContainer(fixture.villagerId));
    REQUIRE(fixture.menu().open());
    REQUIRE(fixture.session.openContainerScreen() == gameplay::ContainerScreen::Trading);
}

// The offer view carries everything a row needs, resolved — a UI never has to
// work out "locked" or "out of stock" for itself.
void testOfferViewCarriesTheWholeRow() {
    Fixture fixture;
    fixture.villager().villagerOfferUses[0] = 16U;   // first offer spent out
    REQUIRE(fixture.session.openTradingContainer(fixture.villagerId));
    const auto& menu = fixture.menu();
    REQUIRE(menu.offerCount == gameplay::entities::offersFor(VillagerProfession::Farmer).size());

    const auto& spent = menu.offers[0];
    REQUIRE(spent.wantsA.item == &gameplay::items::Wheat);
    REQUIRE(spent.wantsA.count == 20U);
    REQUIRE(spent.gives.item == &gameplay::items::Emerald);
    REQUIRE(spent.gives.count == 1U);
    REQUIRE(spent.uses == 16U && spent.maxUses == 16U);
    REQUIRE(spent.unlocked);       // level 1 has it
    REQUIRE(spent.outOfStock);     // but it is spent
    REQUIRE(!spent.selectable());

    // A level-2 offer is listed but locked for a novice.
    const auto& pumpkin = menu.offers[4];
    REQUIRE(pumpkin.level == 2U);
    REQUIRE(!pumpkin.unlocked);
    REQUIRE(!pumpkin.selectable());
    REQUIRE(!pumpkin.empty());     // still listed: vanilla greys, never hides

    // wantsB is empty for every offer in this build and is part of the view all
    // the same — the screen draws two payment slots regardless.
    REQUIRE(menu.offers[0].wantsB.empty());
}

// --- 2) selecting ----------------------------------------------------------

// Every index is legal to send. This is the promise that lets a UI forward a
// click without validating it first.
void testSelectingIsTotal() {
    Fixture fixture;
    REQUIRE(fixture.session.openTradingContainer(fixture.villagerId));
    REQUIRE(!fixture.menu().hasSelection());

    REQUIRE(fixture.session.selectTradeOffer(0U));
    REQUIRE(fixture.menu().hasSelection() && fixture.menu().selectedOffer == 0U);

    // Past the end: cleared, not an error.
    static_cast<void>(fixture.session.selectTradeOffer(999U));
    REQUIRE(!fixture.menu().hasSelection());
    // The explicit "none".
    REQUIRE(fixture.session.selectTradeOffer(0U));
    static_cast<void>(fixture.session.selectTradeOffer(gameplay::kNoTradeSelected));
    REQUIRE(!fixture.menu().hasSelection());
    // A locked row: legal to send, and it selects nothing.
    static_cast<void>(fixture.session.selectTradeOffer(4U));
    REQUIRE(!fixture.menu().hasSelection());
    // An out-of-stock row: likewise.
    fixture.villager().villagerOfferUses[1] = 16U;
    fixture.session.refreshTradingOffers();
    static_cast<void>(fixture.session.selectTradeOffer(1U));
    REQUIRE(!fixture.menu().hasSelection());
}

// --- 3) the derived result -------------------------------------------------

void testResultIsDerivedFromPayment() {
    Fixture fixture;
    REQUIRE(fixture.session.openTradingContainer(fixture.villagerId));
    REQUIRE(fixture.session.selectTradeOffer(0U));   // 20 wheat -> 1 emerald
    REQUIRE(fixture.menu().result.empty());          // nothing paid yet

    // Not enough.
    fixture.menu().paymentA = gameplay::ItemStack{world::Block::Air, 19U, &gameplay::items::Wheat};
    fixture.session.refreshTradingOffers();
    REQUIRE(fixture.menu().result.empty());

    // Exactly enough.
    fixture.menu().paymentA.count = 20U;
    fixture.session.refreshTradingOffers();
    REQUIRE(fixture.menu().result.item == &gameplay::items::Emerald);
    REQUIRE(fixture.menu().result.count == 1U);

    // The payment may sit in EITHER slot — vanilla's isRequiredItem checks both.
    fixture.menu().paymentA = {};
    fixture.menu().paymentB = gameplay::ItemStack{world::Block::Air, 32U, &gameplay::items::Wheat};
    fixture.session.refreshTradingOffers();
    REQUIRE(fixture.menu().result.item == &gameplay::items::Emerald);

    // Clearing the selection clears the result, without touching the payment.
    static_cast<void>(fixture.session.selectTradeOffer(gameplay::kNoTradeSelected));
    REQUIRE(fixture.menu().result.empty());
    REQUIRE(fixture.menu().paymentB.count == 32U);
}

// --- 4) taking the result --------------------------------------------------

void testTakingTheResultIsTheTrade() {
    Fixture fixture;
    REQUIRE(fixture.session.openTradingContainer(fixture.villagerId));
    REQUIRE(fixture.session.selectTradeOffer(0U));
    fixture.menu().paymentA = gameplay::ItemStack{world::Block::Air, 45U, &gameplay::items::Wheat};
    fixture.session.refreshTradingOffers();

    REQUIRE(fixture.session.takeTradeResult());
    // Paid 20, kept 25, got one emerald.
    REQUIRE(fixture.menu().paymentA.count == 25U);
    REQUIRE(countHeld(fixture.session.inventory(), &gameplay::items::Emerald) == 1U);
    // The use is counted and the villager earned the offer's experience.
    REQUIRE(fixture.villager().villagerOfferUses[0] == 1U);
    REQUIRE(fixture.villager().villagerTradeXp == 2);
    // The result re-derived itself from what is left: 25 wheat still covers it.
    REQUIRE(fixture.menu().result.item == &gameplay::items::Emerald);

    // Once the payment no longer covers the cost the result goes away, and
    // taking it does nothing at all.
    fixture.menu().paymentA.count = 5U;
    fixture.session.refreshTradingOffers();
    REQUIRE(fixture.menu().result.empty());
    REQUIRE(!fixture.session.takeTradeResult());
    REQUIRE(fixture.menu().paymentA.count == 5U);
    REQUIRE(fixture.villager().villagerOfferUses[0] == 1U);
}

// Enough trades raise the villager's level, which unlocks the next tier — end
// to end through the backend, since that loop is the whole point of the screen.
void testTradingUnlocksTheNextTier() {
    Fixture fixture;
    REQUIRE(fixture.session.openTradingContainer(fixture.villagerId));
    REQUIRE(!fixture.menu().offers[4].unlocked);   // the level-2 pumpkin offer

    REQUIRE(fixture.session.selectTradeOffer(0U));  // 20 wheat -> 1 emerald, xp 2
    for (int trade = 0; trade < 5; ++trade) {
        fixture.menu().paymentA =
            gameplay::ItemStack{world::Block::Air, 20U, &gameplay::items::Wheat};
        fixture.session.refreshTradingOffers();
        REQUIRE(fixture.session.takeTradeResult());
    }
    // 5 x 2 = 10 experience, exactly VillagerData's level-2 threshold.
    REQUIRE(fixture.villager().villagerTradeXp == 10);
    REQUIRE(fixture.villager().villagerLevel == 2U);
    // And the view followed without anyone asking it to.
    REQUIRE(fixture.menu().offers[4].unlocked);
    REQUIRE(fixture.menu().offers[4].selectable());
    // The level bar's two numbers are relative to the CURRENT level, not to
    // zero: at level 2 the floor is 10 and the next threshold is 70.
    REQUIRE(fixture.menu().villagerLevel == 2U);
    REQUIRE(fixture.menu().xpInLevel == 0);
    REQUIRE(fixture.menu().xpForNextLevel == 60);
}

// A full inventory must not swallow the payment. Asserted from both sides:
// nothing was taken and nothing was given.
void testFullInventoryCancelsTheTrade() {
    Fixture fixture;
    REQUIRE(fixture.session.openTradingContainer(fixture.villagerId));
    REQUIRE(fixture.session.selectTradeOffer(0U));
    fixture.menu().paymentA = gameplay::ItemStack{world::Block::Air, 20U, &gameplay::items::Wheat};
    // Fill every slot with something that cannot merge with an emerald.
    for (std::size_t slot = 0; slot < gameplay::Inventory::kSlotCount; ++slot) {
        fixture.session.inventory().mutableSlot(slot) =
            gameplay::ItemStack{world::Block::Stone, 64U};
    }
    fixture.session.refreshTradingOffers();
    REQUIRE(!fixture.menu().result.empty());   // the offer is payable...
    REQUIRE(!fixture.session.takeTradeResult());  // ...but there is nowhere to put it
    REQUIRE(fixture.menu().paymentA.count == 20U);
    REQUIRE(countHeld(fixture.session.inventory(), &gameplay::items::Emerald) == 0U);
    REQUIRE(fixture.villager().villagerOfferUses[0] == 0U);
}

// --- 5) the payments always come back --------------------------------------

void testClosingReturnsThePayments() {
    Fixture fixture;
    REQUIRE(fixture.session.openTradingContainer(fixture.villagerId));
    REQUIRE(fixture.session.selectTradeOffer(0U));
    fixture.menu().paymentA = gameplay::ItemStack{world::Block::Air, 20U, &gameplay::items::Wheat};
    fixture.session.refreshTradingOffers();
    REQUIRE(!fixture.menu().result.empty());

    fixture.session.closeContainerMenu();
    REQUIRE(!fixture.menu().open());
    REQUIRE(countHeld(fixture.session.inventory(), &gameplay::items::Wheat) == 20U);
    // The RESULT is not returned: it was derived, never owned, so returning it
    // would mint an emerald nobody paid for.
    REQUIRE(countHeld(fixture.session.inventory(), &gameplay::items::Emerald) == 0U);
}

// The villager dying with the screen open closes it and hands the payment back.
// The backend does this by itself, on the tick — a UI must not have to police it.
void testVillagerGoingAwayClosesTheScreen() {
    Fixture fixture;
    REQUIRE(fixture.session.openTradingContainer(fixture.villagerId));
    fixture.menu().paymentA = gameplay::ItemStack{world::Block::Air, 20U, &gameplay::items::Wheat};

    // Discarded, the way a detonating creeper is: gone from the world without a
    // death. The screen must survive the villager simply not being there any
    // more, whatever removed it.
    fixture.villager().discarded = true;
    fixture.tick();

    REQUIRE(!fixture.menu().open());
    REQUIRE(fixture.session.openContainerScreen() != gameplay::ContainerScreen::Trading);
    REQUIRE(countHeld(fixture.session.inventory(), &gameplay::items::Wheat) == 20U);
}

// --- 6) the wiring a UI actually touches -----------------------------------

// The three slots exist, in the right kinds, and only the two payments accept
// items. Without this the screen has nowhere to put anything.
void testScreenHandlerExposesTheThreeSlots() {
    Fixture fixture;
    REQUIRE(fixture.session.openTradingContainer(fixture.villagerId));
    gameplay::ScreenContext context;
    context.screen = gameplay::ContainerScreen::Trading;
    context.gameMode = gameplay::GameMode::Survival;
    const ui::HudLayout layout{1280.0F, 720.0F, 2};
    const auto slots = gameplay::ScreenHandler::buildSlots(fixture.session, context, layout);

    int payments = 0;
    int results = 0;
    for (const auto& slot : slots) {
        if (slot.kind == gameplay::SlotKind::TradePaymentA ||
            slot.kind == gameplay::SlotKind::TradePaymentB) {
            ++payments;
            REQUIRE(slot.acceptsItems());
            REQUIRE(slot.storage != nullptr);
        }
        if (slot.kind == gameplay::SlotKind::TradeResult) {
            ++results;
            REQUIRE(!slot.acceptsItems());
            // Derived: no storage behind it, exactly like a crafting output.
            REQUIRE(slot.storage == nullptr);
        }
    }
    REQUIRE(payments == 2);
    REQUIRE(results == 1);
    // The player's own 36 slots are there too, and on the trade screen's own
    // panel — not the 176-wide one, or they would sit on top of the payments.
    REQUIRE(layout.tradingInventorySlot(0U).x != layout.inventorySlot(0U).x);
}

// The page a UI builds carries seven clickable offer rows.
void testContainerPageCarriesTheOfferRows() {
    gameplay::ScreenContext context;
    context.screen = gameplay::ContainerScreen::Trading;
    context.gameMode = gameplay::GameMode::Survival;
    const ui::HudLayout layout{1280.0F, 720.0F, 2};
    ui::Page page;
    ui::buildContainerPageInto(page, context, layout);
    std::size_t offerButtons = 0U;
    for (const auto& widget : page) {
        if (widget.kind == ui::WidgetKind::Button &&
            widget.debugId == static_cast<std::uint16_t>(ui::WidgetId::TradeOffer)) {
            ++offerButtons;
        }
    }
    REQUIRE(offerButtons == ui::HudLayout::kTradingOfferButtons);
    REQUIRE(ui::containerPageKind(gameplay::ContainerScreen::Trading, gameplay::GameMode::Survival,
                                  true) == ui::ContainerPageKind::Trading);
}

// --- 7) the snapshot the UI reads ------------------------------------------

// Everything the screen draws rides the snapshot, so the render thread never
// reaches into gameplay. This is the assertion that the interface document's
// field table is true.
void testSnapshotCarriesTheWholeScreen() {
    Fixture fixture;
    fixture.villager().villagerOfferUses[2] = 16U;   // an out-of-stock row
    REQUIRE(fixture.session.openTradingContainer(fixture.villagerId));
    REQUIRE(fixture.session.selectTradeOffer(0U));
    fixture.menu().paymentA = gameplay::ItemStack{world::Block::Air, 20U, &gameplay::items::Wheat};
    fixture.tick();

    const auto& snapshot = fixture.session.worldSnapshot();
    REQUIRE(snapshot.tradeOfferCount ==
            gameplay::entities::offersFor(VillagerProfession::Farmer).size());
    REQUIRE(snapshot.tradeSelectedOffer == 0U);
    REQUIRE(snapshot.tradePaymentA.item == &gameplay::items::Wheat);
    REQUIRE(snapshot.tradePaymentA.count == 20U);
    REQUIRE(snapshot.tradeResult.item == &gameplay::items::Emerald);
    REQUIRE(snapshot.tradeWantsA[0].item == &gameplay::items::Wheat);
    REQUIRE(snapshot.tradeGives[0].item == &gameplay::items::Emerald);
    REQUIRE(snapshot.tradeOfferLevels[0] == 1U);
    REQUIRE(snapshot.tradeOfferMaxUses[0] == 16U);
    // The two flags the screen would otherwise have to derive.
    REQUIRE(snapshot.tradeOfferOutOfStock[2] == 1U);
    REQUIRE(snapshot.tradeOfferLocked[4] == 1U);
    REQUIRE(snapshot.tradeOfferLocked[0] == 0U);
    // The level bar.
    REQUIRE(snapshot.tradeVillagerLevel == 1U);
    REQUIRE(snapshot.tradeXpForNextLevel == 10);
    // Rows past the end are blank, so a screen that walks the whole array
    // without checking the count still draws nothing.
    REQUIRE(snapshot.tradeGives[gameplay::kSnapshotTradeOffers - 1U].empty());
}

// The select command survives the wire: the UI's click reaches the session
// through the same codec every other command uses.
void testSelectCommandRoundTripsTheWire() {
    const gameplay::GameCommand command = gameplay::SelectTradeOffer{3U};
    const auto bytes = gameplay::encodeGameCommand(command);
    const auto decoded = gameplay::decodeGameCommand(bytes);
    REQUIRE(decoded.has_value());
    REQUIRE(std::holds_alternative<gameplay::SelectTradeOffer>(*decoded));
    REQUIRE(std::get<gameplay::SelectTradeOffer>(*decoded).offerIndex == 3U);
}

} // namespace

int main() {
    gameplay::entities::registerBuiltinEntities();
    testOpeningRefusesWhatCannotBeTradedWith();
    testOfferViewCarriesTheWholeRow();
    testSelectingIsTotal();
    testResultIsDerivedFromPayment();
    testTakingTheResultIsTheTrade();
    testTradingUnlocksTheNextTier();
    testFullInventoryCancelsTheTrade();
    testClosingReturnsThePayments();
    testVillagerGoingAwayClosesTheScreen();
    testScreenHandlerExposesTheThreeSlots();
    testContainerPageCarriesTheOfferRows();
    testSnapshotCarriesTheWholeScreen();
    testSelectCommandRoundTripsTheWire();
    return 0;
}

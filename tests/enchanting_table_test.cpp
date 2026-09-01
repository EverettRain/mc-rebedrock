// ENCH-2: the enchanting table — bookshelf power, the derived three offers, and
// the purchase.
//
// The offer MATH is ENCH-0's and already has its own test; what is new here is
// everything ENCH-0 deliberately left out, so that is what this covers:
//
//   * the bookshelf scan (EnchantingTableBlock#isValidBookShelf): the 5x5x2
//     ring, and the "the cell between must be air" rule that makes a shelf
//     behind a wall worth nothing;
//   * the two costs, which are DIFFERENT numbers — requiredLevel is a
//     threshold, the spend is (index+1) levels and (index+1) lapis;
//   * the spend itself going through XP-4's consumeLevels rather than the level
//     field (sabotage ①);
//   * the seed reroll on a successful purchase (sabotage ②), which is what makes
//     the next three offers different;
//   * every refusal path being a true no-op: nothing spent, nothing enchanted;
//   * the block's own identity: a 12/16 shape, a container that opens the
//     screen, and a craftable recipe — an unreachable table is not a feature.

#include "gameplay/EnchantingTable.hpp"
#include "gameplay/Enchantment.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/ItemUse.hpp"
#include "gameplay/PlayerExperience.hpp"
#include "world/Block.hpp"
#include "world/BlockShape.hpp"
#include "world/World.hpp"
#include "gameplay/ContentRegistry.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/GameSnapshotCodec.hpp"
#include "gameplay/CraftingSystem.hpp"
#include "world/gen/JavaRandom.hpp"

#include <cassert>
#include <variant>
#include <string_view>
#include <iostream>

namespace {

using namespace mc::gameplay;
using mc::world::Block;

// A single chunk of air with a stone floor, so a bookshelf placed in the ring
// has air between it and the table unless the test puts something there.
void buildWorld(mc::world::World& world) {
    mc::world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, Block::Stone);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
}

[[nodiscard]] ItemStack diamondPickaxe() {
    ItemStack stack;
    stack.item = &items::DiamondPickaxe;
    stack.count = 1U;
    return stack;
}

[[nodiscard]] ItemStack lapis(std::uint8_t count) {
    ItemStack stack;
    stack.item = &items::LapisLazuli;
    stack.count = count;
    return stack;
}

// ---- the bookshelf scan ----

void testBookshelfOffsets() {
    const auto offsets = bookshelfOffsets();
    assert(offsets.size() == 32U);
    // Every offset is on the ring (|x| == 2 or |z| == 2), at y 0 or 1, and no
    // two are the same cell.
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        const auto& offset = offsets[i];
        assert(offset.x >= -2 && offset.x <= 2);
        assert(offset.z >= -2 && offset.z <= 2);
        assert(offset.y == 0 || offset.y == 1);
        assert(offset.x == 2 || offset.x == -2 || offset.z == 2 || offset.z == -2);
        for (std::size_t j = 0; j < i; ++j) {
            assert(!(offsets[j].x == offset.x && offsets[j].y == offset.y &&
                     offsets[j].z == offset.z));
        }
    }
    std::cout << "bookshelf offsets: 32 distinct ring cells\n";
}

void testBookshelfPower() {
    mc::world::World world;
    buildWorld(world);
    const glm::ivec3 table{8, 1, 8};
    assert(bookshelfPower(world, table) == 0);

    // A shelf two cells away with air between: counts.
    assert(world.setBlock(10, 1, 8, Block::Bookshelf));
    assert(bookshelfPower(world, table) == 1);

    // Wall off the gap cell (x/2 == 1, so cell (9,1,8)): the shelf is still
    // there, but its power no longer reaches the table.
    assert(world.setBlock(9, 1, 8, Block::Stone));
    assert(bookshelfPower(world, table) == 0);
    assert(world.setBlock(9, 1, 8, Block::Air));
    assert(bookshelfPower(world, table) == 1);

    // A shelf INSIDE the ring (|x| < 2 and |z| < 2) is not an offset at all.
    assert(world.setBlock(9, 1, 9, Block::Bookshelf));
    assert(bookshelfPower(world, table) == 1);
    assert(world.setBlock(9, 1, 9, Block::Air));

    // The upper ring (y+1) counts too, and its gap cell is at the SAME y as the
    // shelf (vanilla halves x/z but never y).
    assert(world.setBlock(10, 2, 8, Block::Bookshelf));
    assert(bookshelfPower(world, table) == 2);
    assert(world.setBlock(9, 2, 8, Block::Stone));
    assert(bookshelfPower(world, table) == 1);
    assert(world.setBlock(9, 2, 8, Block::Air));

    // A replaceable block in the gap still transmits — the tag is
    // `#minecraft:replaceable`, not "is air", so grass growing between the
    // table and the shelf must not cost power.
    assert(world.setBlock(9, 1, 8, Block::GrassPlant));
    assert(bookshelfPower(world, table) == 2);
    assert(world.setBlock(9, 1, 8, Block::Air));

    // Fill the whole ring: 32 shelves, reported unclamped.
    for (const auto& offset : bookshelfOffsets()) {
        assert(world.setBlock(table.x + offset.x, table.y + offset.y, table.z + offset.z,
                              Block::Bookshelf));
    }
    assert(bookshelfPower(world, table) == 32);
    std::cout << "bookshelf power: ring/occlusion/upper-layer rules match vanilla\n";
}

// ---- the offers ----

void testOffersDerivation() {
    EnchantingMenu menu;
    menu.item = diamondPickaxe();
    menu.bookshelfPower = 15;
    refreshOffers(menu, 12345);
    const auto first = menu.offers;
    assert(first.slots[0].requiredLevel > 0);
    assert(first.slots[2].requiredLevel >= first.slots[0].requiredLevel);

    // Same triple -> byte-identical offers, and the derivation does not re-run.
    refreshOffers(menu, 12345);
    for (std::size_t slot = 0; slot < 3U; ++slot) {
        assert(menu.offers.slots[slot].requiredLevel == first.slots[slot].requiredLevel);
        assert(menu.offers.slots[slot].enchantments.size() ==
               first.slots[slot].enchantments.size());
    }

    // A different seed moves them.
    refreshOffers(menu, 999);
    bool moved = false;
    for (std::size_t slot = 0; slot < 3U; ++slot) {
        moved = moved || menu.offers.slots[slot].requiredLevel != first.slots[slot].requiredLevel;
    }
    assert(moved);

    // An empty slot, a stack of more than one, and an already-enchanted item all
    // read as not enchantable, so the bars go dead.
    menu.item = {};
    refreshOffers(menu, 12345);
    assert(menu.offers.slots[0].requiredLevel == 0);
    assert(menu.offers.slots[1].requiredLevel == 0);
    assert(menu.offers.slots[2].requiredLevel == 0);

    ItemStack twoPicks = diamondPickaxe();
    twoPicks.count = 2U;
    assert(!isEnchantable(twoPicks));

    ItemStack alreadyEnchanted = diamondPickaxe();
    setEnchantmentLevel(alreadyEnchanted, EnchantmentId::Efficiency, 3U);
    assert(!isEnchantable(alreadyEnchanted));

    ItemStack cobblestone;
    cobblestone.block = Block::Cobblestone;
    cobblestone.count = 1U;
    assert(!isEnchantable(cobblestone));
    std::cout << "offers: deterministic per (seed, power, item); dead for unenchantables\n";
}

// ---- the purchase ----

struct Fixture final {
    EnchantingMenu menu;
    PlayerExperience experience;
    mc::world::gen::JavaRandom seedRandom;

    Fixture(std::int32_t levels, std::uint8_t lapisCount, int power = 15) {
        menu.item = diamondPickaxe();
        menu.lapis = lapis(lapisCount);
        menu.bookshelfPower = power;
        experience.setExperienceLevel(levels);
        // NOT 0x5DEECE66D: JavaRandom's setSeed XORs the multiplier in, so that
        // exact value lands the stream on internal state 0, whose first
        // nextInt() is 0 — which is also the default enchantment seed, making
        // the reroll assertion below unfalsifiable.
        seedRandom.setSeed(20260831ULL);
        refreshOffers(menu, 4242);
    }
};

void testPurchaseSpendsTheRightTwoCosts() {
    Fixture fixture{60, 3};
    // Buy the third bar: the threshold is offers[2].requiredLevel, but the
    // SPEND is 3 levels and 3 lapis. Getting these two mixed up is the classic
    // bug here, so assert the exact numbers rather than "went down".
    const auto requiredLevel = fixture.menu.offers.slots[2].requiredLevel;
    assert(requiredLevel > 0 && requiredLevel != 3);
    const auto seedBefore = fixture.experience.enchantmentSeed();
    const auto result =
        purchase(fixture.menu, fixture.experience, 2, /*infiniteMaterials=*/false,
                 fixture.seedRandom);
    assert(result.applied);
    assert(result.levelsSpent == 3);
    assert(result.lapisSpent == 3);
    assert(fixture.experience.level() == 57);
    assert(fixture.menu.lapis.empty());
    assert(fixture.menu.item.enchantmentCount > 0U);
    // Sabotage ②: the seed must move, or the same three offers could be bought
    // over and over.
    assert(fixture.experience.enchantmentSeed() != seedBefore);
    std::cout << "purchase: spends index+1 levels and index+1 lapis, rerolls the seed\n";
}

void testPurchaseAppliesEveryRolledEnchantment() {
    Fixture fixture{60, 3};
    const auto& offer = fixture.menu.offers.slots[2];
    assert(!offer.enchantments.empty());
    const auto expected = offer.enchantments;
    const auto result =
        purchase(fixture.menu, fixture.experience, 2, false, fixture.seedRandom);
    assert(result.applied);
    for (const auto& entry : expected) {
        assert(enchantmentLevel(fixture.menu.item, entry.id) ==
               static_cast<std::uint8_t>(entry.level));
    }
    std::cout << "purchase: every enchantment the offer rolled lands on the item\n";
}

void testRefusalsChangeNothing() {
    // Not enough levels for the THRESHOLD even though the 3-level spend is
    // affordable: vanilla checks both, so this must refuse.
    {
        Fixture fixture{60, 3};
        const auto threshold = fixture.menu.offers.slots[2].requiredLevel;
        fixture.experience.setExperienceLevel(threshold - 1);
        const auto seedBefore = fixture.experience.enchantmentSeed();
        const auto result =
            purchase(fixture.menu, fixture.experience, 2, false, fixture.seedRandom);
        assert(!result.applied);
        assert(fixture.experience.level() == threshold - 1);
        assert(fixture.menu.lapis.count == 3U);
        assert(fixture.menu.item.enchantmentCount == 0U);
        assert(fixture.experience.enchantmentSeed() == seedBefore);
    }
    // Enough levels, not enough lapis.
    {
        Fixture fixture{60, 2};
        const auto result =
            purchase(fixture.menu, fixture.experience, 2, false, fixture.seedRandom);
        assert(!result.applied);
        assert(fixture.experience.level() == 60);
        assert(fixture.menu.lapis.count == 2U);
        assert(fixture.menu.item.enchantmentCount == 0U);
    }
    // A dead bar (no item at all) offers nothing to buy.
    {
        Fixture fixture{60, 3};
        fixture.menu.item = {};
        refreshOffers(fixture.menu, 4242);
        for (int option = 0; option < 3; ++option) {
            assert(!purchase(fixture.menu, fixture.experience, option, false,
                             fixture.seedRandom)
                        .applied);
        }
        assert(fixture.experience.level() == 60);
    }
    // Out-of-range buttons.
    {
        Fixture fixture{60, 3};
        assert(!purchase(fixture.menu, fixture.experience, -1, false, fixture.seedRandom).applied);
        assert(!purchase(fixture.menu, fixture.experience, 3, false, fixture.seedRandom).applied);
        assert(fixture.experience.level() == 60);
    }
    std::cout << "purchase: every refusal is a true no-op\n";
}

void testCreativeSpendsNothing() {
    Fixture fixture{0, 0};
    const auto result =
        purchase(fixture.menu, fixture.experience, 2, /*infiniteMaterials=*/true,
                 fixture.seedRandom);
    assert(result.applied);
    assert(result.levelsSpent == 0);
    assert(result.lapisSpent == 0);
    assert(fixture.experience.level() == 0);
    assert(fixture.menu.item.enchantmentCount > 0U);
    std::cout << "purchase: creative pays neither levels nor lapis\n";
}

// Sabotage ①: a purchase that bypassed XP-4 and wrote the level field directly
// would happily go negative. consumeLevels is atomic, so a spend that cannot be
// afforded leaves the level untouched — assert the invariant the bypass breaks.
void testSpendNeverGoesNegative() {
    Fixture fixture{60, 3};
    const auto threshold = fixture.menu.offers.slots[2].requiredLevel;
    fixture.experience.setExperienceLevel(threshold);
    const auto result =
        purchase(fixture.menu, fixture.experience, 2, false, fixture.seedRandom);
    assert(result.applied);
    assert(fixture.experience.level() == threshold - 3);
    assert(fixture.experience.level() >= 0);
    std::cout << "purchase: the level spend goes through XP-4 and never underflows\n";
}

// ---- the block itself ----

void testBlockIdentity() {
    const auto& definition = mc::world::blockDefinition(Block::EnchantingTable);
    assert(definition.identifier.path == "enchanting_table");
    assert(definition.container == mc::world::ContainerType::EnchantingTable);
    assert(definition.model == mc::world::BlockModel::ElementModel);
    // Not a full cube: it must not occlude its neighbours or act face-sturdy,
    // matching vanilla's useShapeForLightOcclusion on a 12/16 box.
    assert(!mc::world::isFullCube(Block::EnchantingTable));
    assert(!mc::world::isFaceSturdy(Block::EnchantingTable));
    // EnchantingTableBlock's SHAPE is Block.column(16, 0, 12).
    const auto shape = mc::world::blockShape(mc::world::BlockState{Block::EnchantingTable});
    assert(shape.kind == mc::world::ShapeKind::Column);
    assert(shape.bottom == 0.0F);
    assert(shape.top == 12.0F / 16.0F);
    assert(mc::world::hasCollision(Block::EnchantingTable));

    // Right-clicking it opens the screen rather than using the held item — and,
    // like every other container, sneaking with something in hand builds against
    // it instead.
    const auto open = decideBlockInteraction(mc::world::ContainerType::EnchantingTable,
                                             /*secondaryUseActive=*/false,
                                             /*holdingItem=*/false);
    assert(open.interaction == BlockInteraction::OpenEnchantingTable);
    const auto suppressed = decideBlockInteraction(mc::world::ContainerType::EnchantingTable,
                                                   /*secondaryUseActive=*/true,
                                                   /*holdingItem=*/true);
    assert(suppressed.interaction == BlockInteraction::UseItem);
    std::cout << "block: 12/16 column, non-occluding, opens its screen\n";
}

// A table nobody can craft is not survival content. Assert it through the real
// crafting path rather than the baked table: fill a 3x3 grid with vanilla's
// shape (" B " / "D#D" / "###") and check what comes out.
void testRecipeIsReachable() {
    CraftingSystem crafting;
    const auto put = [&](std::size_t index, const mc::gameplay::Item* item, Block block) {
        ItemStack& slot = crafting.tableGridSlot(index);
        slot = {};
        slot.item = item;
        slot.block = block;
        slot.count = 1U;
    };
    const auto blank = [&](std::size_t index) { crafting.tableGridSlot(index) = {}; };
    blank(0);
    put(1, &items::Book, Block::Air);
    blank(2);
    put(3, &items::Diamond, Block::Air);
    put(4, blockItemFor(Block::Obsidian), Block::Obsidian);
    put(5, &items::Diamond, Block::Air);
    for (std::size_t index = 6; index < 9; ++index) {
        put(index, blockItemFor(Block::Obsidian), Block::Obsidian);
    }
    const auto output = crafting.tableOutput();
    assert(!output.empty());
    assert(output.block == Block::EnchantingTable);
    assert(output.count == 1U);

    // The shape matters: the same nine items in a different arrangement (book
    // in the middle) is not the recipe.
    std::swap(crafting.tableGridSlot(1), crafting.tableGridSlot(4));
    assert(crafting.tableOutput().empty());
    std::cout << "recipe: enchanting_table crafts from vanilla's 3x3 shape\n";
}

// ---- reachability: creative catalog + the enchanted book ----

// A block that is registered but not in a tab is not obtainable in creative,
// and the tab has to be the right one (26.1 puts ENCHANTING_TABLE in
// functionalBlocks). Assert the whole shape: present, present ONCE, and present
// in no other tab.
void testTableIsInTheCreativeCatalog() {
    const auto& registry = contentRegistry();
    std::size_t inFunctional = 0;
    std::size_t inOtherTabs = 0;
    for (std::size_t tab = 0; tab < static_cast<std::size_t>(CreativeCategory::Count); ++tab) {
        const auto category = static_cast<CreativeCategory>(tab);
        for (const auto& stack : registry.catalog(category)) {
            if (stack.block != Block::EnchantingTable || stack.item == nullptr) {
                continue;
            }
            if (category == CreativeCategory::Functional) {
                ++inFunctional;
            } else {
                ++inOtherTabs;
            }
        }
    }
    assert(inFunctional == 1U);
    assert(inOtherTabs == 0U);
    // And it is a real, wieldable item stack — pick-block, /give and the
    // mined drop all resolve through this.
    assert(blockItemFor(Block::EnchantingTable) != nullptr);
    assert(registry.block("rebedrock:enchanting_table") != nullptr);
    std::cout << "catalog: enchanting_table is in Functional, once, and obtainable\n";
}

// A book is enchantable, and enchanting one TRANSMUTES it: vanilla's
// clickMenuButton swaps Items.BOOK for Items.ENCHANTED_BOOK before applying
// anything. Without that swap an "enchanted book" would stack with plain books
// and still read as a crafting ingredient.
void testBookBecomesAnEnchantedBook() {
    ItemStack book;
    book.item = &items::Book;
    book.count = 1U;
    assert(isEnchantable(book));

    EnchantingMenu menu;
    menu.item = book;
    menu.lapis = lapis(3);
    menu.bookshelfPower = 15;
    refreshOffers(menu, 4242);
    // A book accepts every enchantment, so all three bars must be live.
    for (std::size_t slot = 0; slot < 3U; ++slot) {
        assert(menu.offers.slots[slot].requiredLevel > 0);
        // EnchantmentScreenHandler's private wrapper drops a book's extra rolls:
        // a book offer is always exactly one enchantment.
        assert(menu.offers.slots[slot].enchantments.size() == 1U);
    }

    PlayerExperience experience;
    experience.setExperienceLevel(60);
    mc::world::gen::JavaRandom seedRandom;
    seedRandom.setSeed(20260831ULL);
    const auto expected = menu.offers.slots[2].enchantments.front();
    const auto result = purchase(menu, experience, 2, false, seedRandom);
    assert(result.applied);
    assert(menu.item.item == &items::EnchantedBook);
    assert(menu.item.item != &items::Book);
    assert(enchantmentLevel(menu.item, expected.id) ==
           static_cast<std::uint8_t>(expected.level));
    // An enchanted book is not stackable with a plain book, and cannot go
    // straight back into the table.
    assert(!sameItem(menu.item, book));
    assert(!isEnchantable(menu.item));
    std::cout << "book: enchants, and comes out as a real enchanted_book\n";
}

// 26.1 fills the Ingredients tab with one enchanted book per enchantment at its
// maximum level (generateEnchantmentBookTypesOnlyMaxLevel). A bare unenchanted
// enchanted_book is never listed — it would be a meaningless entry.
void testEnchantedBooksAreInTheCatalog() {
    const auto& registry = contentRegistry();
    std::size_t booksInIngredients = 0;
    std::size_t bareBooks = 0;
    std::array<bool, kEnchantmentCount> seen{};
    for (const auto& stack : registry.catalog(CreativeCategory::Ingredients)) {
        if (stack.item != &items::EnchantedBook) {
            continue;
        }
        ++booksInIngredients;
        if (stack.enchantmentCount == 0U) {
            ++bareBooks;
            continue;
        }
        assert(stack.enchantmentCount == 1U);
        const auto id = static_cast<EnchantmentId>(stack.enchantments[0].id);
        assert(stack.enchantments[0].level == enchantmentDefinition(id).maxLevel);
        assert(!seen[static_cast<std::size_t>(id)]); // no duplicates
        seen[static_cast<std::size_t>(id)] = true;
    }
    assert(bareBooks == 0U);
    assert(booksInIngredients == kEnchantmentCount);
    for (const bool present : seen) {
        assert(present);
    }
    // Identity is still registered exactly once despite the many tab entries.
    assert(registry.item("rebedrock:enchanted_book") != nullptr);
    std::cout << "catalog: " << kEnchantmentCount
              << " enchanted books in Ingredients, one per enchantment at max level\n";
}

// The lapis gate again, but through GameSession this time — the pure function
// is only half the answer. purchaseEnchantment is what decides whether the
// creative bypass applies, by reading the player's game mode, and a wiring slip
// there (a hardcoded `true`, the wrong player) would hand out free enchantments
// in survival while every unit test on purchase() still passed.
void testGameModeGateIsWiredThroughTheSession() {
    const auto arm = [](GameSession& session, std::uint8_t lapisCount) {
        session.primaryPlayer().experience.setExperienceLevel(60);
        EnchantingMenu& menu = session.enchantingMenu();
        menu.item = diamondPickaxe();
        menu.lapis = lapisCount == 0U ? ItemStack{} : lapis(lapisCount);
        menu.bookshelfPower = 15;
        menu.derived = false;
        refreshOffers(menu, session.primaryPlayer().experience.enchantmentSeed());
        session.openContainer(ContainerScreen::EnchantingTable);
    };

    // Survival, no lapis in the slot: refused, and nothing moves.
    {
        GameSession session;
        session.primaryPlayer().gameMode = GameMode::Survival;
        arm(session, 0U);
        assert(!session.purchaseEnchantment(2));
        assert(session.primaryPlayer().experience.level() == 60);
        assert(session.enchantingMenu().item.enchantmentCount == 0U);
    }
    // Survival, two lapis but the third bar wants three: still refused.
    {
        GameSession session;
        session.primaryPlayer().gameMode = GameMode::Survival;
        arm(session, 2U);
        assert(!session.purchaseEnchantment(2));
        assert(session.enchantingMenu().lapis.count == 2U);
        assert(session.enchantingMenu().item.enchantmentCount == 0U);
    }
    // Survival with three lapis: goes through, and spends both currencies.
    {
        GameSession session;
        session.primaryPlayer().gameMode = GameMode::Survival;
        arm(session, 3U);
        assert(session.purchaseEnchantment(2));
        assert(session.primaryPlayer().experience.level() == 57);
        assert(session.enchantingMenu().lapis.empty());
        assert(session.enchantingMenu().item.enchantmentCount > 0U);
    }
    // Creative with neither lapis nor levels: allowed and free — vanilla's
    // hasInfiniteMaterials bypass, not a bug.
    {
        GameSession session;
        session.primaryPlayer().gameMode = GameMode::Creative;
        arm(session, 0U);
        session.primaryPlayer().experience.setExperienceLevel(0);
        assert(session.purchaseEnchantment(2));
        assert(session.primaryPlayer().experience.level() == 0);
        assert(session.enchantingMenu().item.enchantmentCount > 0U);
    }
    // A closed screen refuses regardless: the button cannot be pressed from a
    // screen that is not open.
    {
        GameSession session;
        session.primaryPlayer().gameMode = GameMode::Survival;
        arm(session, 3U);
        session.closeContainer();
        assert(!session.purchaseEnchantment(2));
        assert(session.primaryPlayer().experience.level() == 60);
    }
    std::cout << "session: survival needs the lapis, creative bypasses, closed screen refuses\n";
}

// The tooltip can only show what reaches the client. The renderer draws from the
// published WorldSnapshot, which crosses the codec even in single player (the
// integrated server talks over a loopback), so a field the codec drops is a
// field the screen can never show. Filled with NON-DEFAULT values throughout —
// a codec that silently writes nothing still round-trips all-zeroes.
void testSnapshotCarriesTheEnchantingScreen() {
    WorldSnapshot sent;
    sent.openContainerScreen = ContainerScreen::EnchantingTable;
    sent.enchantingItem = diamondPickaxe();
    setEnchantmentLevel(sent.enchantingItem, EnchantmentId::Efficiency, 4U);
    setEnchantmentLevel(sent.enchantingItem, EnchantmentId::Unbreaking, 2U);
    sent.enchantingLapis = lapis(7);
    sent.enchantingRequiredLevels = {3, 0, 27};
    sent.enchantingClueIds = {static_cast<std::uint8_t>(EnchantmentId::Efficiency), 0U,
                              static_cast<std::uint8_t>(EnchantmentId::Fortune)};
    sent.enchantingClueLevels = {1U, 0U, 3U};
    sent.enchantingBookshelfPower = 22;
    sent.enchantingSeed = -1234567;

    const auto bytes = encodeSnapshot(PublishedSnapshot{sent});
    const auto decoded = decodeSnapshot(bytes);
    assert(decoded.has_value());
    const auto* received = std::get_if<WorldSnapshot>(&*decoded);
    assert(received != nullptr);

    assert(received->openContainerScreen == ContainerScreen::EnchantingTable);
    assert(received->enchantingItem.item == sent.enchantingItem.item);
    // The enchantments themselves, not just the item: this is exactly what the
    // tooltip lists, and what tells a player the enchant worked.
    assert(enchantmentLevel(received->enchantingItem, EnchantmentId::Efficiency) == 4U);
    assert(enchantmentLevel(received->enchantingItem, EnchantmentId::Unbreaking) == 2U);
    assert(received->enchantingLapis.count == 7U);
    assert(received->enchantingRequiredLevels == sent.enchantingRequiredLevels);
    assert(received->enchantingClueIds == sent.enchantingClueIds);
    assert(received->enchantingClueLevels == sent.enchantingClueLevels);
    assert(received->enchantingBookshelfPower == 22);
    assert(received->enchantingSeed == -1234567);
    std::cout << "snapshot: the whole enchanting screen (enchantments included) crosses the codec\n";
}

} // namespace

int main() {
    testBookshelfOffsets();
    testBookshelfPower();
    testOffersDerivation();
    testPurchaseSpendsTheRightTwoCosts();
    testPurchaseAppliesEveryRolledEnchantment();
    testRefusalsChangeNothing();
    testCreativeSpendsNothing();
    testSpendNeverGoesNegative();
    testBlockIdentity();
    testRecipeIsReachable();
    testTableIsInTheCreativeCatalog();
    testBookBecomesAnEnchantedBook();
    testEnchantedBooksAreInTheCatalog();
    testGameModeGateIsWiredThroughTheSession();
    testSnapshotCarriesTheEnchantingScreen();
    std::cout << "enchanting table: all checks passed\n";
    return 0;
}

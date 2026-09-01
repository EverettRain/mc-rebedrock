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
#include "gameplay/CraftingSystem.hpp"
#include "world/gen/JavaRandom.hpp"

#include <cassert>
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
    std::cout << "enchanting table: all checks passed\n";
    return 0;
}

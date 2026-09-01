// ENCH-3: the anvil — material repair, combining, the enchantment merge rules,
// and the two things that make the anvil an economy rather than a free lunch:
// the prior-work penalty and the 40-level wall.
//
// Every number here was checked against 26.1's AnvilMenu#createResult by hand
// before being written down, not read back out of the implementation:
//
//   * an iron pickaxe (250 durability) at 200 damage takes three ingots — each
//     mends min(damage, max/4) = 62 — landing at 14 damage for 3 levels;
//   * combining it with one at 100 damage gives remaining 50 + 150 + 12% of 250
//     = 230, i.e. 20 damage, for 2 levels;
//   * Efficiency's anvil cost is 1 (rarity Common), halved-but-floored-at-1 for
//     a book, so Efficiency IV off a book is 4 levels.
//
// The prior-work penalty is the sabotage target: without it an item could be
// worked forever at the same price.

#include "gameplay/Anvil.hpp"
#include "gameplay/Enchantment.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/PlayerExperience.hpp"
#include "gameplay/ContentRegistry.hpp"
#include "gameplay/CraftingSystem.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/GameSnapshotCodec.hpp"
#include "gameplay/ItemUse.hpp"
#include "gameplay/entities/ExperienceOrb.hpp"
#include "world/BlockShape.hpp"

#include <cassert>
#include <utility>
#include <variant>
#include <iostream>

namespace {

using namespace mc::gameplay;

[[nodiscard]] ItemStack tool(const Item* item, std::uint16_t damage = 0U) {
    ItemStack stack;
    stack.item = item;
    stack.count = 1U;
    stack.damage = damage;
    return stack;
}

[[nodiscard]] ItemStack stackOf(const Item* item, std::uint8_t count) {
    return ItemStack{mc::world::Block::Air, count, item};
}

[[nodiscard]] ItemStack enchantedBook(EnchantmentId id, std::uint8_t level) {
    ItemStack book = tool(&items::EnchantedBook);
    setEnchantmentLevel(book, id, level);
    return book;
}

// ---- the anvil cost table ----

void testAnvilCostFollowsRarity() {
    // 26.1 carries anvil_cost per enchantment in the datapack, but across the
    // whole vanilla enchantment directory it is a pure function of the rarity
    // weight — 10/5/2/1 -> 1/2/4/8, no exceptions. Guard the mapping so a
    // future rarity edit cannot silently reprice every anvil operation.
    assert(enchantmentAnvilCost(EnchantmentId::Efficiency) == 1);   // Common
    assert(enchantmentAnvilCost(EnchantmentId::Unbreaking) == 2);   // Uncommon
    assert(enchantmentAnvilCost(EnchantmentId::Fortune) == 4);      // Rare
    assert(enchantmentAnvilCost(EnchantmentId::SilkTouch) == 8);    // VeryRare
    std::cout << "anvil cost: derives from rarity, 1/2/4/8\n";
}

void testRepairCostDoubles() {
    // 2n+1, and the sixth trip is already past the wall.
    assert(increasedRepairCost(0U) == 1U);
    assert(increasedRepairCost(1U) == 3U);
    assert(increasedRepairCost(3U) == 7U);
    assert(increasedRepairCost(7U) == 15U);
    assert(increasedRepairCost(15U) == 31U);
    assert(increasedRepairCost(31U) == 63U);
    assert(increasedRepairCost(63U) > kAnvilMaximumCost);
    // Saturates rather than wrapping — a wrapped penalty would make a
    // heavily-worked item cheap again.
    assert(increasedRepairCost(255U) == 255U);
    assert(increasedRepairCost(200U) == 255U);
    std::cout << "prior work: 2n+1, saturating, past the wall by the sixth trip\n";
}

// ---- material repair ----

void testMaterialRepair() {
    AnvilMenu menu;
    menu.left = tool(&items::IronPickaxe, 200U);
    menu.right = stackOf(&items::IronIngot, 3U);
    refreshAnvilResult(menu, /*infiniteMaterials=*/false);
    // Three ingots at 62 damage each: 200 -> 138 -> 76 -> 14, one level apiece.
    assert(menu.result.item == &items::IronPickaxe);
    assert(menu.result.damage == 14U);
    assert(menu.cost == 3);
    assert(menu.repairItemCountCost == 3U);

    // Only as many ingots as the repair actually needs are charged for.
    AnvilMenu fewer;
    fewer.left = tool(&items::IronPickaxe, 60U);
    fewer.right = stackOf(&items::IronIngot, 5U);
    refreshAnvilResult(fewer, false);
    assert(fewer.result.damage == 0U);
    assert(fewer.cost == 1);
    assert(fewer.repairItemCountCost == 1U);

    // An undamaged item has nothing to sell.
    AnvilMenu pristine;
    pristine.left = tool(&items::IronPickaxe, 0U);
    pristine.right = stackOf(&items::IronIngot, 3U);
    refreshAnvilResult(pristine, false);
    assert(pristine.result.empty());
    assert(pristine.cost == 0);

    // The wrong material is not a repair — and, not being another pickaxe
    // either, not a combine, so nothing comes out.
    AnvilMenu wrong;
    wrong.left = tool(&items::IronPickaxe, 200U);
    wrong.right = stackOf(&items::GoldIngot, 3U);
    refreshAnvilResult(wrong, false);
    assert(wrong.result.empty());
    std::cout << "repair: per-ingot quarter mend, charges only what it uses\n";
}

// ---- combining two of the same item ----

void testCombineDurability() {
    AnvilMenu menu;
    menu.left = tool(&items::IronPickaxe, 200U);
    menu.right = tool(&items::IronPickaxe, 100U);
    refreshAnvilResult(menu, false);
    // remaining 50 + 150 + 12% of 250 (=30) = 230 -> damage 20.
    assert(menu.result.damage == 20U);
    assert(menu.cost == 2);

    // Two different items with no book: refused.
    AnvilMenu mismatched;
    mismatched.left = tool(&items::IronPickaxe, 200U);
    mismatched.right = tool(&items::IronAxe, 100U);
    refreshAnvilResult(mismatched, false);
    assert(mismatched.result.empty());
    std::cout << "combine: durability adds with the 12% bonus, 2 levels\n";
}

// ---- the enchantment merge rules ----

void testEnchantmentMerge() {
    // Equal levels merge upward by one.
    {
        AnvilMenu menu;
        menu.left = tool(&items::DiamondPickaxe, 10U);
        setEnchantmentLevel(menu.left, EnchantmentId::Efficiency, 2U);
        menu.right = enchantedBook(EnchantmentId::Efficiency, 2U);
        refreshAnvilResult(menu, false);
        assert(enchantmentLevel(menu.result, EnchantmentId::Efficiency) == 3U);
    }
    // Different levels take the higher, not the sum.
    {
        AnvilMenu menu;
        menu.left = tool(&items::DiamondPickaxe, 10U);
        setEnchantmentLevel(menu.left, EnchantmentId::Efficiency, 4U);
        menu.right = enchantedBook(EnchantmentId::Efficiency, 2U);
        refreshAnvilResult(menu, false);
        assert(enchantmentLevel(menu.result, EnchantmentId::Efficiency) == 4U);
    }
    // Merging two maxed levels does not exceed the enchantment's maximum.
    {
        const auto maximum = enchantmentDefinition(EnchantmentId::Efficiency).maxLevel;
        AnvilMenu menu;
        menu.left = tool(&items::DiamondPickaxe, 10U);
        setEnchantmentLevel(menu.left, EnchantmentId::Efficiency,
                            static_cast<std::uint8_t>(maximum));
        menu.right = enchantedBook(EnchantmentId::Efficiency,
                                   static_cast<std::uint8_t>(maximum));
        refreshAnvilResult(menu, false);
        assert(enchantmentLevel(menu.result, EnchantmentId::Efficiency) ==
               static_cast<std::uint8_t>(maximum));
    }
    // A book's fee is halved but floored at 1: Efficiency IV off a book is 4.
    {
        AnvilMenu menu;
        menu.left = tool(&items::DiamondPickaxe, 50U);
        menu.right = enchantedBook(EnchantmentId::Efficiency, 4U);
        refreshAnvilResult(menu, false);
        assert(enchantmentLevel(menu.result, EnchantmentId::Efficiency) == 4U);
        assert(menu.cost == 4);
    }
    std::cout << "merge: equal levels +1, unequal take the higher, capped, book fee halved\n";
}

void testIncompatibleEnchantments() {
    // Silk Touch onto a pickaxe that already has Fortune: they conflict, so it
    // is refused — and, being the only offer, the whole operation produces
    // nothing.
    AnvilMenu menu;
    menu.left = tool(&items::DiamondPickaxe, 10U);
    setEnchantmentLevel(menu.left, EnchantmentId::Fortune, 3U);
    menu.right = enchantedBook(EnchantmentId::SilkTouch, 1U);
    refreshAnvilResult(menu, false);
    assert(menu.result.empty());
    assert(enchantmentLevel(menu.left, EnchantmentId::Fortune) == 3U); // input untouched

    // The same book also carrying something compatible: the conflicting one is
    // dropped (and charged for), the compatible one lands.
    AnvilMenu mixed;
    mixed.left = tool(&items::DiamondPickaxe, 10U);
    setEnchantmentLevel(mixed.left, EnchantmentId::Fortune, 3U);
    ItemStack book = enchantedBook(EnchantmentId::SilkTouch, 1U);
    setEnchantmentLevel(book, EnchantmentId::Unbreaking, 3U);
    mixed.right = book;
    refreshAnvilResult(mixed, false);
    assert(!mixed.result.empty());
    assert(enchantmentLevel(mixed.result, EnchantmentId::Unbreaking) == 3U);
    assert(enchantmentLevel(mixed.result, EnchantmentId::SilkTouch) == 0U);
    assert(enchantmentLevel(mixed.result, EnchantmentId::Fortune) == 3U);
    std::cout << "merge: a conflicting enchantment is refused, charged, and never applied\n";
}

// ---- taking the result: levels, inputs, and the penalty stamp ----

void testTakeSpendsLevelsAndStampsThePenalty() {
    AnvilMenu menu;
    menu.left = tool(&items::IronPickaxe, 200U);
    menu.right = stackOf(&items::IronIngot, 5U);
    refreshAnvilResult(menu, false);
    // 200 -> 138 -> 76 -> 14 -> 0: the fourth ingot mends the last 14, so four
    // are used and one is left over. (Three ingots would stop at 14 damage —
    // the repair keeps going while there is damage AND stack left.)
    assert(menu.cost == 4);

    PlayerExperience experience;
    experience.setExperienceLevel(30);
    ItemStack taken;
    const auto outcome = takeAnvilResult(menu, experience, false, taken);
    assert(outcome.applied);
    assert(outcome.levelsSpent == 4);
    assert(experience.level() == 26);
    assert(taken.item == &items::IronPickaxe);
    assert(taken.damage == 0U);
    // Fresh item, so the penalty starts at 2*0+1.
    assert(taken.repairCost == 1U);
    // The left slot is consumed; the right keeps the ingots the repair did not
    // need (5 in, 4 used).
    assert(menu.left.empty());
    assert(menu.right.count == 1U);

    // Not enough levels: nothing happens at all.
    AnvilMenu poor;
    poor.left = tool(&items::IronPickaxe, 200U);
    poor.right = stackOf(&items::IronIngot, 3U);
    refreshAnvilResult(poor, false);
    PlayerExperience broke;
    broke.setExperienceLevel(2);
    ItemStack nothing;
    assert(!takeAnvilResult(poor, broke, false, nothing).applied);
    assert(broke.level() == 2);
    assert(!poor.left.empty());
    std::cout << "take: spends the levels through XP-4, consumes only what it used\n";
}

// Sabotage ①'s target, and the anvil's whole economy: each trip through doubles
// the penalty, the penalty is added to the next trip's price, and the fifth or
// sixth trip is refused outright.
void testPriorWorkMakesItTerminal() {
    ItemStack item = tool(&items::DiamondPickaxe, 100U);
    PlayerExperience experience;
    experience.setExperienceLevel(200);
    const std::uint8_t expected[] = {1U, 3U, 7U, 15U, 31U};
    std::int32_t previousCost = 0;
    int trips = 0;
    for (const std::uint8_t expectedPenalty : expected) {
        AnvilMenu menu;
        menu.left = item;
        menu.right = enchantedBook(EnchantmentId::Efficiency, 1U);
        refreshAnvilResult(menu, false);
        if (menu.result.empty()) {
            break; // hit the wall
        }
        // Every trip is strictly more expensive than the last: the penalty is
        // what climbs, since the operation itself costs the same each time.
        assert(menu.cost > previousCost);
        previousCost = menu.cost;
        ItemStack taken;
        assert(takeAnvilResult(menu, experience, false, taken).applied);
        assert(taken.repairCost == expectedPenalty);
        item = taken;
        ++trips;
    }
    assert(trips >= 4); // it survives a few trips before the wall

    // Now force the wall: an item already at the maximum penalty is refused,
    // and the price is still reported so the screen can say "too expensive".
    AnvilMenu walled;
    walled.left = tool(&items::DiamondPickaxe, 100U);
    walled.left.repairCost = 63U;
    walled.right = enchantedBook(EnchantmentId::Efficiency, 1U);
    refreshAnvilResult(walled, false);
    assert(walled.cost >= kAnvilMaximumCost);
    assert(walled.result.empty());
    // Creative ignores the wall, as hasInfiniteMaterials does in vanilla.
    refreshAnvilResult(walled, /*infiniteMaterials=*/true);
    assert(!walled.result.empty());
    std::cout << "prior work: each trip costs more, and the wall is terminal in survival\n";
}

void testCreativeCostsNothing() {
    AnvilMenu menu;
    menu.left = tool(&items::IronPickaxe, 200U);
    menu.right = stackOf(&items::IronIngot, 3U);
    refreshAnvilResult(menu, true);
    PlayerExperience experience;
    experience.setExperienceLevel(0);
    ItemStack taken;
    const auto outcome = takeAnvilResult(menu, experience, true, taken);
    assert(outcome.applied);
    assert(outcome.levelsSpent == 0);
    assert(experience.level() == 0);
    assert(taken.damage == 14U);
    std::cout << "take: creative pays no levels\n";
}

void testNonAnvilInputsDoNothing() {
    // A block, or an item the anvil does not work on, produces nothing.
    AnvilMenu menu;
    menu.left = ItemStack{mc::world::Block::Cobblestone, 4U, blockItemFor(mc::world::Block::Cobblestone)};
    menu.right = stackOf(&items::IronIngot, 3U);
    refreshAnvilResult(menu, false);
    assert(menu.result.empty() && menu.cost == 0);

    // One input alone is not an operation either.
    AnvilMenu lonely;
    lonely.left = tool(&items::IronPickaxe, 200U);
    refreshAnvilResult(lonely, false);
    assert(lonely.result.empty() && lonely.cost == 0);
    std::cout << "inputs: a non-workable stack, or a lone input, is not an operation\n";
}

// ---- Mending: the orb's cut, taken before the level bar ----

// Sabotage ②'s target. Mending is not a new experience source, it is a
// DIVERSION of an existing one: what repairs gear never reaches the player's
// levels, and what is left over does.
void testMendingDivertsOrbPoints() {
    mc::world::gen::JavaRandom rng;
    rng.setSeed(20260901ULL);

    // One damaged Mending pickaxe, one orb's worth of points.
    ItemStack pickaxe = tool(&items::DiamondPickaxe, 100U);
    setEnchantmentLevel(pickaxe, EnchantmentId::Mending, 1U);
    ItemStack* candidates[] = {&pickaxe};
    MendingTargets targets{candidates, &rng};
    // 10 points mend 20 durability, and every point is consumed doing it.
    assert(repairWithExperience(targets, 10) == 0);
    assert(pickaxe.damage == 80U);

    // A nearly-whole item takes only what it needs; the rest survives as
    // experience (vanilla's proportional leftover, not a plain subtraction).
    ItemStack almost = tool(&items::DiamondPickaxe, 4U);
    setEnchantmentLevel(almost, EnchantmentId::Mending, 1U);
    ItemStack* few[] = {&almost};
    MendingTargets fewTargets{few, &rng};
    const std::int32_t leftover = repairWithExperience(fewTargets, 10);
    assert(almost.damage == 0U);
    assert(leftover > 0 && leftover < 10);

    // No Mending: every point survives, nothing is repaired.
    ItemStack plain = tool(&items::DiamondPickaxe, 100U);
    ItemStack* plainOnly[] = {&plain};
    MendingTargets plainTargets{plainOnly, &rng};
    assert(repairWithExperience(plainTargets, 10) == 10);
    assert(plain.damage == 100U);

    // Mending but undamaged: not a candidate, so the points pass through.
    ItemStack whole = tool(&items::DiamondPickaxe, 0U);
    setEnchantmentLevel(whole, EnchantmentId::Mending, 1U);
    ItemStack* wholeOnly[] = {&whole};
    MendingTargets wholeTargets{wholeOnly, &rng};
    assert(repairWithExperience(wholeTargets, 10) == 10);

    // No candidates at all (nothing worn, nothing held).
    assert(repairWithExperience(MendingTargets{}, 10) == 10);

    // Two damaged Mending items and plenty of points: the leftover rolls on to
    // the second, so a big orb repairs more than one piece.
    ItemStack helmet = tool(&items::DiamondHelmet, 6U);
    setEnchantmentLevel(helmet, EnchantmentId::Mending, 1U);
    ItemStack boots = tool(&items::DiamondBoots, 6U);
    setEnchantmentLevel(boots, EnchantmentId::Mending, 1U);
    ItemStack* pair[] = {&helmet, &boots};
    MendingTargets pairTargets{pair, &rng};
    static_cast<void>(repairWithExperience(pairTargets, 40));
    assert(helmet.damage == 0U);
    assert(boots.damage == 0U);
    std::cout << "mending: repairs first, only the leftover becomes experience\n";
}

// Same seed, same sequence — the pick must not come from a wall clock.
void testMendingIsDeterministic() {
    const auto run = [] {
        mc::world::gen::JavaRandom rng;
        rng.setSeed(4242ULL);
        ItemStack helmet = tool(&items::DiamondHelmet, 40U);
        setEnchantmentLevel(helmet, EnchantmentId::Mending, 1U);
        ItemStack boots = tool(&items::DiamondBoots, 40U);
        setEnchantmentLevel(boots, EnchantmentId::Mending, 1U);
        ItemStack* pair[] = {&helmet, &boots};
        MendingTargets targets{pair, &rng};
        static_cast<void>(repairWithExperience(targets, 5));
        return std::pair<std::uint16_t, std::uint16_t>{helmet.damage, boots.damage};
    };
    assert(run() == run());
    std::cout << "mending: same seed, same item picked\n";
}

// ---- the block, and reachability ----

void testAnvilBlockIdentity() {
    for (const mc::world::Block block :
         {mc::world::Block::Anvil, mc::world::Block::ChippedAnvil,
          mc::world::Block::DamagedAnvil}) {
        const auto& definition = mc::world::blockDefinition(block);
        assert(definition.container == mc::world::ContainerType::Anvil);
        assert(definition.model == mc::world::BlockModel::ElementModel);
        // Four stacked boxes, not a cube: it must not occlude or be face-sturdy.
        assert(!mc::world::isFullCube(block));
        const auto shape = mc::world::blockShape(mc::world::BlockState{block});
        assert(shape.kind == mc::world::ShapeKind::Boxes);
        assert(shape.boxes.size() == 4U);
        assert(mc::world::hasCollision(block));
        // Right-clicking opens the screen rather than using the held item.
        const auto decision = decideBlockInteraction(mc::world::ContainerType::Anvil, false, false);
        assert(decision.interaction == BlockInteraction::OpenAnvil);
    }
    // The shape rotates with the facing axis: the top plate runs along z when
    // the anvil faces north/south and along x when it faces east/west.
    const mc::world::BlockState facingNorth{mc::world::Block::Anvil,
                                            mc::world::BlockOrientation::North};
    const mc::world::BlockState facingEast{mc::world::Block::Anvil,
                                           mc::world::BlockOrientation::East};
    const auto north = mc::world::blockShape(facingNorth);
    const auto east = mc::world::blockShape(facingEast);
    assert(north.boxes[3].minZ == 0.0F && north.boxes[3].maxZ == 1.0F);
    assert(east.boxes[3].minX == 0.0F && east.boxes[3].maxX == 1.0F);
    std::cout << "block: three wear states, four-box shape that turns with the facing\n";
}

void testAnvilIsReachable() {
    const auto& registry = contentRegistry();
    std::size_t inFunctional = 0;
    for (const auto& stack : registry.catalog(CreativeCategory::Functional)) {
        if (stack.block == mc::world::Block::Anvil && stack.item != nullptr) {
            ++inFunctional;
        }
    }
    assert(inFunctional == 1U);
    assert(blockItemFor(mc::world::Block::Anvil) != nullptr);

    // And craftable: "III" / " i " / "iii" over a block of iron that is itself
    // craftable, so the whole chain closes.
    CraftingSystem crafting;
    const auto put = [&](std::size_t index, const Item* item, mc::world::Block block) {
        ItemStack& slot = crafting.tableGridSlot(index);
        slot = {};
        slot.item = item;
        slot.block = block;
        slot.count = 1U;
    };
    for (std::size_t index = 0; index < 9U; ++index) {
        put(index, &items::IronIngot, mc::world::Block::Air);
    }
    assert(crafting.tableOutput().block == mc::world::Block::IronBlock);

    const auto* ironBlockItem = blockItemFor(mc::world::Block::IronBlock);
    for (std::size_t index = 0; index < 3U; ++index) {
        put(index, ironBlockItem, mc::world::Block::IronBlock);
    }
    crafting.tableGridSlot(3) = {};
    put(4, &items::IronIngot, mc::world::Block::Air);
    crafting.tableGridSlot(5) = {};
    for (std::size_t index = 6; index < 9U; ++index) {
        put(index, &items::IronIngot, mc::world::Block::Air);
    }
    assert(crafting.tableOutput().block == mc::world::Block::Anvil);
    std::cout << "reachability: in Functional, craftable, and its iron block is too\n";
}

// ---- the session wiring ----

void testSessionWiring() {
    const auto arm = [](GameSession& session, GameMode mode) {
        session.primaryPlayer().gameMode = mode;
        session.primaryPlayer().experience.setExperienceLevel(30);
        session.openAnvilContainer(glm::ivec3{4, 5, 6});
        AnvilMenu& menu = session.anvilMenu();
        menu.left = tool(&items::IronPickaxe, 200U);
        menu.right = stackOf(&items::IronIngot, 3U);
        session.refreshAnvilResult();
    };
    {
        GameSession session;
        arm(session, GameMode::Survival);
        assert(session.anvilMenu().cost == 3);
        assert(session.takeAnvilResult(/*shiftHeld=*/true));
        assert(session.primaryPlayer().experience.level() == 27);
        assert(session.anvilMenu().left.empty());
        // The repaired pickaxe went to the inventory and carries its penalty.
        bool found = false;
        for (const auto& stack : session.inventory().slots()) {
            if (stack.item == &items::IronPickaxe) {
                assert(stack.damage == 14U);
                assert(stack.repairCost == 1U);
                found = true;
            }
        }
        assert(found);
    }
    // A closed screen refuses.
    {
        GameSession session;
        arm(session, GameMode::Survival);
        session.closeContainer();
        assert(!session.takeAnvilResult(true));
        assert(session.primaryPlayer().experience.level() == 30);
    }
    // Closing the menu hands the inputs back rather than eating them.
    {
        GameSession session;
        arm(session, GameMode::Survival);
        session.closeContainerMenu();
        assert(session.anvilMenu().left.empty() && session.anvilMenu().right.empty());
        int returned = 0;
        for (const auto& stack : session.inventory().slots()) {
            if (stack.item == &items::IronPickaxe || stack.item == &items::IronIngot) {
                ++returned;
            }
        }
        assert(returned == 2);
    }
    std::cout << "session: takes through XP-4, refuses when closed, hands inputs back\n";
}

void testSnapshotCarriesTheAnvilScreen() {
    WorldSnapshot sent;
    sent.openContainerScreen = ContainerScreen::Anvil;
    sent.anvilLeft = tool(&items::DiamondPickaxe, 120U);
    sent.anvilLeft.repairCost = 7U;
    setEnchantmentLevel(sent.anvilLeft, EnchantmentId::Efficiency, 3U);
    sent.anvilRight = enchantedBook(EnchantmentId::Unbreaking, 2U);
    sent.anvilResult = tool(&items::DiamondPickaxe, 100U);
    sent.anvilResult.repairCost = 15U;
    sent.anvilCost = 17;

    const auto bytes = encodeSnapshot(PublishedSnapshot{sent});
    const auto decoded = decodeSnapshot(bytes);
    assert(decoded.has_value());
    const auto* received = std::get_if<WorldSnapshot>(&*decoded);
    assert(received != nullptr);
    assert(received->anvilCost == 17);
    // The prior-work penalty has to cross too — the screen's price includes it.
    assert(received->anvilLeft.repairCost == 7U);
    assert(received->anvilResult.repairCost == 15U);
    assert(enchantmentLevel(received->anvilLeft, EnchantmentId::Efficiency) == 3U);
    assert(received->anvilRight.item == &items::EnchantedBook);
    std::cout << "snapshot: the anvil screen, prior-work penalty included, crosses the codec\n";
}

} // namespace

int main() {
    testAnvilCostFollowsRarity();
    testRepairCostDoubles();
    testMaterialRepair();
    testCombineDurability();
    testEnchantmentMerge();
    testIncompatibleEnchantments();
    testTakeSpendsLevelsAndStampsThePenalty();
    testPriorWorkMakesItTerminal();
    testCreativeCostsNothing();
    testNonAnvilInputsDoNothing();
    testMendingDivertsOrbPoints();
    testMendingIsDeterministic();
    testAnvilBlockIdentity();
    testAnvilIsReachable();
    testSessionWiring();
    testSnapshotCarriesTheAnvilScreen();
    std::cout << "anvil: all checks passed\n";
    return 0;
}

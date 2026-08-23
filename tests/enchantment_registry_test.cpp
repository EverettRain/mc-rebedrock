// ENCH-0: the constexpr enchantment identity registry — max level, rarity
// weight, category applicability, treasure/curse flags, cost curves and
// exclusivity, transcribed from 1.16.1's Enchantments.java + each
// Enchantment subclass. No gameplay effects, no table/UI/anvil — those are
// ENCH-1+; this only covers the data table Enchantment.hpp owns.

#include "gameplay/Enchantment.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/Item.hpp"

#include <cassert>
#include <iostream>

using namespace mc;
using namespace mc::gameplay;

namespace {

void testMaxLevelAndRarity() {
    assert(enchantmentMaxLevel(EnchantmentId::Sharpness) == 5);
    assert(enchantmentMaxLevel(EnchantmentId::SilkTouch) == 1);
    assert(enchantmentMaxLevel(EnchantmentId::Protection) == 4);
    assert(enchantmentMaxLevel(EnchantmentId::Efficiency) == 5);
    assert(enchantmentMaxLevel(EnchantmentId::Mending) == 1);

    assert(enchantmentRarity(EnchantmentId::Sharpness) == EnchantmentRarity::Common);
    assert(enchantmentRarity(EnchantmentId::Unbreaking) == EnchantmentRarity::Uncommon);
    assert(enchantmentRarity(EnchantmentId::Mending) == EnchantmentRarity::Rare);
    assert(enchantmentRarity(EnchantmentId::SilkTouch) == EnchantmentRarity::VeryRare);
    assert(enchantmentRarityWeight(EnchantmentRarity::Common) == 10U);
    assert(enchantmentRarityWeight(EnchantmentRarity::Uncommon) == 5U);
    assert(enchantmentRarityWeight(EnchantmentRarity::Rare) == 2U);
    assert(enchantmentRarityWeight(EnchantmentRarity::VeryRare) == 1U);
    std::cout << "testMaxLevelAndRarity OK\n";
}

void testTreasureAndCurseFlags() {
    assert(!enchantmentIsTreasureOnly(EnchantmentId::Sharpness));
    assert(enchantmentIsTreasureOnly(EnchantmentId::Mending));
    assert(enchantmentIsTreasureOnly(EnchantmentId::FrostWalker));
    assert(enchantmentIsTreasureOnly(EnchantmentId::SoulSpeed));
    assert(enchantmentIsTreasureOnly(EnchantmentId::BindingCurse));
    assert(enchantmentIsTreasureOnly(EnchantmentId::VanishingCurse));

    assert(!enchantmentIsCurse(EnchantmentId::Mending));
    assert(enchantmentIsCurse(EnchantmentId::BindingCurse));
    assert(enchantmentIsCurse(EnchantmentId::VanishingCurse));

    // Soul Speed is treasure-only AND excluded from random selection
    // entirely (bartering/commands only in vanilla).
    assert(!enchantmentAvailableForRandomSelection(EnchantmentId::SoulSpeed));
    assert(enchantmentAvailableForRandomSelection(EnchantmentId::Sharpness));
    std::cout << "testTreasureAndCurseFlags OK\n";
}

void testCategoryApplicability() {
    ItemStack pickaxe{world::Block::Air, 1U, &items::DiamondPickaxe};
    ItemStack sword{world::Block::Air, 1U, &items::IronSword};
    ItemStack book{world::Block::Air, 1U, &items::Book};
    ItemStack plainBlock{world::Block::Stone, 1U};

    // Efficiency (DIGGER) applies to a pickaxe, not a sword.
    assert(canEnchant(EnchantmentId::Efficiency, pickaxe));
    assert(!canEnchant(EnchantmentId::Efficiency, sword));
    // Sharpness (WEAPON) applies to a sword, not a pickaxe.
    assert(canEnchant(EnchantmentId::Sharpness, sword));
    assert(!canEnchant(EnchantmentId::Sharpness, pickaxe));
    // Unbreaking (BREAKABLE) applies to anything damageable — both tools.
    assert(canEnchant(EnchantmentId::Unbreaking, pickaxe));
    assert(canEnchant(EnchantmentId::Unbreaking, sword));
    // A book accepts every enchantment, matching vanilla's `|| bl` bypass.
    assert(canEnchant(EnchantmentId::Sharpness, book));
    assert(canEnchant(EnchantmentId::Efficiency, book));
    assert(canEnchant(EnchantmentId::Protection, book));
    // A plain block stack (no item, no ToolType) accepts nothing.
    assert(!canEnchant(EnchantmentId::Unbreaking, plainBlock));

    // Armor-only categories correctly answer false for every item this
    // registry has today (no armor items exist yet — AR content gap, not an
    // ENCH-0 gap): honest "no", not a false positive.
    assert(!canEnchant(EnchantmentId::Protection, sword));
    assert(!canEnchant(EnchantmentId::Protection, pickaxe));
    std::cout << "testCategoryApplicability OK\n";
}

void testEnchantability() {
    ItemStack woodPickaxe{world::Block::Air, 1U, &items::WoodenPickaxe};
    ItemStack stonePickaxe{world::Block::Air, 1U, &items::StonePickaxe};
    ItemStack ironPickaxe{world::Block::Air, 1U, &items::IronPickaxe};
    ItemStack goldPickaxe{world::Block::Air, 1U, &items::GoldPickaxe};
    ItemStack diamondPickaxe{world::Block::Air, 1U, &items::DiamondPickaxe};
    ItemStack book{world::Block::Air, 1U, &items::Book};
    ItemStack plainBlock{world::Block::Stone, 1U};

    assert(itemEnchantability(woodPickaxe) == 15);
    assert(itemEnchantability(stonePickaxe) == 5);
    assert(itemEnchantability(ironPickaxe) == 14);
    assert(itemEnchantability(goldPickaxe) == 22);
    assert(itemEnchantability(diamondPickaxe) == 10);
    assert(itemEnchantability(book) == 1);
    assert(itemEnchantability(plainBlock) == 0);
    std::cout << "testEnchantability OK\n";
}

// The sabotage③ target: getMinCost/getMaxCost must match 1.16.1's worked
// examples exactly, including the per-level multiplier.
void testCostFormulasWorkedExamples() {
    // Sharpness (DamageEnchantment typeIndex 0): minPower = 1 + (level-1)*11,
    // maxPower = minPower + 20.
    assert(getMinCost(EnchantmentId::Sharpness, 1) == 1);
    assert(getMaxCost(EnchantmentId::Sharpness, 1) == 21);
    assert(getMinCost(EnchantmentId::Sharpness, 5) == 1 + 4 * 11);   // 45
    assert(getMaxCost(EnchantmentId::Sharpness, 5) == 45 + 20);      // 65

    // Efficiency: minPower = 1 + 10*(level-1), maxPower = minPower + 50.
    assert(getMinCost(EnchantmentId::Efficiency, 1) == 1);
    assert(getMaxCost(EnchantmentId::Efficiency, 1) == 51);
    assert(getMinCost(EnchantmentId::Efficiency, 5) == 1 + 10 * 4);  // 41
    assert(getMaxCost(EnchantmentId::Efficiency, 5) == 41 + 50);     // 91

    // Protection (ALL type): base=1, perLevel=11, maxOffset=11.
    assert(getMinCost(EnchantmentId::Protection, 1) == 1);
    assert(getMaxCost(EnchantmentId::Protection, 1) == 12);
    assert(getMinCost(EnchantmentId::Protection, 4) == 1 + 3 * 11);  // 34
    assert(getMaxCost(EnchantmentId::Protection, 4) == 34 + 11);     // 45

    // SilkTouch: flat minPower 15, maxPower 65, at its only level.
    assert(getMinCost(EnchantmentId::SilkTouch, 1) == 15);
    assert(getMaxCost(EnchantmentId::SilkTouch, 1) == 65);

    // Infinity: flat 20..50.
    assert(getMinCost(EnchantmentId::Infinity, 1) == 20);
    assert(getMaxCost(EnchantmentId::Infinity, 1) == 50);

    // Loyalty: minPower = 5 + level*7 (not (level-1)*7); maxPower is a flat
    // 50 at EVERY level (LoyaltyEnchantment#getMaxPower returns the literal
    // constant, not minPower+offset) — the sabotage③ trap: an off-by-one
    // multiplier here would still coincidentally pass a naive minCost check
    // but fail this maxCost-flatness assertion.
    assert(getMinCost(EnchantmentId::Loyalty, 1) == 12);
    assert(getMaxCost(EnchantmentId::Loyalty, 1) == 50);
    assert(getMinCost(EnchantmentId::Loyalty, 3) == 5 + 3 * 7);  // 26
    assert(getMaxCost(EnchantmentId::Loyalty, 3) == 50);

    // Mending: minPower = level*25 (only level 1 exists), maxPower = +50.
    assert(getMinCost(EnchantmentId::Mending, 1) == 25);
    assert(getMaxCost(EnchantmentId::Mending, 1) == 75);
    std::cout << "testCostFormulasWorkedExamples OK\n";
}

void testExclusivity() {
    // Sharpness/Smite/BaneOfArthropods mutually exclusive (DamageEnchantment family).
    assert(!isCompatibleWith(EnchantmentId::Sharpness, EnchantmentId::Smite));
    assert(!isCompatibleWith(EnchantmentId::Sharpness, EnchantmentId::BaneOfArthropods));
    assert(!isCompatibleWith(EnchantmentId::Smite, EnchantmentId::BaneOfArthropods));
    // Symmetric.
    assert(!isCompatibleWith(EnchantmentId::Smite, EnchantmentId::Sharpness));

    // Silk Touch / Fortune mutually exclusive.
    assert(!isCompatibleWith(EnchantmentId::SilkTouch, EnchantmentId::Fortune));
    assert(!isCompatibleWith(EnchantmentId::Fortune, EnchantmentId::SilkTouch));

    // Infinity / Mending mutually exclusive.
    assert(!isCompatibleWith(EnchantmentId::Infinity, EnchantmentId::Mending));
    assert(!isCompatibleWith(EnchantmentId::Mending, EnchantmentId::Infinity));

    // Protection family: any two distinct members conflict UNLESS one is
    // Feather Falling (the FALL type).
    assert(!isCompatibleWith(EnchantmentId::Protection, EnchantmentId::FireProtection));
    assert(isCompatibleWith(EnchantmentId::Protection, EnchantmentId::FeatherFalling));
    assert(isCompatibleWith(EnchantmentId::BlastProtection, EnchantmentId::FeatherFalling));

    // Depth Strider / Frost Walker mutually exclusive (both feet, both water-related).
    assert(!isCompatibleWith(EnchantmentId::DepthStrider, EnchantmentId::FrostWalker));

    // An enchantment is never "compatible" with itself in the combine sense.
    assert(!isCompatibleWith(EnchantmentId::Sharpness, EnchantmentId::Sharpness));

    // Unrelated enchantments ARE compatible (Sharpness + Unbreaking, a
    // completely ordinary loadout).
    assert(isCompatibleWith(EnchantmentId::Sharpness, EnchantmentId::Unbreaking));
    assert(isCompatibleWith(EnchantmentId::Efficiency, EnchantmentId::Unbreaking));
    std::cout << "testExclusivity OK\n";
}

void testTableWellFormedAndVanillaNames() {
    static_assert(kEnchantmentTable.size() == kEnchantmentCount);
    assert(enchantmentVanillaName(EnchantmentId::Sharpness) == "sharpness");
    assert(enchantmentVanillaName(EnchantmentId::SilkTouch) == "silk_touch");
    assert(enchantmentVanillaName(EnchantmentId::VanishingCurse) == "vanishing_curse");
    std::cout << "testTableWellFormedAndVanillaNames OK\n";
}

}  // namespace

int main() {
    testMaxLevelAndRarity();
    testTreasureAndCurseFlags();
    testCategoryApplicability();
    testEnchantability();
    testCostFormulasWorkedExamples();
    testExclusivity();
    testTableWellFormedAndVanillaNames();
    std::cout << "enchantment_registry_test: all tests passed\n";
    return 0;
}

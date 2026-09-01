// EQ-0: armor content — the ArmorMaterial table, the 20 armor items (5
// materials x 4 slots), their durability/protection/toughness/enchantability,
// crafting recipes, and creative Combat tab membership. No equipment
// slots/wearing (EQ-1's storage half already landed separately), no damage-
// reduction formula (EQ-2), no armor enchant effects (EQ-4), no rendering —
// this file covers exactly the content EQ-0 adds.

#include "core/CreativeCategory.hpp"
#include "gameplay/ContentRegistry.hpp"
#include "gameplay/EquipmentSlot.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/RecipeTable.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <span>

using namespace mc;
using namespace mc::gameplay;

namespace {

// --- Sabotage② target: every armor item must carry the correct slot. ---
void testEachArmorItemDeclaresItsSlot() {
    assert(items::LeatherHelmet.armorSlot == EquipmentSlot::Head);
    assert(items::LeatherChestplate.armorSlot == EquipmentSlot::Chest);
    assert(items::LeatherLeggings.armorSlot == EquipmentSlot::Legs);
    assert(items::LeatherBoots.armorSlot == EquipmentSlot::Feet);

    assert(items::ChainmailHelmet.armorSlot == EquipmentSlot::Head);
    assert(items::ChainmailChestplate.armorSlot == EquipmentSlot::Chest);
    assert(items::ChainmailLeggings.armorSlot == EquipmentSlot::Legs);
    assert(items::ChainmailBoots.armorSlot == EquipmentSlot::Feet);

    assert(items::IronHelmet.armorSlot == EquipmentSlot::Head);
    assert(items::IronChestplate.armorSlot == EquipmentSlot::Chest);
    assert(items::IronLeggings.armorSlot == EquipmentSlot::Legs);
    assert(items::IronBoots.armorSlot == EquipmentSlot::Feet);

    assert(items::GoldHelmet.armorSlot == EquipmentSlot::Head);
    assert(items::GoldChestplate.armorSlot == EquipmentSlot::Chest);
    assert(items::GoldLeggings.armorSlot == EquipmentSlot::Legs);
    assert(items::GoldBoots.armorSlot == EquipmentSlot::Feet);

    // The acceptance criterion's literal assertion.
    assert(items::DiamondHelmet.armorSlot == EquipmentSlot::Head);
    assert(items::DiamondChestplate.armorSlot == EquipmentSlot::Chest);
    assert(items::DiamondLeggings.armorSlot == EquipmentSlot::Legs);
    assert(items::DiamondBoots.armorSlot == EquipmentSlot::Feet);

    // And the armorSlotOf() query agrees.
    assert(armorSlotOf(&items::DiamondHelmet) == EquipmentSlot::Head);
    std::cout << "testEachArmorItemDeclaresItsSlot OK\n";
}

void testEachArmorItemDeclaresItsMaterial() {
    assert(items::LeatherHelmet.armorMaterial == ArmorMaterialId::Leather);
    assert(items::ChainmailChestplate.armorMaterial == ArmorMaterialId::Chainmail);
    assert(items::IronLeggings.armorMaterial == ArmorMaterialId::Iron);
    assert(items::GoldBoots.armorMaterial == ArmorMaterialId::Gold);
    assert(items::DiamondHelmet.armorMaterial == ArmorMaterialId::Diamond);
    // Non-armor items are unaffected (ArmorMaterialId::None sentinel).
    assert(items::DiamondSword.armorMaterial == ArmorMaterialId::None);
    assert(!isArmor(&items::DiamondSword));
    assert(isArmor(&items::DiamondHelmet));
    std::cout << "testEachArmorItemDeclaresItsMaterial OK\n";
}

// --- Sabotage① target: full-set armor value/toughness sums must match
// vanilla exactly. ---
void testFullSetArmorValuesMatchVanilla() {
    // Diamond: 3+6+8+3 = 20 armor, toughness 2 per piece x 4 = 8.
    const std::uint32_t diamondSet =
        armorValue(&items::DiamondBoots) + armorValue(&items::DiamondLeggings) +
        armorValue(&items::DiamondChestplate) + armorValue(&items::DiamondHelmet);
    const float diamondToughness =
        armorToughness(&items::DiamondBoots) + armorToughness(&items::DiamondLeggings) +
        armorToughness(&items::DiamondChestplate) + armorToughness(&items::DiamondHelmet);
    assert(diamondSet == 20U);
    assert(diamondToughness == 8.0F);

    // Iron: 2+5+6+2 = 15 armor, toughness 0.
    const std::uint32_t ironSet =
        armorValue(&items::IronBoots) + armorValue(&items::IronLeggings) +
        armorValue(&items::IronChestplate) + armorValue(&items::IronHelmet);
    assert(ironSet == 15U);
    assert(armorToughness(&items::IronHelmet) == 0.0F);

    // Gold: 1+3+5+2 = 11 armor.
    const std::uint32_t goldSet =
        armorValue(&items::GoldBoots) + armorValue(&items::GoldLeggings) +
        armorValue(&items::GoldChestplate) + armorValue(&items::GoldHelmet);
    assert(goldSet == 11U);

    // Leather: 1+2+3+1 = 7 armor.
    const std::uint32_t leatherSet =
        armorValue(&items::LeatherBoots) + armorValue(&items::LeatherLeggings) +
        armorValue(&items::LeatherChestplate) + armorValue(&items::LeatherHelmet);
    assert(leatherSet == 7U);

    // Chainmail: 1+4+5+2 = 12 armor.
    const std::uint32_t chainmailSet =
        armorValue(&items::ChainmailBoots) + armorValue(&items::ChainmailLeggings) +
        armorValue(&items::ChainmailChestplate) + armorValue(&items::ChainmailHelmet);
    assert(chainmailSet == 12U);

    std::cout << "testFullSetArmorValuesMatchVanilla OK\n";
}

// Per-piece protection values, transcribed straight from vanilla's
// ArmorMaterials (PROTECTION_VALUES), independent of the full-set sums above
// — catches a compensating error (e.g. two swapped pieces that still sum
// right) the full-set test alone would miss.
void testPerPieceProtectionValues() {
    assert(armorValue(&items::LeatherHelmet) == 1U);
    assert(armorValue(&items::LeatherChestplate) == 3U);
    assert(armorValue(&items::LeatherLeggings) == 2U);
    assert(armorValue(&items::LeatherBoots) == 1U);

    assert(armorValue(&items::ChainmailHelmet) == 2U);
    assert(armorValue(&items::ChainmailChestplate) == 5U);
    assert(armorValue(&items::ChainmailLeggings) == 4U);
    assert(armorValue(&items::ChainmailBoots) == 1U);

    assert(armorValue(&items::IronHelmet) == 2U);
    assert(armorValue(&items::IronChestplate) == 6U);
    assert(armorValue(&items::IronLeggings) == 5U);
    assert(armorValue(&items::IronBoots) == 2U);

    assert(armorValue(&items::GoldHelmet) == 2U);
    assert(armorValue(&items::GoldChestplate) == 5U);
    assert(armorValue(&items::GoldLeggings) == 3U);
    assert(armorValue(&items::GoldBoots) == 1U);

    assert(armorValue(&items::DiamondHelmet) == 3U);
    assert(armorValue(&items::DiamondChestplate) == 8U);
    assert(armorValue(&items::DiamondLeggings) == 6U);
    assert(armorValue(&items::DiamondBoots) == 3U);

    // Only diamond carries toughness; every other material is 0.
    assert(armorToughness(&items::LeatherChestplate) == 0.0F);
    assert(armorToughness(&items::ChainmailChestplate) == 0.0F);
    assert(armorToughness(&items::IronChestplate) == 0.0F);
    assert(armorToughness(&items::GoldChestplate) == 0.0F);
    assert(armorToughness(&items::DiamondChestplate) == 2.0F);
    std::cout << "testPerPieceProtectionValues OK\n";
}

// --- Sabotage③ target: durability must scale by BOTH material multiplier
// AND per-slot base — not a flat per-material number. ---
void testDurabilityScalesByMaterialAndSlot() {
    // BASE_DURABILITY = {feet:13, legs:15, chest:16, head:11} (vanilla).
    // durabilityMultiplier: leather 5, chainmail 15, iron 15, gold 7, diamond 33.
    const ItemStack leatherHelmet{world::Block::Air, 1U, &items::LeatherHelmet};
    const ItemStack leatherBoots{world::Block::Air, 1U, &items::LeatherBoots};
    assert(itemMaximumDamage(leatherHelmet) == 11U * 5U);   // 55
    assert(itemMaximumDamage(leatherBoots) == 13U * 5U);    // 65
    // Same material, different slot -> different durability (proves the
    // per-slot base durability actually varies the result, not just the
    // per-material multiplier).
    assert(itemMaximumDamage(leatherHelmet) != itemMaximumDamage(leatherBoots));

    const ItemStack ironChestplate{world::Block::Air, 1U, &items::IronChestplate};
    assert(itemMaximumDamage(ironChestplate) == 16U * 15U);  // 240

    const ItemStack diamondLeggings{world::Block::Air, 1U, &items::DiamondLeggings};
    assert(itemMaximumDamage(diamondLeggings) == 15U * 33U);  // 495

    const ItemStack goldHelmet{world::Block::Air, 1U, &items::GoldHelmet};
    assert(itemMaximumDamage(goldHelmet) == 11U * 7U);  // 77

    const ItemStack chainmailChestplate{world::Block::Air, 1U, &items::ChainmailChestplate};
    assert(itemMaximumDamage(chainmailChestplate) == 16U * 15U);  // 240

    // Iron and chainmail share a durability multiplier (15) in vanilla, so
    // same-slot durability is expected to match between them — that shared
    // value itself is the assertion (a table that dropped the multiplier
    // would fail one of the per-material checks above instead). The
    // cross-material check below uses gold, whose multiplier (7) differs
    // from iron's (15), to prove the material axis is not ignored entirely.
    assert(itemMaximumDamage(ironChestplate) == itemMaximumDamage(chainmailChestplate));
    const ItemStack goldChestplate{world::Block::Air, 1U, &items::GoldChestplate};
    assert(itemMaximumDamage(ironChestplate) != itemMaximumDamage(goldChestplate));

    assert(isDamageable(leatherHelmet));
    std::cout << "testDurabilityScalesByMaterialAndSlot OK\n";
}

void testEnchantability() {
    assert(armorAttributes(ArmorMaterialId::Leather, EquipmentSlot::Head).enchantability == 15U);
    assert(armorAttributes(ArmorMaterialId::Chainmail, EquipmentSlot::Head).enchantability == 12U);
    assert(armorAttributes(ArmorMaterialId::Iron, EquipmentSlot::Head).enchantability == 9U);
    assert(armorAttributes(ArmorMaterialId::Gold, EquipmentSlot::Head).enchantability == 25U);
    assert(armorAttributes(ArmorMaterialId::Diamond, EquipmentSlot::Head).enchantability == 10U);
    std::cout << "testEnchantability OK\n";
}

// The registry grew by exactly the 20 armor items, and every entry stays
// unique/namespaced (itemRegistryIsWellFormed's static_assert already proves
// this at compile time; this just double-checks the runtime view agrees).
// RW-1 added 2 more (arrow, bow) after EQ-0 landed, DYE-1 added the 16 dyes,
// and AR-CX4-b added flint_and_steel — so the registry total this test rechecks
// is EQ-0's own 77 + RW-1's 2 + DYE-1's 16 + AR-CX4-b's 1 — the armor count
// below is unaffected (none of those 19 items is armor).
void testRegistryCount() {
    static_assert(kItemRegistry.size() == 103U); // +6 mined ore items (raw ores/lapis/redstone/quartz), +1 ENCH-2 enchanted_book
    int armorCount = 0;
    for (const Item* item : kItemRegistry) {
        if (isArmor(item)) ++armorCount;
    }
    assert(armorCount == 20);
    std::cout << "testRegistryCount OK\n";
}

// Crafting: leather/iron/gold/diamond armor is craftable; chainmail is not
// (no recipe in vanilla).
void testCraftingRecipesResolve() {
    RecipeTable table;
    table.loadBuiltinDefaults();
    const auto recipes = table.crafting();

    const auto find = [&](std::string_view identifier) -> const CraftingRecipe* {
        for (const auto& recipe : recipes) {
            if (recipe.identifier == identifier) return &recipe;
        }
        return nullptr;
    };

    const auto* diamondHelmet = find("minecraft:diamond_helmet");
    assert(diamondHelmet != nullptr);
    assert(diamondHelmet->output.item == &items::DiamondHelmet);
    assert(diamondHelmet->output.count == 1U);
    assert(diamondHelmet->width == 3U && diamondHelmet->height == 2U);

    const auto* ironBoots = find("minecraft:iron_boots");
    assert(ironBoots != nullptr);
    assert(ironBoots->output.item == &items::IronBoots);
    assert(ironBoots->width == 2U && ironBoots->height == 2U);

    const auto* leatherChestplate = find("minecraft:leather_chestplate");
    assert(leatherChestplate != nullptr);
    assert(leatherChestplate->output.item == &items::LeatherChestplate);
    assert(leatherChestplate->width == 3U && leatherChestplate->height == 3U);

    const auto* goldLeggings = find("minecraft:golden_leggings");
    assert(goldLeggings != nullptr);
    assert(goldLeggings->output.item == &items::GoldLeggings);

    // Chainmail: no recipe exists anywhere in the table (vanilla parity).
    assert(find("minecraft:chainmail_helmet") == nullptr);
    assert(find("minecraft:chainmail_chestplate") == nullptr);
    assert(find("minecraft:chainmail_leggings") == nullptr);
    assert(find("minecraft:chainmail_boots") == nullptr);

    std::cout << "testCraftingRecipesResolve OK\n";
}

// Creative catalog: all 20 armor items are filed under Combat, plus (RW-1)
// arrow and bow — both Combat-tabbed in vanilla too.
void testCreativeCombatTab() {
    const auto& registry = contentRegistry();
    const auto combat = registry.catalog(core::CreativeCategory::Combat);
    assert(combat.size() == 22U);

    int found = 0;
    for (const auto& stack : combat) {
        if (isArmor(stack.item)) ++found;
    }
    assert(found == 20);

    // Spot check: diamond_helmet resolves through the registry lookup too.
    const auto* registered = registry.item("rebedrock:diamond_helmet");
    assert(registered != nullptr);
    assert(registered->category == core::CreativeCategory::Combat);
    assert(registered->value == &items::DiamondHelmet);
    std::cout << "testCreativeCombatTab OK\n";
}

}  // namespace

int main() {
    testEachArmorItemDeclaresItsSlot();
    testEachArmorItemDeclaresItsMaterial();
    testFullSetArmorValuesMatchVanilla();
    testPerPieceProtectionValues();
    testDurabilityScalesByMaterialAndSlot();
    testEnchantability();
    testRegistryCount();
    testCraftingRecipesResolve();
    testCreativeCombatTab();
    std::cout << "armor_content_test: all tests passed\n";
    return 0;
}

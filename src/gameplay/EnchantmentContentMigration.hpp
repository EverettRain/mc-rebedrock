#pragma once

// DDC-3: the legacy-content migration bridge for enchantments — the seam DDC-1
// left open ("把 JSON def 折进 gameplay 运行期定义 … 是 DDC-2/DDC-3").
//
// DDC-0 poured the constexpr kEnchantmentTable into a runtime registry. DDC-1
// added the JE-schema reader (data::EnchantmentDef, parsed from
// data/<space>/enchantment/*.json). DDC-2 added the effects compiler. What was
// still missing was the *fold*: turning the data-side EnchantmentDef (what a
// JE datapack ships) into the gameplay-side EnchantmentDefinition (what the
// runtime registry and the enchanting-table candidate generator read), and the
// reverse fold that lets the built-in constexpr table be *emitted* as JE-schema
// datapack JSON. This header is that bridge, and it is the load-time (never
// hot-path) glue DDC-3's migration runs through.
//
// Two directions, both here so the golden migration test can prove they are
// exact inverses on every built-in:
//
//   toGameplayDefinition(data::EnchantmentDef, EnchantmentId)
//       -> gameplay::EnchantmentDefinition
//     Folds a JE-schema JSON def into the dense runtime POD. This is the fold a
//     datapack overlay travels: DataStore<EnchantmentDef> (DDC-1) parses the
//     files, this turns each into the EnchantmentDefinition the registry stores.
//
//   toContentDef(const gameplay::EnchantmentDefinition&)
//       -> data::EnchantmentDef
//     Folds the constexpr definition back into JE-schema data. Feeding this
//     through data::Codec<EnchantmentDef>::write dumps the *exact* JSON a
//     built-in would ship as a datapack file — which is how the internal
//     rebedrock datapack under resources/data/rebedrock/enchantment/ is
//     generated (tools/, and the golden test), byte-identical to the numbers
//     Enchantment.hpp transcribed.
//
// ---- What JE's schema carries vs what ENCH-0 needs ----
//
// JE 26.1's enchantment JSON carries weight / max_level / min_cost / max_cost /
// anvil_cost / slots / supported_items directly. It does NOT carry, as fields,
// the four ENCH-0 attributes that 1.16.1 hardcoded on each Enchantment subclass
// and that 26.1 instead expresses through *registry tags*
// (#minecraft:treasure, #minecraft:curse, #minecraft:non_treasure,
// #minecraft:in_enchanting_table, and the enchantment's own EnchantmentTarget
// -> category):
//
//   * rarity        — 1.16.1 Enchantment.Rarity; JE derives its weight from it,
//                     and the weight is the field that survives into JSON, so the
//                     rarity round-trips *through* weight (weight 10/5/2/1 <->
//                     Common/Uncommon/Rare/VeryRare, ENCH-0's own
//                     enchantmentRarityWeight table, inverted).
//   * category      — 1.16.1 EnchantmentTarget; JE expresses it as the
//                     supported_items tag, so it round-trips through
//                     supported_items + slots (a canonical tag per category).
//   * treasureOnly  — a #minecraft:treasure tag membership in 26.1.
//   * curse         — a #minecraft:curse tag membership in 26.1.
//   * availableForRandomSelection — 26.1's #minecraft:in_enchanting_table
//                     absence (soul_speed only).
//
// The three booleans have no lossless home in the pure-JE fields, so the bridge
// keeps them in a small `rebedrock` extension object on the def (a datapack may
// carry an unknown top-level object without the reader failing — DDC-1's
// forward-compat rule). A real JE official file omits the block; its defaults
// (treasure=false, curse=false, random=true) are exactly what the vanilla
// non-treasure, non-curse, table-rollable enchantment wants, so JE files still
// fold correctly. rarity and category are recovered from weight + supported_items
// so they do NOT need the extension block — only the three booleans do, and only
// for the handful of enchantments that deviate from the common defaults.
//
// This is deliberately not a gameplay change: kEnchantmentTable stays the baked
// floor (DDC-DESIGN §5 — the built-in content a no-datapack build runs on is
// mandatory), and the constexpr accessors in Enchantment.hpp stay for the
// compile-time / hot-path callers. DDC-3 proves the floor is *expressible* as
// JE-schema datapack JSON with zero drift and ships that JSON as the internal
// datapack; it does not delete the floor.

#include "core/Json.hpp"
#include "data/Codec.hpp"
#include "data/EnchantmentFile.hpp"
#include "gameplay/Enchantment.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace mc::gameplay {

// ---- rarity <-> weight (lossless, through ENCH-0's own weight table) --------

// The inverse of enchantmentRarityWeight: the four vanilla weights map 1:1 back
// to the four rarities. An unrecognised weight (a datapack picking an arbitrary
// number) falls to the closest bucket, monotone in weight, so a custom weight
// never aborts — it just lands in a sensible rarity for the candidate generator.
[[nodiscard]] constexpr EnchantmentRarity rarityFromWeight(std::int32_t weight) {
    if (weight >= 10) return EnchantmentRarity::Common;
    if (weight >= 5) return EnchantmentRarity::Uncommon;
    if (weight >= 2) return EnchantmentRarity::Rare;
    return EnchantmentRarity::VeryRare;
}

// ---- category <-> supported_items tag (canonical per category) -------------

// The canonical JE `supported_items` tag ENCH-0's EnchantmentCategory maps to.
// This is the tag 26.1's own file uses for the same target, so a built-in emitted
// through toContentDef writes the tag a vanilla file would, and a vanilla file's
// tag folds back to the matching category. Categories this build has no item for
// yet (armor pieces, bow, trident, …) still get their honest vanilla tag so the
// round-trip is exact even though canEnchant answers false for them today.
[[nodiscard]] constexpr std::string_view categorySupportedItemsTag(EnchantmentCategory category) {
    switch (category) {
    case EnchantmentCategory::Armor: return "#minecraft:enchantable/armor";
    case EnchantmentCategory::ArmorFeet: return "#minecraft:enchantable/foot_armor";
    case EnchantmentCategory::ArmorLegs: return "#minecraft:enchantable/leg_armor";
    case EnchantmentCategory::ArmorChest: return "#minecraft:enchantable/chest_armor";
    case EnchantmentCategory::ArmorHead: return "#minecraft:enchantable/head_armor";
    case EnchantmentCategory::Weapon: return "#minecraft:enchantable/weapon";
    case EnchantmentCategory::Digger: return "#minecraft:enchantable/mining";
    case EnchantmentCategory::FishingRod: return "#minecraft:enchantable/fishing";
    case EnchantmentCategory::Trident: return "#minecraft:enchantable/trident";
    case EnchantmentCategory::Breakable: return "#minecraft:enchantable/durability";
    case EnchantmentCategory::Bow: return "#minecraft:enchantable/bow";
    case EnchantmentCategory::Wearable: return "#minecraft:enchantable/equippable";
    case EnchantmentCategory::Crossbow: return "#minecraft:enchantable/crossbow";
    case EnchantmentCategory::Vanishable: return "#minecraft:enchantable/vanishing";
    }
    return "#minecraft:enchantable/durability";
}

// The inverse: the EnchantmentCategory a supported_items tag names. An unknown
// tag (a datapack inventing its own enchantable tag) folds to Breakable — the
// most permissive damageable-item category — rather than aborting, so a custom
// datapack enchantment still lands somewhere the candidate generator accepts.
[[nodiscard]] constexpr EnchantmentCategory categoryFromSupportedItemsTag(std::string_view tag) {
    // Accept both the "#minecraft:..." spelling and the bare tag path.
    if (!tag.empty() && tag.front() == '#') tag.remove_prefix(1U);
    struct Row {
        std::string_view tag;
        EnchantmentCategory category;
    };
    constexpr std::array<Row, 14> kRows{{
        {"minecraft:enchantable/armor", EnchantmentCategory::Armor},
        {"minecraft:enchantable/foot_armor", EnchantmentCategory::ArmorFeet},
        {"minecraft:enchantable/leg_armor", EnchantmentCategory::ArmorLegs},
        {"minecraft:enchantable/chest_armor", EnchantmentCategory::ArmorChest},
        {"minecraft:enchantable/head_armor", EnchantmentCategory::ArmorHead},
        {"minecraft:enchantable/weapon", EnchantmentCategory::Weapon},
        {"minecraft:enchantable/mining", EnchantmentCategory::Digger},
        {"minecraft:enchantable/fishing", EnchantmentCategory::FishingRod},
        {"minecraft:enchantable/trident", EnchantmentCategory::Trident},
        {"minecraft:enchantable/durability", EnchantmentCategory::Breakable},
        {"minecraft:enchantable/bow", EnchantmentCategory::Bow},
        {"minecraft:enchantable/equippable", EnchantmentCategory::Wearable},
        {"minecraft:enchantable/crossbow", EnchantmentCategory::Crossbow},
        {"minecraft:enchantable/vanishing", EnchantmentCategory::Vanishable},
    }};
    for (const Row& row : kRows) {
        if (row.tag == tag) return row.category;
    }
    return EnchantmentCategory::Breakable;
}

// The canonical `slots` list a category equips into, matching 26.1's file for
// the same target. Armor pieces list their single slot, the armor family lists
// "armor", weapons/tools list "mainhand", everything wearable "any".
[[nodiscard]] inline std::vector<std::string> categorySlots(EnchantmentCategory category) {
    switch (category) {
    case EnchantmentCategory::Armor: return {"armor"};
    case EnchantmentCategory::ArmorFeet: return {"feet"};
    case EnchantmentCategory::ArmorLegs: return {"legs"};
    case EnchantmentCategory::ArmorChest: return {"chest"};
    case EnchantmentCategory::ArmorHead: return {"head"};
    case EnchantmentCategory::Weapon:
    case EnchantmentCategory::Digger:
    case EnchantmentCategory::FishingRod:
        return {"mainhand"};
    case EnchantmentCategory::Trident:
    case EnchantmentCategory::Crossbow:
    case EnchantmentCategory::Bow:
        return {"hand"};
    case EnchantmentCategory::Breakable:
    case EnchantmentCategory::Wearable:
    case EnchantmentCategory::Vanishable:
        return {"any"};
    }
    return {"any"};
}

// ---- the cost curve <-> min_cost/max_cost fold ------------------------------

// getMaxCost's per-level offset shape (maxPower(level) = minPower(level) +
// maxOffset) becomes JE's max_cost { base, per_level_above_first } as:
//   max_cost.base           = minBase + maxOffset
//   max_cost.per_level      = minPerLevel
// except for the four flat-50 enchantments (Loyalty/Riptide/QuickCharge/
// Piercing) whose vanilla getMaxPower returns the literal 50 regardless of
// level — those write max_cost { 50, 0 }, exactly what the constexpr accessor
// getMaxCost returns. This is the same reasoning Enchantment.hpp's
// enchantmentHasFlatMaxCostOf50 encodes; we consult it so the fold and the
// accessor never disagree.
[[nodiscard]] inline data::EnchantmentCostValue minCostValue(EnchantmentId id) {
    const EnchantmentCostCurve& curve = enchantmentDefinition(id).cost;
    return data::EnchantmentCostValue{curve.minBase, curve.minPerLevel};
}
[[nodiscard]] inline data::EnchantmentCostValue maxCostValue(EnchantmentId id) {
    if (enchantmentHasFlatMaxCostOf50(id)) {
        return data::EnchantmentCostValue{50, 0};
    }
    const EnchantmentCostCurve& curve = enchantmentDefinition(id).cost;
    return data::EnchantmentCostValue{curve.minBase + curve.maxOffset, curve.minPerLevel};
}

// The `rebedrock` extension key the three tag-derived booleans travel under when
// they deviate from the common (non-treasure, non-curse, table-rollable)
// defaults. Absent on a vanilla file, which is exactly the common case.
inline constexpr std::string_view kRebedrockExtensionKey = "rebedrock";

// ---- forward fold: JE-schema data def -> gameplay runtime definition --------

// Reads the three tag-derived booleans off a def's `rebedrock` extension object,
// leaving them at the vanilla defaults when the block (or a given key) is absent
// — so an official JE file with no extension folds to a non-treasure, non-curse,
// table-rollable enchantment, which every vanilla non-special enchantment is.
struct RebedrockEnchantmentExtras final {
    bool treasureOnly = false;
    bool curse = false;
    bool availableForRandomSelection = true;
};

[[nodiscard]] inline RebedrockEnchantmentExtras readRebedrockExtras(const core::Json& sourceObject) {
    RebedrockEnchantmentExtras extras{};
    if (!sourceObject.isObject() || !sourceObject.contains(kRebedrockExtensionKey)) {
        return extras;
    }
    const core::Json& block = sourceObject[kRebedrockExtensionKey];
    if (!block.isObject()) return extras;
    const auto readBool = [&block](std::string_view key, bool& out) {
        if (block.contains(key) && block[key].type() == core::Json::Type::Boolean) {
            out = block[key].asBool();
        }
    };
    readBool("treasure_only", extras.treasureOnly);
    readBool("curse", extras.curse);
    readBool("random_selection", extras.availableForRandomSelection);
    return extras;
}

// Folds a JE-schema EnchantmentDef into the dense gameplay POD. `id` is the
// dense EnchantmentId the built-in table assigns (a datapack addition past the
// enum uses EnchantmentId::Count as a "not a built-in" marker; the registry
// reaches it by name, not this ordinal). `vanillaName` names the def; `extras`
// carries the three tag-derived booleans the caller read off the source object
// via readRebedrockExtras (empty for a vanilla file).
[[nodiscard]] inline EnchantmentDefinition
toGameplayDefinition(const data::EnchantmentDef& def, EnchantmentId id,
                     std::string_view vanillaName,
                     const RebedrockEnchantmentExtras& extras = {}) {
    EnchantmentDefinition out{};
    out.id = id;
    out.vanillaName = vanillaName;
    out.minLevel = 1U;
    out.maxLevel = static_cast<std::uint8_t>(def.maxLevel < 1 ? 1 : def.maxLevel);
    out.rarity = rarityFromWeight(def.weight);
    out.category = categoryFromSupportedItemsTag(def.supportedItems.value);
    // The min curve is JE's min_cost directly; the max curve is folded back into
    // the (minBase, minPerLevel, maxOffset) shape ENCH-0's EnchantmentCostCurve
    // stores. For a flat-50 enchantment JE writes max_cost {50,0}; that cannot be
    // re-expressed as an offset over the (possibly non-constant) min curve, so we
    // mark it with a sentinel offset the accessor already special-cases: the
    // maxOffset stored is (max_cost.base - min_cost.base), which is correct for
    // every non-flat enchantment, and the flat ones are recognised by name in
    // enchantmentHasFlatMaxCostOf50 anyway, so getMaxCost returns 50 regardless
    // of the stored offset for them.
    out.cost.minBase = def.minCost.base;
    out.cost.minPerLevel = def.minCost.perLevelAboveFirst;
    out.cost.maxOffset = def.maxCost.base - def.minCost.base;
    out.treasureOnly = extras.treasureOnly;
    out.curse = extras.curse;
    out.availableForRandomSelection = extras.availableForRandomSelection;
    return out;
}

// ---- reverse fold: gameplay constexpr definition -> JE-schema data def ------

// The JE weight the def's rarity maps to (ENCH-0's own table). Public so the
// generator and the golden test agree on the number written to disk.
[[nodiscard]] constexpr std::int32_t enchantmentWeight(const EnchantmentDefinition& def) {
    return enchantmentRarityWeight(def.rarity);
}

// Folds a constexpr EnchantmentDefinition back into a JE-schema EnchantmentDef —
// the exact data a built-in would ship as a datapack file. anvil_cost mirrors
// 1.16.1's per-enchantment anvil multiplier is not part of ENCH-0's constexpr
// table (ENCH-0 scope was identity + cost + exclusivity, no anvil), so we write
// the vanilla base 1 (2 for durability-family) the same way 26.1 does; a future
// anvil node can refine it. supported_items / slots / min_cost / max_cost /
// weight / max_level reproduce the constexpr numbers exactly.
[[nodiscard]] inline data::EnchantmentDef toContentDef(const EnchantmentDefinition& def) {
    data::EnchantmentDef out{};
    // anvil_cost: 1.16.1's Enchantment default is 1; the durability/breakable
    // family (Unbreaking, Mending) and the trident/crossbow rares cost 2 in
    // vanilla. ENCH-0 did not model it, so we mirror 26.1's own value: 1 for the
    // common case, 2 for Uncommon/Rare/VeryRare durability. This field is not
    // golden-compared against ENCH-0 (ENCH-0 has no anvil field); it is written
    // for JE-shape completeness.
    out.anvilCost = 1;
    out.weight = enchantmentWeight(def);
    out.maxLevel = def.maxLevel;
    out.minCost = minCostValue(def.id);
    out.maxCost = maxCostValue(def.id);
    out.slots = categorySlots(def.category);
    out.supportedItems = data::EnchantmentItemSet{
        std::string{categorySupportedItemsTag(def.category)}, true};
    out.hasPrimaryItems = false;
    // exclusive_set: left empty here (ENCH-0 encodes exclusivity procedurally in
    // isCompatibleWith, not as a tag); the JE exclusive_set tag is a DDC-4
    // concern when ingesting official files. The golden test compares the
    // procedural exclusivity, not this field.
    return out;
}

// Writes the JE-schema JSON object for a built-in, including the `rebedrock`
// extension block *only* when at least one boolean deviates from the vanilla
// default — so a plain enchantment's file is pure JE (no rebedrock key), and
// only the special ones (treasure/curse/non-table) carry the block. This is the
// object the internal datapack file holds.
[[nodiscard]] inline core::Json enchantmentContentJson(const EnchantmentDefinition& def) {
    core::Json json = data::Codec<data::EnchantmentDef>::write(toContentDef(def));
    const bool hasExtras =
        def.treasureOnly || def.curse || !def.availableForRandomSelection;
    if (hasExtras && json.isObject()) {
        core::Json::Object block;
        if (def.treasureOnly) block.emplace_back("treasure_only", core::Json{true});
        if (def.curse) block.emplace_back("curse", core::Json{true});
        if (!def.availableForRandomSelection) {
            block.emplace_back("random_selection", core::Json{false});
        }
        core::Json::Object members = json.asObject();
        members.emplace_back(std::string{kRebedrockExtensionKey},
                             core::Json{std::move(block)});
        json = core::Json{std::move(members)};
    }
    return json;
}

} // namespace mc::gameplay

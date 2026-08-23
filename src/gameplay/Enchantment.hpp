#pragma once

// ENCH-0: the enchantment identity registry — Java 1.16.1's fixed,
// hardcoded-in-Java enchantment set (Enchantments.java's 34 constants), each
// carrying the max level, rarity weight, applicability category, cost curve
// and exclusivity rule its own 1.16.1 Enchantment subclass hardcoded. This
// project targets 26.1 architecturally, but item/tool code in this file's
// neighbourhood (Item.hpp's ToolMaterials comment) is explicitly modeled on
// 1.16.1, and 26.1's data-driven enchantment JSON is a future PACK/D-subtree
// concern (deferred, not started here) — so this table mirrors 1.16.1's
// hardcoded Enchantment/EnchantmentTarget/Rarity shape exactly, one constexpr
// entry per subclass, dense-indexed by EnchantmentId instead of virtual
// dispatch (DOD: data table, not a class hierarchy).
//
// Source: yarn-mapped 1.16.1 net.minecraft.enchantment.{Enchantment,
// Enchantments,EnchantmentTarget,<EachSubclass>}.java (see
// docs/vanilla-1161-sources for the jar location) plus
// net.minecraft.item.ToolMaterials (per-material enchantability) and
// BookItem#getEnchantability (books fix it at 1).
//
// Scope: identity + cost curves + exclusivity only. NO gameplay effects
// (protection damage reduction, sharpness bonus damage, efficiency mining
// speed math, thorns reflect, frost walker freezing, etc.) — those are
// ENCH-1+. NO enchanting-table block/UI, NO anvil, NO enchanted books, NO XP
// spend. EnchantmentHelper.hpp (ENCH-0's other half) is the only consumer of
// getMinCost/getMaxCost/isCompatibleWith/canEnchant here.

#include "gameplay/Inventory.hpp"
#include "gameplay/Item.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace mc::gameplay {

// One entry per Enchantments.java constant. The ordinal is this project's own
// dense index (JC debt: no direct binary-id mapping to vanilla's registry raw
// id is preserved — enchantmentIdentifier() below is the string-name bridge
// vanilla's data would need to interoperate, see JC-mapping note at the
// bottom of this file).
enum class EnchantmentId : std::uint8_t {
    Protection,
    FireProtection,
    FeatherFalling,
    BlastProtection,
    ProjectileProtection,
    Respiration,
    AquaAffinity,
    Thorns,
    DepthStrider,
    FrostWalker,
    BindingCurse,
    SoulSpeed,
    Sharpness,
    Smite,
    BaneOfArthropods,
    Knockback,
    FireAspect,
    Looting,
    Sweeping,
    Efficiency,
    SilkTouch,
    Unbreaking,
    Fortune,
    Power,
    Punch,
    Flame,
    Infinity,
    LuckOfTheSea,
    Lure,
    Loyalty,
    Impaling,
    Riptide,
    Channeling,
    Multishot,
    QuickCharge,
    Piercing,
    Mending,
    VanishingCurse,
    Count,
};

inline constexpr std::size_t kEnchantmentCount =
    static_cast<std::size_t>(EnchantmentId::Count);

// Enchantment.Rarity: the four weight buckets a table offer's weighted pick
// draws from (WeightedPicker.getRandom in EnchantmentHelper.generateEnchantments).
enum class EnchantmentRarity : std::uint8_t {
    Common,
    Uncommon,
    Rare,
    VeryRare,
};

[[nodiscard]] constexpr std::uint8_t enchantmentRarityWeight(EnchantmentRarity rarity) {
    switch (rarity) {
    case EnchantmentRarity::Common: return 10U;
    case EnchantmentRarity::Uncommon: return 5U;
    case EnchantmentRarity::Rare: return 2U;
    case EnchantmentRarity::VeryRare: return 1U;
    }
    return 0U;
}

// EnchantmentTarget (1.16.1): what an enchantment is allowed to land on. Kept
// as the full vanilla set even though this project has no armor/bow/fishing
// rod/trident/crossbow/shears items yet (AR content gap, not an ENCH-0 gap) —
// canEnchant() below answers false for those honestly rather than the table
// lying about a category this build cannot yet place on any item.
enum class EnchantmentCategory : std::uint8_t {
    Armor,
    ArmorFeet,
    ArmorLegs,
    ArmorChest,
    ArmorHead,
    Weapon,
    Digger,
    FishingRod,
    Trident,
    Breakable,
    Bow,
    Wearable,
    Crossbow,
    Vanishable,
};

// One row of the cost-curve table: `getMinPower`/`getMaxPower` are usually
// affine in level (base + (level-1)*perLevel), plus a max-power offset. A
// couple of enchantments (Infinity, Flame, Mending, curses, Frost Walker
// doubling as Depth Strider's twin, etc.) do not fit "min affine, max = min +
// constant" without per-entry overrides, so the row also carries an explicit
// maxPowerOverride flag for the few (Infinity/Flame use a flat 20..50 with no
// level dependence at all — level is always 1 there so the affine form would
// coincidentally work, but is spelled out for clarity).
struct EnchantmentCostCurve final {
    std::int32_t minBase = 1;
    std::int32_t minPerLevel = 10;
    std::int32_t maxOffset = 5;  // maxPower(level) = minPower(level) + maxOffset
};

struct EnchantmentDefinition final {
    EnchantmentId id = EnchantmentId::Count;
    std::string_view vanillaName{};
    std::uint8_t minLevel = 1;
    std::uint8_t maxLevel = 1;
    EnchantmentRarity rarity = EnchantmentRarity::Common;
    EnchantmentCategory category = EnchantmentCategory::Breakable;
    EnchantmentCostCurve cost{};
    bool treasureOnly = false;
    bool curse = false;
    // Enchantment#isAvailableForRandomSelection: Soul Speed cannot be rolled by
    // the table (or an enchanted book loot table's random pick) at all, only
    // obtained pre-enchanted (bartering) or via /enchant.
    bool availableForRandomSelection = true;
};

namespace detail {

// getMinPower(level) = base + (level-1)*perLevel; getMaxPower(level) =
// getMinPower(level) + maxOffset. Mirrors every 1.16.1 Enchantment subclass's
// override pair exactly (including the ones whose perLevel is 0, i.e.
// SilkTouch/Infinity/Flame/BindingCurse/VanishingCurse/Channeling/Mending's
// level*25 case, which is base=0,perLevel=25 with minLevel==maxLevel==1 so
// "per level" only ever evaluates at level 1).
[[nodiscard]] constexpr std::int32_t minPower(const EnchantmentCostCurve& curve,
                                              std::int32_t level) {
    return curve.minBase + (level - 1) * curve.minPerLevel;
}
[[nodiscard]] constexpr std::int32_t maxPower(const EnchantmentCostCurve& curve,
                                              std::int32_t level) {
    return minPower(curve, level) + curve.maxOffset;
}

} // namespace detail

// The dense table, one row per EnchantmentId, in enum declaration order.
// Every numeric field below is transcribed from the matching 1.16.1
// Enchantment subclass's getMinPower/getMaxPower/getMaxLevel/isTreasure/
// isCursed overrides (see the file banner for the exact source paths).
inline constexpr std::array<EnchantmentDefinition, kEnchantmentCount> kEnchantmentTable{{
    // Protection family: ProtectionEnchantment.Type(name, basePower, powerPerLevel),
    // maxOffset == powerPerLevel for every member of the family.
    {EnchantmentId::Protection, "protection", 1, 4, EnchantmentRarity::Common,
     EnchantmentCategory::Armor, {1, 11, 11}, false, false, true},
    {EnchantmentId::FireProtection, "fire_protection", 1, 4, EnchantmentRarity::Uncommon,
     EnchantmentCategory::Armor, {10, 8, 8}, false, false, true},
    {EnchantmentId::FeatherFalling, "feather_falling", 1, 4, EnchantmentRarity::Uncommon,
     EnchantmentCategory::ArmorFeet, {5, 6, 6}, false, false, true},
    {EnchantmentId::BlastProtection, "blast_protection", 1, 4, EnchantmentRarity::Rare,
     EnchantmentCategory::Armor, {5, 8, 8}, false, false, true},
    {EnchantmentId::ProjectileProtection, "projectile_protection", 1, 4, EnchantmentRarity::Uncommon,
     EnchantmentCategory::Armor, {3, 6, 6}, false, false, true},
    // RespirationEnchantment: minPower = 10*level -> base=10, perLevel=10; maxOffset=30.
    {EnchantmentId::Respiration, "respiration", 1, 3, EnchantmentRarity::Rare,
     EnchantmentCategory::ArmorHead, {10, 10, 30}, false, false, true},
    // AquaAffinityEnchantment: minPower always 1 (perLevel irrelevant, max level 1).
    {EnchantmentId::AquaAffinity, "aqua_affinity", 1, 1, EnchantmentRarity::Rare,
     EnchantmentCategory::ArmorHead, {1, 0, 40}, false, false, true},
    // ThornsEnchantment: minPower = 10 + 20*(level-1).
    {EnchantmentId::Thorns, "thorns", 1, 3, EnchantmentRarity::VeryRare,
     EnchantmentCategory::ArmorChest, {10, 20, 50}, false, false, true},
    // DepthStriderEnchantment: minPower = level*10 -> base=10, perLevel=10.
    {EnchantmentId::DepthStrider, "depth_strider", 1, 3, EnchantmentRarity::Rare,
     EnchantmentCategory::ArmorFeet, {10, 10, 15}, false, false, true},
    // FrostWalkerEnchantment: minPower = level*10; treasure-only (never rolled
    // by the table at all in vanilla — isTreasure gates generateEnchantments'
    // getPossibleEntries via the `treasureAllowed` flag EnchantmentHelper.hpp's
    // table-offer path passes as false, same as vanilla's screen handler).
    {EnchantmentId::FrostWalker, "frost_walker", 1, 2, EnchantmentRarity::Rare,
     EnchantmentCategory::ArmorFeet, {10, 10, 15}, true, false, true},
    // BindingCurseEnchantment: flat 25..50, treasure + curse.
    {EnchantmentId::BindingCurse, "binding_curse", 1, 1, EnchantmentRarity::VeryRare,
     EnchantmentCategory::Wearable, {25, 0, 25}, true, true, true},
    // SoulSpeedEnchantment: treasure-only AND excluded from random selection
    // entirely (isAvailableForRandomSelection() == false) — obtainable only via
    // bartering/commands in vanilla, never the table or an enchanted book roll.
    {EnchantmentId::SoulSpeed, "soul_speed", 1, 3, EnchantmentRarity::VeryRare,
     EnchantmentCategory::ArmorFeet, {10, 10, 15}, true, false, false},
    // DamageEnchantment(typeIndex 0/1/2 = all/undead/arthropods): minPower =
    // base[typeIndex] + (level-1)*perLevel[typeIndex]; maxOffset is 20 for all three.
    {EnchantmentId::Sharpness, "sharpness", 1, 5, EnchantmentRarity::Common,
     EnchantmentCategory::Weapon, {1, 11, 20}, false, false, true},
    {EnchantmentId::Smite, "smite", 1, 5, EnchantmentRarity::Uncommon,
     EnchantmentCategory::Weapon, {5, 8, 20}, false, false, true},
    {EnchantmentId::BaneOfArthropods, "bane_of_arthropods", 1, 5, EnchantmentRarity::Uncommon,
     EnchantmentCategory::Weapon, {5, 8, 20}, false, false, true},
    // KnockbackEnchantment: minPower = 5 + 20*(level-1).
    {EnchantmentId::Knockback, "knockback", 1, 2, EnchantmentRarity::Uncommon,
     EnchantmentCategory::Weapon, {5, 20, 50}, false, false, true},
    // FireAspectEnchantment: minPower = 10 + 20*(level-1).
    {EnchantmentId::FireAspect, "fire_aspect", 1, 2, EnchantmentRarity::Rare,
     EnchantmentCategory::Weapon, {10, 20, 50}, false, false, true},
    // LuckEnchantment (Looting instance, EnchantmentTarget.WEAPON): minPower = 15 + (level-1)*9.
    {EnchantmentId::Looting, "looting", 1, 3, EnchantmentRarity::Rare,
     EnchantmentCategory::Weapon, {15, 9, 50}, false, false, true},
    // SweepingEnchantment: minPower = 5 + (level-1)*9.
    {EnchantmentId::Sweeping, "sweeping", 1, 3, EnchantmentRarity::Rare,
     EnchantmentCategory::Weapon, {5, 9, 15}, false, false, true},
    // EfficiencyEnchantment: minPower = 1 + 10*(level-1); maxOffset = 50 (note:
    // vanilla's getMaxPower calls super.getMinPower, i.e. the SAME curve, so it
    // really is minPower(level)+50 despite the odd "super." spelling in source).
    {EnchantmentId::Efficiency, "efficiency", 1, 5, EnchantmentRarity::Common,
     EnchantmentCategory::Digger, {1, 10, 50}, false, false, true},
    // SilkTouchEnchantment: minPower always 15 (perLevel irrelevant, max level 1).
    {EnchantmentId::SilkTouch, "silk_touch", 1, 1, EnchantmentRarity::VeryRare,
     EnchantmentCategory::Digger, {15, 0, 50}, false, false, true},
    // UnbreakingEnchantment: minPower = 5 + (level-1)*8.
    {EnchantmentId::Unbreaking, "unbreaking", 1, 3, EnchantmentRarity::Uncommon,
     EnchantmentCategory::Breakable, {5, 8, 50}, false, false, true},
    // LuckEnchantment (Fortune instance, EnchantmentTarget.DIGGER): same curve shape as Looting.
    {EnchantmentId::Fortune, "fortune", 1, 3, EnchantmentRarity::Rare,
     EnchantmentCategory::Digger, {15, 9, 50}, false, false, true},
    // PowerEnchantment: minPower = 1 + (level-1)*10.
    {EnchantmentId::Power, "power", 1, 5, EnchantmentRarity::Common,
     EnchantmentCategory::Bow, {1, 10, 15}, false, false, true},
    // PunchEnchantment: minPower = 12 + (level-1)*20.
    {EnchantmentId::Punch, "punch", 1, 2, EnchantmentRarity::Rare,
     EnchantmentCategory::Bow, {12, 20, 25}, false, false, true},
    // FlameEnchantment: flat 20..50.
    {EnchantmentId::Flame, "flame", 1, 1, EnchantmentRarity::Rare,
     EnchantmentCategory::Bow, {20, 0, 30}, false, false, true},
    // InfinityEnchantment: flat 20..50.
    {EnchantmentId::Infinity, "infinity", 1, 1, EnchantmentRarity::VeryRare,
     EnchantmentCategory::Bow, {20, 0, 30}, false, false, true},
    // LuckEnchantment (LuckOfTheSea instance, EnchantmentTarget.FISHING_ROD).
    {EnchantmentId::LuckOfTheSea, "luck_of_the_sea", 1, 3, EnchantmentRarity::Rare,
     EnchantmentCategory::FishingRod, {15, 9, 50}, false, false, true},
    // LureEnchantment: same curve shape.
    {EnchantmentId::Lure, "lure", 1, 3, EnchantmentRarity::Rare,
     EnchantmentCategory::FishingRod, {15, 9, 50}, false, false, true},
    // LoyaltyEnchantment: minPower = 5 + level*7 (NOT (level-1)*7 — vanilla's
    // own formula already includes one level's worth in the additive term).
    // Re-expressed in this table's base+(level-1)*perLevel shape, that is
    // base'=5+7=12, perLevel=7 (so minPower(level) = 12 + (level-1)*7 = 5 +
    // level*7, identical for every level — verified against level 1 and 3 in
    // enchantment_registry_test's worked-example test). maxPower is a flat
    // 50, not minPower+offset, so it cannot be expressed via maxOffset at
    // all; handled as a special case in getMaxCost() via
    // enchantmentHasFlatMaxCostOf50() below.
    {EnchantmentId::Loyalty, "loyalty", 1, 3, EnchantmentRarity::Uncommon,
     EnchantmentCategory::Trident, {12, 7, 0}, false, false, true},
    // ImpalingEnchantment: minPower = 1 + (level-1)*8.
    {EnchantmentId::Impaling, "impaling", 1, 5, EnchantmentRarity::Rare,
     EnchantmentCategory::Trident, {1, 8, 20}, false, false, true},
    // RiptideEnchantment: minPower = 10 + level*7 -> base'=10+7=17, perLevel=7
    // (same re-expression as Loyalty above); maxPower flat 50, same special
    // case.
    {EnchantmentId::Riptide, "riptide", 1, 3, EnchantmentRarity::Rare,
     EnchantmentCategory::Trident, {17, 7, 0}, false, false, true},
    // ChannelingEnchantment: flat 25..50.
    {EnchantmentId::Channeling, "channeling", 1, 1, EnchantmentRarity::VeryRare,
     EnchantmentCategory::Trident, {25, 0, 25}, false, false, true},
    // MultishotEnchantment: flat 20..50.
    {EnchantmentId::Multishot, "multishot", 1, 1, EnchantmentRarity::Rare,
     EnchantmentCategory::Crossbow, {20, 0, 30}, false, false, true},
    // QuickChargeEnchantment: minPower = 12 + (level-1)*20; maxPower flat 50
    // (special case, see enchantmentMaxCost()).
    {EnchantmentId::QuickCharge, "quick_charge", 1, 3, EnchantmentRarity::Uncommon,
     EnchantmentCategory::Crossbow, {12, 20, 0}, false, false, true},
    // PiercingEnchantment: minPower = 1 + (level-1)*10; maxPower flat 50
    // (special case, see enchantmentMaxCost()).
    {EnchantmentId::Piercing, "piercing", 1, 4, EnchantmentRarity::Common,
     EnchantmentCategory::Crossbow, {1, 10, 0}, false, false, true},
    // MendingEnchantment: minPower = level*25 -> base=25, perLevel=25 (level
    // always 1 since maxLevel==1, so "per level" never actually applies, but
    // the transcription follows vanilla's own formula shape); treasure-only.
    {EnchantmentId::Mending, "mending", 1, 1, EnchantmentRarity::Rare,
     EnchantmentCategory::Breakable, {25, 25, 50}, true, false, true},
    // VanishingCurseEnchantment: flat 25..50, treasure + curse.
    {EnchantmentId::VanishingCurse, "vanishing_curse", 1, 1, EnchantmentRarity::VeryRare,
     EnchantmentCategory::Vanishable, {25, 0, 25}, true, true, true},
}};

static_assert(kEnchantmentTable.size() == kEnchantmentCount,
              "kEnchantmentTable must have exactly one row per EnchantmentId");

// The table-offer path's own well-formedness check: row `i`'s id field must
// equal `i` (the table is written in enum-declaration order by hand, so a
// reordered or skipped row is a silent corruption without this).
[[nodiscard]] constexpr bool enchantmentTableIsWellFormed() {
    for (std::size_t index = 0; index < kEnchantmentTable.size(); ++index) {
        if (static_cast<std::size_t>(kEnchantmentTable[index].id) != index) return false;
        if (kEnchantmentTable[index].minLevel < 1) return false;
        if (kEnchantmentTable[index].maxLevel < kEnchantmentTable[index].minLevel) return false;
    }
    return true;
}
static_assert(enchantmentTableIsWellFormed(),
              "kEnchantmentTable rows must be in EnchantmentId order with sane level bounds");

[[nodiscard]] constexpr const EnchantmentDefinition& enchantmentDefinition(EnchantmentId id) {
    return kEnchantmentTable[static_cast<std::size_t>(id)];
}

[[nodiscard]] constexpr std::uint8_t enchantmentMaxLevel(EnchantmentId id) {
    return enchantmentDefinition(id).maxLevel;
}

[[nodiscard]] constexpr EnchantmentRarity enchantmentRarity(EnchantmentId id) {
    return enchantmentDefinition(id).rarity;
}

[[nodiscard]] constexpr EnchantmentCategory enchantmentCategory(EnchantmentId id) {
    return enchantmentDefinition(id).category;
}

[[nodiscard]] constexpr bool enchantmentIsTreasureOnly(EnchantmentId id) {
    return enchantmentDefinition(id).treasureOnly;
}

[[nodiscard]] constexpr bool enchantmentIsCurse(EnchantmentId id) {
    return enchantmentDefinition(id).curse;
}

[[nodiscard]] constexpr bool enchantmentAvailableForRandomSelection(EnchantmentId id) {
    return enchantmentDefinition(id).availableForRandomSelection;
}

// The ids whose vanilla getMaxPower is a flat 50 rather than
// minPower(level)+maxOffset (Loyalty, Riptide, QuickCharge, Piercing — every
// one of them a 1.16.1 override that returns the literal constant `50`
// instead of calling into minPower at all). Every other enchantment's
// maxOffset field already encodes minPower+offset==maxPower exactly.
[[nodiscard]] constexpr bool enchantmentHasFlatMaxCostOf50(EnchantmentId id) {
    return id == EnchantmentId::Loyalty || id == EnchantmentId::Riptide ||
        id == EnchantmentId::QuickCharge || id == EnchantmentId::Piercing;
}

// Enchantment#getMinPower / #getMaxPower (EnchantmentHelper.getPossibleEntries
// calls these "getMinPower"/"getMaxPower"; other call sites' comments in this
// codebase call the concept "cost", matching calculateRequiredExperienceLevel's
// return value's own name — same numbers, same curve, just named for the two
// different call sites that read them).
[[nodiscard]] constexpr std::int32_t getMinCost(EnchantmentId id, std::int32_t level) {
    return detail::minPower(enchantmentDefinition(id).cost, level);
}
[[nodiscard]] constexpr std::int32_t getMaxCost(EnchantmentId id, std::int32_t level) {
    if (enchantmentHasFlatMaxCostOf50(id)) {
        return 50;
    }
    return detail::maxPower(enchantmentDefinition(id).cost, level);
}

// EnchantmentTarget#isAcceptableItem, restricted to what this build can
// actually place an enchantment question against today: a tool's ToolType (or
// a book, which accepts every category exactly like vanilla's `bl` bypass in
// getPossibleEntries). Armor/bow/fishing-rod/trident/crossbow/shears items do
// not exist in the registry yet (AR content gap tracked elsewhere; ENCH-0
// does not add them), so those categories correctly answer false for every
// item this build can hand it — not a false positive, just nothing to say yes
// to yet.
[[nodiscard]] constexpr bool categoryAcceptsToolType(EnchantmentCategory category,
                                                      ToolType toolType) {
    switch (category) {
    case EnchantmentCategory::Weapon: return toolType == ToolType::Sword;
    case EnchantmentCategory::Digger:
        return toolType == ToolType::Pickaxe || toolType == ToolType::Axe ||
            toolType == ToolType::Shovel || toolType == ToolType::Hoe;
    case EnchantmentCategory::Breakable:
        // BREAKABLE accepts anything damageable; every registered tool wears
        // durability, so all five ToolTypes qualify. (Armor, when it exists,
        // will also qualify here the same way ArmorItem does in vanilla.)
        return toolType == ToolType::Sword || toolType == ToolType::Pickaxe ||
            toolType == ToolType::Axe || toolType == ToolType::Shovel ||
            toolType == ToolType::Hoe;
    case EnchantmentCategory::Vanishable:
        // VANISHABLE = Vanishable-marked items OR BREAKABLE's own set
        // (EnchantmentTarget.java's `|| BREAKABLE.isAcceptableItem(item)`).
        // No item in this registry implements the Vanishable marker interface
        // yet, so this collapses to the Breakable set for now.
        return toolType == ToolType::Sword || toolType == ToolType::Pickaxe ||
            toolType == ToolType::Axe || toolType == ToolType::Shovel ||
            toolType == ToolType::Hoe;
    case EnchantmentCategory::Armor:
    case EnchantmentCategory::ArmorFeet:
    case EnchantmentCategory::ArmorLegs:
    case EnchantmentCategory::ArmorChest:
    case EnchantmentCategory::ArmorHead:
    case EnchantmentCategory::FishingRod:
    case EnchantmentCategory::Trident:
    case EnchantmentCategory::Bow:
    case EnchantmentCategory::Wearable:
    case EnchantmentCategory::Crossbow:
        return false;
    }
    return false;
}

// Item#getEnchantability (0 = not enchantable at all): the wood/stone/iron/
// gold/diamond ToolMaterials values, plus 1 for a book (BookItem's fixed
// override). Everything else (no durability, no ToolType, not a book) is 0,
// matching Item's own default.
[[nodiscard]] constexpr std::int32_t itemEnchantability(const ItemStack& stack) {
    if (stack.item == &items::Book) {
        return 1;
    }
    if (stack.item == nullptr || stack.item->toolType == ToolType::None) {
        return 0;
    }
    switch (stack.item->toolTier) {
    case ToolTier::Wood: return 15;
    case ToolTier::Stone: return 5;
    case ToolTier::Iron: return 14;
    case ToolTier::Gold: return 22;
    case ToolTier::Diamond: return 10;
    case ToolTier::None: return 0;
    }
    return 0;
}

// Enchantment#isAcceptableItem: whether `id` may ever land on `stack` at all
// (the table-offer generator and a future /enchant command both gate on
// this). A book accepts every enchantment (EnchantmentHelper.getPossibleEntries'
// `|| bl` bypass), matching vanilla's "any enchantment can go on a book"
// design that lets the loot table generate any enchanted book.
[[nodiscard]] constexpr bool canEnchant(EnchantmentId id, const ItemStack& stack) {
    if (stack.item == &items::Book) {
        return true;
    }
    if (stack.item == nullptr) {
        return false;
    }
    return categoryAcceptsToolType(enchantmentDefinition(id).category, stack.item->toolType);
}

// Enchantment#canCombine (both directions must agree; every 1.16.1
// canAccept() override is symmetric in practice — a pair either both refuse
// each other or neither does — so a single symmetric table suffices). `id`
// combining with itself is not "compatible" in the applying sense (you cannot
// stack two Sharpness entries as two separate enchantments — a second
// application replaces/raises the level instead), so the identity case
// answers false the way the table-offer's removeConflicts step needs (an
// already-picked enchantment must remove itself, along with everything
// incompatible with it, from the remaining pool).
[[nodiscard]] constexpr bool isCompatibleWith(EnchantmentId first, EnchantmentId second) {
    if (first == second) {
        return false;
    }
    // Protection family: any two distinct Protection-type enchantments
    // conflict UNLESS one of them is Feather Falling (ProtectionEnchantment's
    // canAccept: same protectionType -> false; otherwise true only if either
    // side is FALL type). Feather Falling is the only FALL-type member.
    const auto isProtectionFamily = [](EnchantmentId id) {
        return id == EnchantmentId::Protection || id == EnchantmentId::FireProtection ||
            id == EnchantmentId::FeatherFalling || id == EnchantmentId::BlastProtection ||
            id == EnchantmentId::ProjectileProtection;
    };
    if (isProtectionFamily(first) && isProtectionFamily(second)) {
        return first == EnchantmentId::FeatherFalling || second == EnchantmentId::FeatherFalling;
    }
    // DamageEnchantment family: Sharpness/Smite/BaneOfArthropods are mutually
    // exclusive with each other (canAccept: `!(other instanceof DamageEnchantment)`).
    const auto isDamageFamily = [](EnchantmentId id) {
        return id == EnchantmentId::Sharpness || id == EnchantmentId::Smite ||
            id == EnchantmentId::BaneOfArthropods;
    };
    if (isDamageFamily(first) && isDamageFamily(second)) {
        return false;
    }
    // LuckEnchantment (Fortune/Looting/LuckOfTheSea/Lure all share the class,
    // but only Fortune declares the SilkTouch conflict; Looting/LuckOfTheSea/
    // Lure never collide with each other since they occupy different
    // categories anyway and canAccept only special-cases SilkTouch).
    const auto pair = [&](EnchantmentId a, EnchantmentId b) {
        return (first == a && second == b) || (first == b && second == a);
    };
    if (pair(EnchantmentId::Fortune, EnchantmentId::SilkTouch)) return false;
    if (pair(EnchantmentId::Infinity, EnchantmentId::Mending)) return false;
    if (pair(EnchantmentId::DepthStrider, EnchantmentId::FrostWalker)) return false;
    if (pair(EnchantmentId::Multishot, EnchantmentId::Piercing)) return false;
    if (pair(EnchantmentId::Riptide, EnchantmentId::Loyalty)) return false;
    if (pair(EnchantmentId::Riptide, EnchantmentId::Channeling)) return false;
    return true;
}

[[nodiscard]] constexpr std::string_view enchantmentVanillaName(EnchantmentId id) {
    return enchantmentDefinition(id).vanillaName;
}

// Typed wrappers over ItemStack's raw (storage-typed) enchantment accessors —
// ItemStack itself only knows EnchantmentIdStorage (Inventory.hpp does not
// depend on this file), so every gameplay call site reaches for these instead
// of casting by hand.
[[nodiscard]] constexpr std::uint8_t enchantmentLevel(const ItemStack& stack, EnchantmentId id) {
    return stack.enchantmentLevelRaw(static_cast<EnchantmentIdStorage>(id));
}
constexpr void setEnchantmentLevel(ItemStack& stack, EnchantmentId id, std::uint8_t level) {
    stack.setEnchantmentRaw(static_cast<EnchantmentIdStorage>(id), level);
}
[[nodiscard]] constexpr bool hasEnchantment(const ItemStack& stack, EnchantmentId id) {
    return enchantmentLevel(stack, id) > 0U;
}

} // namespace mc::gameplay

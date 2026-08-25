#pragma once

// ENCH-1b: mining-tool enchantment effects — Efficiency / Unbreaking / Fortune /
// Silk Touch, the mining half of the ENCH.gate. ENCH-0 gave every ItemStack an
// enchantment list; ENCH-1 filled the outgoing melee weapon effects. Nothing
// read the *digger* enchantments to change a break, a durability spend, or a
// drop count until this file.
//
// Same "真 datapack 化" model as EQ-4 (ArmorEnchantment.hpp): the numeric part of
// each effect is NOT a hand-written `speed += level*level+1` / `count *= …`
// branch per enchantment. It is driven by the DDC-2 effect-component engine —
// each enchantment's `effects` shape is embedded below as the baked floor, JSON
// in the exact form a data/minecraft/enchantment/*.json file carries, compiled
// once at load through data::effect::compileEffectsText into the flat
// EffectProgram POD, then evaluated at runtime by data::effect::
// applyDamageModifiers (the engine's generic per-level value transform, reused
// here exactly as EQ-4 reused applyDamageProtection). The gameplay call sites
// (MiningSystem's mining speed, GameSession's durability spend, MiningSystem's
// drop roll) consume those values — no per-enchant arithmetic outside the
// vanilla fold, no reduction/multiply math in the mining code.
//
// Why embedded here rather than the enchantment JSON's `effects` field: the same
// reason EQ-4 states — the shipped datapack files under resources/data/rebedrock/
// enchantment/ are held byte-identical to what the constexpr ENCH-0 table
// generates (the enchantment_migration golden test enforces it), and ENCH-0's
// table does not model `effects`. So the effect component is the baked floor
// here, exactly as Enchantment.hpp is the baked floor the generated files mirror.
// The evaluation path is identical either way (same JSON → compileEffects →
// runtime eval), so a future datapack-overlay path is a drop-in.
//
// The DDC-2 engine models `minecraft:damage` / `damage_protection` value buckets
// and `post_attack` actions — it has no dedicated "mining_efficiency attribute"
// or "loot bonus" component, and this node must NOT change the engine body.
// So each mining effect's *value curve* is carried as a generic `minecraft:damage`
// value term (an `add`/`set` over a per-level LevelBasedValue), the engine's
// general-purpose value transform, and read with applyDamageModifiers against a
// zero base — the value that comes back is the pure per-level number (level²+1,
// the Unbreaking skip probability, the Fortune bonus ceiling), never a damage.
// This is the same reuse EQ-4 made of `damage_protection` for a non-combat idea.
//
// Determinism (REGULAR.md / ENCH-DESIGN §3): every probabilistic draw goes
// through a world::gen::JavaRandom the caller owns and seeds (Unbreaking's per-
// point skip, Fortune's ore-bonus roll), never a wall clock or a global rng.
// Unbreaking's skip is drawn through the DDC-2 engine's own random_chance
// predicate (ctx.random->nextFloat() < chance) so the RNG genuinely flows through
// the effect engine, not a side path. Fortune's uniform-int bonus draw (vanilla's
// random.nextInt(level+2)) has no int primitive in DDC-2, so the *ceiling* comes
// from the engine and the caller performs the nextInt draw against ctx.random —
// the same "engine decides the magnitude, gameplay owns the draw/mutation" split
// DDC-2's PostAttackOutcome contract establishes for Thorns (JC note at bottom).
//
// DOD: the compiled programs are cached in function-local statics; each query is
// a subscript + a handful of multiply-adds, no allocation on the mining hot path.

#include "data/effect/EffectCompiler.hpp"
#include "data/effect/EffectRuntime.hpp"
#include "gameplay/Enchantment.hpp"
#include "gameplay/Inventory.hpp"
#include "world/gen/JavaRandom.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mc::gameplay {

// The four mining-tool enchantments ENCH-1b drives. A small fixed list so a
// query walks exactly these, never the whole registry.
inline constexpr std::array<EnchantmentId, 4> kMiningEffectEnchantments{
    EnchantmentId::Efficiency,
    EnchantmentId::Unbreaking,
    EnchantmentId::Fortune,
    EnchantmentId::SilkTouch,
};

namespace detail {

// efficiency: PlayerEntity#getBlockBreakingSpeed adds `i*i + 1` (i = level) to
// the tool's breaking speed. Carried as a `minecraft:damage` add over a
// levels_squared curve with additive base 1 — levels_squared evaluates to
// `base + level*level`, i.e. level²+1, which applyDamageModifiers returns against
// a zero base. (JE 26.1 expresses this as a mining_efficiency attribute; DDC-2
// has no attribute component, so the generic value bucket carries the same curve.)
inline constexpr std::string_view kEfficiencyEffects = R"JSON({
  "minecraft:damage": [
    { "effect": { "type": "minecraft:add",
        "value": { "type": "minecraft:levels_squared", "added": 1.0 } } }
  ]
})JSON";

// unbreaking: UnbreakingEnchantment#shouldPreventDamage (non-armor) is
// `random.nextInt(level+1) > 0`, i.e. a durability point is skipped with
// probability level/(level+1). Carried as a `minecraft:damage` `set` to 1 gated
// by a random_chance whose per-level chance is the lookup {1/2, 2/3, 3/4} for
// levels 1..3 — so applyDamageModifiers returns 1 (skip) exactly when the
// engine's random_chance predicate fires against ctx.random, else 0 (spend).
// The draw is the engine's own nextFloat < chance, deterministic per seed.
inline constexpr std::string_view kUnbreakingEffects = R"JSON({
  "minecraft:damage": [
    { "effect": { "type": "minecraft:set",
        "value": { "type": "minecraft:constant", "value": 1.0 } },
      "requirements": { "condition": "minecraft:random_chance",
        "chance": { "type": "minecraft:lookup", "values": [0.5, 0.6666667, 0.75],
          "fallback": { "type": "minecraft:constant", "value": 0.75 } } } }
  ]
})JSON";

// fortune: ApplyBonusLootFunction.OreDrops multiplies the ore item count by
// `max(0, random.nextInt(level+2) - 1) + 1`. The uniform-int bound `level+2` is
// carried as a `minecraft:damage` add over a linear curve (base 2 at level 1,
// +1 per level → level+1... expressed base 3, per_level 1 gives level+2), read
// with applyDamageModifiers against a zero base; the caller draws the nextInt
// against that ceiling and applies the vanilla fold. No RNG lives in this curve.
inline constexpr std::string_view kFortuneEffects = R"JSON({
  "minecraft:damage": [
    { "effect": { "type": "minecraft:add",
        "value": { "type": "minecraft:linear", "base": 3.0, "per_level_above_first": 1.0 } } }
  ]
})JSON";

// silk_touch: no value curve — it is a pure "drop the block itself" switch with
// no level dependence and no RNG. An empty program (no damage/protection/
// post_attack term); silkTouchYieldsSelf() below reads only the level, so this
// exists so silk_touch compiles cleanly through DDC-2 alongside its siblings and
// its unknown counters stay zero.
inline constexpr std::string_view kSilkTouchEffects = R"JSON({})JSON";

[[nodiscard]] inline std::string_view miningEffectJson(EnchantmentId id) {
    switch (id) {
    case EnchantmentId::Efficiency: return kEfficiencyEffects;
    case EnchantmentId::Unbreaking: return kUnbreakingEffects;
    case EnchantmentId::Fortune: return kFortuneEffects;
    case EnchantmentId::SilkTouch: return kSilkTouchEffects;
    default: return {};
    }
}

// The compiled program for each mining enchantment, in kMiningEffectEnchantments
// order. Compiled once (load-time, off the mining hot path) through DDC-2's
// compileEffectsText and cached in the function-local static, so a per-break
// query is a subscript, never a re-parse.
[[nodiscard]] inline const std::array<data::effect::EffectProgram, 4>& miningEffectPrograms() {
    static const std::array<data::effect::EffectProgram, 4> programs = [] {
        std::array<data::effect::EffectProgram, 4> built{};
        for (std::size_t index = 0; index < kMiningEffectEnchantments.size(); ++index) {
            const EnchantmentId id = kMiningEffectEnchantments[index];
            built[index] = data::effect::compileEffectsText(
                miningEffectJson(id), static_cast<std::int32_t>(enchantmentMaxLevel(id)));
        }
        return built;
    }();
    return programs;
}

inline constexpr std::size_t kEfficiencyIndex = 0;
inline constexpr std::size_t kUnbreakingIndex = 1;
inline constexpr std::size_t kFortuneIndex = 2;

[[nodiscard]] inline const data::effect::EffectProgram& miningEffectProgram(std::size_t index) {
    return miningEffectPrograms()[index];
}

} // namespace detail

// ---------------------------------------------------------------------------
// Efficiency — mining speed bonus (PlayerEntity#getBlockBreakingSpeed).
// ---------------------------------------------------------------------------

// The speed added to a tool's breaking speed by Efficiency `level`: level²+1,
// or 0 when the tool carries no Efficiency (identity — a no-enchant break is
// never faster). Driven through DDC-2: applyDamageModifiers over the levels_
// squared curve against a zero base. The caller adds this to the tool's speed
// exactly where vanilla's `f += i*i + 1` sits (guarded by `f > 1`, i.e. only a
// tool already faster than a fist gets the bonus — that guard is the caller's).
[[nodiscard]] inline float efficiencyMiningSpeedBonus(std::uint8_t level) {
    if (level == 0U) {
        return 0.0F;  // sabotage ③: no enchant ⇒ no bonus, never level 0 math
    }
    data::effect::EffectContext ctx{};
    ctx.level = static_cast<std::int32_t>(level);
    ctx.baseAmount = 0.0F;
    return data::effect::applyDamageModifiers(detail::miningEffectProgram(detail::kEfficiencyIndex),
                                              ctx);
}

// Convenience over a held stack.
[[nodiscard]] inline float efficiencyMiningSpeedBonus(const ItemStack& tool) {
    return efficiencyMiningSpeedBonus(enchantmentLevel(tool, EnchantmentId::Efficiency));
}

// ---------------------------------------------------------------------------
// Unbreaking — probabilistic durability preservation (ItemStack#damage).
// ---------------------------------------------------------------------------

// Whether one durability point is prevented by Unbreaking `level`, drawn through
// DDC-2's random_chance predicate against `random`. Probability level/(level+1),
// matching UnbreakingEnchantment#shouldPreventDamage's `nextInt(level+1) > 0`
// (same probability; the draw is the engine's nextFloat, a documented JC
// deviation from vanilla's nextInt stream — determinism is preserved). Level 0
// never prevents. Every draw comes from `random`, never a wall clock (sabotage ①).
[[nodiscard]] inline bool unbreakingPreventsPoint(std::uint8_t level,
                                                  world::gen::JavaRandom& random) {
    if (level == 0U) {
        return false;
    }
    data::effect::EffectContext ctx{};
    ctx.level = static_cast<std::int32_t>(level);
    ctx.baseAmount = 0.0F;
    ctx.random = &random;
    // The set-to-1 term fires only when random_chance succeeds; a return > 0 is a
    // prevented point.
    return data::effect::applyDamageModifiers(detail::miningEffectProgram(detail::kUnbreakingIndex),
                                              ctx) > 0.5F;
}

// The durability actually spent for a base cost of `baseCost` points on a tool
// with Unbreaking `level`, mirroring ItemStack#damage's per-point loop: each of
// the `baseCost` points is independently skipped with probability level/(level+1).
// Deterministic for a given seed (the whole point sequence is replayable). With
// no Unbreaking this returns baseCost unchanged (identity).
[[nodiscard]] inline std::uint16_t unbreakingDurabilityCost(std::uint16_t baseCost,
                                                            std::uint8_t level,
                                                            world::gen::JavaRandom& random) {
    if (level == 0U || baseCost == 0U) {
        return baseCost;
    }
    std::uint16_t prevented = 0U;
    for (std::uint16_t point = 0U; point < baseCost; ++point) {
        if (unbreakingPreventsPoint(level, random)) {
            ++prevented;
        }
    }
    return static_cast<std::uint16_t>(baseCost - prevented);
}

// ---------------------------------------------------------------------------
// Fortune — ore drop count bonus (ApplyBonusLootFunction.OreDrops).
// ---------------------------------------------------------------------------

// The multiplied drop count for an ore that drops `baseCount` items, mined with
// Fortune `level`, mirroring OreDrops#getValue:
//   i = random.nextInt(level+2) - 1; if (i < 0) i = 0; return baseCount*(i+1).
// The uniform-int ceiling `level+2` comes from DDC-2 (applyDamageModifiers over
// the linear curve against a zero base); the nextInt draw is the caller's against
// `random` (DDC-2 has no int primitive). Level 0 returns baseCount unchanged
// (identity — a plain break never multiplies). Deterministic per seed.
[[nodiscard]] inline std::uint8_t fortuneDropCount(std::uint8_t baseCount, std::uint8_t level,
                                                   world::gen::JavaRandom& random) {
    if (level == 0U || baseCount == 0U) {
        return baseCount;
    }
    data::effect::EffectContext ctx{};
    ctx.level = static_cast<std::int32_t>(level);
    ctx.baseAmount = 0.0F;
    const float ceilingValue =
        data::effect::applyDamageModifiers(detail::miningEffectProgram(detail::kFortuneIndex), ctx);
    const auto bound = static_cast<std::int32_t>(ceilingValue);  // level+2
    if (bound <= 0) {
        return baseCount;
    }
    std::int32_t bonus = random.nextInt(bound) - 1;  // random.nextInt(level+2) - 1
    if (bonus < 0) {
        bonus = 0;
    }
    const int multiplied = static_cast<int>(baseCount) * (bonus + 1);
    return static_cast<std::uint8_t>(multiplied > 255 ? 255 : multiplied);
}

// The Fortune bonus ceiling `level+2` alone (the DDC-2 value), for a caller that
// owns its draw on a non-JavaRandom deterministic stream (the loot path's
// mc::rng state). Zero for no Fortune. Kept so the loot stream stays a single
// mc::rng sequence rather than splicing a JavaRandom in mid-roll — the int draw
// itself is the caller's, exactly as the JavaRandom overload documents.
[[nodiscard]] inline std::int32_t fortuneBonusCeiling(std::uint8_t level) {
    if (level == 0U) {
        return 0;
    }
    data::effect::EffectContext ctx{};
    ctx.level = static_cast<std::int32_t>(level);
    ctx.baseAmount = 0.0F;
    return static_cast<std::int32_t>(
        data::effect::applyDamageModifiers(detail::miningEffectProgram(detail::kFortuneIndex), ctx));
}

// Folds a drawn `nextInt(level+2)` value into OreDrops#getValue's multiplier for
// a base count. `uniformDraw` is in [0, ceiling) from the caller's own stream.
[[nodiscard]] inline std::uint8_t fortuneApply(std::uint8_t baseCount, std::int32_t uniformDraw) {
    std::int32_t bonus = uniformDraw - 1;  // random.nextInt(level+2) - 1
    if (bonus < 0) {
        bonus = 0;
    }
    const int multiplied = static_cast<int>(baseCount) * (bonus + 1);
    return static_cast<std::uint8_t>(multiplied > 255 ? 255 : multiplied);
}

// ---------------------------------------------------------------------------
// Silk Touch — drop the block itself (SilkTouchEnchantment, loot condition).
// ---------------------------------------------------------------------------

// Whether the tool's Silk Touch should make a break drop the block itself rather
// than its normal loot. A pure switch: true iff the tool carries Silk Touch
// (level >= 1). Silk Touch and Fortune are mutually exclusive on one stack
// (ENCH-0's isCompatibleWith already refuses the pair), so a well-formed tool
// never has both; silkTouchYieldsSelf takes precedence at the call site to make
// the exclusivity observable even on a hand-forced stack (sabotage ②).
[[nodiscard]] inline bool silkTouchYieldsSelf(const ItemStack& tool) {
    return hasEnchantment(tool, EnchantmentId::SilkTouch);
}

// JC note: Unbreaking's skip draw uses the engine's nextFloat<chance rather than
// vanilla's nextInt(level+1)>0 (same probability, different stream), and
// Fortune's bonus ceiling comes from DDC-2 while the nextInt draw itself is the
// caller's — both because DDC-2 models no integer/attribute/loot component and
// this node must not change the engine body. Documented in ENCH-DESIGN §5 as an
// ENCH deviation for a future JC replay to reconcile.

} // namespace mc::gameplay

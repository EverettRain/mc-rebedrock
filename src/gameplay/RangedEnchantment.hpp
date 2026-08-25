#pragma once

// RW-4: ranged (bow) enchantment effects — Power / Punch / Flame / Infinity, the
// remote half of the ENCH.gate. ENCH-0 (Enchantment.hpp) gave every ItemStack an
// enchantment list; RW-0/1/1a built the projectile pool and the bow's charge/
// release; nothing read the *bow's* enchantments to change an arrow until this
// file.
//
// Same "真 datapack 化" model as EQ-4 (ArmorEnchantment.hpp) and ENCH-1b
// (EnchantmentMining.hpp): the numeric part of each effect is NOT a hand-written
// `damage *= 1 + 0.25*(level+1)` / `knockback += level` branch per enchantment.
// It is driven by the DDC-2 effect-component engine — each enchantment's `effects`
// shape is embedded below as the baked floor, JSON in the exact form a
// data/minecraft/enchantment/*.json file carries, compiled once at load through
// data::effect::compileEffectsText into the flat EffectProgram POD, then evaluated
// at runtime by data::effect::applyDamageModifiers (Power / Punch value curves)
// and data::effect::runPostAttack (Flame's ignite action) — the same two engine
// surfaces EQ-4 and ENCH-1b reuse, no per-enchant arithmetic outside the vanilla
// fold, no engine-body change.
//
// Why embedded here rather than the enchantment JSON's `effects` field: the same
// reason EQ-4 / ENCH-1b state — the shipped datapack files under
// resources/data/rebedrock/enchantment/ are held byte-identical to what the
// constexpr ENCH-0 table generates (the enchantment_migration golden test
// enforces it), and ENCH-0's table does not model `effects`. So the effect
// component is the baked floor here; the evaluation path is identical either way
// (same JSON → compileEffects → runtime eval), so a future datapack-overlay path
// is a drop-in.
//
// Scope (RW-DESIGN §2, this node's bow shard): Power (arrow damage bonus),
// Punch (extra knockback), Flame (ignite the target), Infinity (a survival shot
// consumes no arrow). Trident / crossbow enchantments (Loyalty / Riptide /
// Channeling / Impaling / Piercing / Multishot / Quick Charge) press on weapons
// that do not exist yet (RW-2 / RW-3 are deferred) and stay registered-but-
// unimplemented gates (RW-DESIGN §2 / ENCH-DESIGN §4.2), NOT stubs here.
//
// DDC-2 has no dedicated "projectile power attribute" or "knockback" component,
// so — exactly as ENCH-1b carried Efficiency's mining-speed curve as a generic
// `minecraft:damage` value term — Power's damage-multiplier factor and Punch's
// knockback strength are each carried as a `minecraft:damage` value curve read
// against a ZERO base (the returned number is the pure per-level factor/strength,
// never a damage), and the gameplay caller folds it. Flame is a real
// `minecraft:post_attack` `ignite` action, the engine's own ActionKind::Ignite,
// reported as a PostAttackOutcome the caller applies via EntitySystem::setOnFire.
//
// Determinism (REGULAR.md / ENCH-DESIGN §3): none of Power/Punch/Flame draws RNG
// (all deterministic per-level values / a flat ignite duration); the projectile
// pool's own JavaRandom stream still drives scatter/crit exactly as RW-1a set it.
// No wall clock, no global rng, anywhere on this path.
//
// DOD: the compiled programs are cached in function-local statics; each query is
// a subscript + a handful of multiply-adds, no allocation on the shot hot path.

#include "data/effect/EffectCompiler.hpp"
#include "data/effect/EffectRuntime.hpp"
#include "gameplay/Enchantment.hpp"
#include "gameplay/Inventory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mc::gameplay {

// The four bow enchantments RW-4 drives. A small fixed list so a query walks
// exactly these, never the whole registry.
inline constexpr std::array<EnchantmentId, 4> kBowEffectEnchantments{
    EnchantmentId::Power,
    EnchantmentId::Punch,
    EnchantmentId::Flame,
    EnchantmentId::Infinity,
};

namespace detail {

// power: PowerEnchantment#getAttackDamage adds a damage MULTIPLIER factor of
// `0.25 * (level + 1)` (RW-DESIGN §2 / the task's `baseDamage*(1+0.25*(level+1))`
// form). Carried as a `minecraft:damage` add over a linear curve read against a
// zero base — at level L the value is `0.25*(L+1) = 0.5 + 0.25*(L-1)`, i.e.
// base 0.5, per_level_above_first 0.25 — so applyDamageModifiers returns the pure
// factor and powerDamageFactor() below folds it into `base*(1+factor)`. (JE 26.1
// expresses this on the arrow's base-damage setter; DDC-2 has no attribute
// component, so the generic value bucket carries the same curve — the identical
// reuse ENCH-1b made for Efficiency.)
inline constexpr std::string_view kPowerEffects = R"JSON({
  "minecraft:damage": [
    { "effect": { "type": "minecraft:add",
        "value": { "type": "minecraft:linear", "base": 0.5, "per_level_above_first": 0.25 } } }
  ]
})JSON";

// punch: PunchEnchantment adds `level` to the arrow's knockback strength
// (AbstractArrow#setKnockback(punchLevel)). Carried as a `minecraft:damage` add
// over a linear curve against a zero base — base 0.5, per_level 0.5 gives
// `0.5*level`, matching the knockback-STRENGTH unit this codebase's
// EntitySystem::hurt(extraKnockbackStrength) already uses for melee Knockback
// (EnchantmentCombat.hpp's meleeKnockbackEnchantBonus, `0.5*level`). JC note: an
// exact vanilla replay pushes the target by `knockback * 0.6` blocks along the
// arrow's motion; this reuses the single-application strength unit the melee
// Knockback path established, a documented ENCH/RW deviation (ENCH-DESIGN §4.2).
inline constexpr std::string_view kPunchEffects = R"JSON({
  "minecraft:damage": [
    { "effect": { "type": "minecraft:add",
        "value": { "type": "minecraft:linear", "base": 0.5, "per_level_above_first": 0.5 } } }
  ]
})JSON";

// flame: FlameEnchantment sets the struck target on fire for 100 game seconds
// (AbstractArrow's `setSecondsOnFire(100)` when the firing bow carries Flame,
// via `isOnFire` — level-independent, no RNG). A real `minecraft:post_attack`
// `ignite` action with a constant 100-second duration affecting the victim,
// resolved by the engine's ActionKind::Ignite and applied by the caller through
// EntitySystem::setOnFire.
inline constexpr std::string_view kFlameEffects = R"JSON({
  "minecraft:post_attack": [
    { "affected": "victim", "enchanted": "attacker",
      "effect": { "type": "minecraft:ignite",
        "duration": { "type": "minecraft:constant", "value": 100.0 } } }
  ]
})JSON";

// infinity: no value curve and no post_attack — a pure "a survival shot consumes
// no arrow" switch (InfinityEnchantment, read by BowItem#onStoppedUsing's arrow-
// consumption branch), level-independent, no RNG. An empty program (the same
// shape SilkTouch uses in ENCH-1b); infinityKeepsArrow() reads only the level, so
// this exists so infinity compiles cleanly through DDC-2 alongside its siblings
// and its unknown counters stay zero.
inline constexpr std::string_view kInfinityEffects = R"JSON({})JSON";

[[nodiscard]] inline std::string_view bowEffectJson(EnchantmentId id) {
    switch (id) {
    case EnchantmentId::Power: return kPowerEffects;
    case EnchantmentId::Punch: return kPunchEffects;
    case EnchantmentId::Flame: return kFlameEffects;
    case EnchantmentId::Infinity: return kInfinityEffects;
    default: return {};
    }
}

// The compiled program for each bow enchantment, in kBowEffectEnchantments order.
// Compiled once (load-time, off the shot hot path) through DDC-2's
// compileEffectsText and cached in the function-local static, so a per-shot query
// is a subscript, never a re-parse.
[[nodiscard]] inline const std::array<data::effect::EffectProgram, 4>& bowEffectPrograms() {
    static const std::array<data::effect::EffectProgram, 4> programs = [] {
        std::array<data::effect::EffectProgram, 4> built{};
        for (std::size_t index = 0; index < kBowEffectEnchantments.size(); ++index) {
            const EnchantmentId id = kBowEffectEnchantments[index];
            built[index] = data::effect::compileEffectsText(
                bowEffectJson(id), static_cast<std::int32_t>(enchantmentMaxLevel(id)));
        }
        return built;
    }();
    return programs;
}

inline constexpr std::size_t kPowerIndex = 0;
inline constexpr std::size_t kPunchIndex = 1;
inline constexpr std::size_t kFlameIndex = 2;

[[nodiscard]] inline const data::effect::EffectProgram& bowEffectProgram(std::size_t index) {
    return bowEffectPrograms()[index];
}

} // namespace detail

// ---------------------------------------------------------------------------
// Power — arrow damage multiplier (PowerEnchantment).
// ---------------------------------------------------------------------------

// The damage-multiplier FACTOR Power `level` contributes: `0.25*(level+1)`, or 0
// for level 0 (identity — a no-enchant shot never multiplies). Driven through
// DDC-2: applyDamageModifiers over the linear curve against a zero base. The
// caller folds it as `base * (1 + factor)` — see powerArrowBaseDamage() below.
[[nodiscard]] inline float powerDamageFactor(std::uint8_t level) {
    if (level == 0U) {
        return 0.0F;  // sabotage ①: no enchant ⇒ no factor, never level-0 math
    }
    data::effect::EffectContext ctx{};
    ctx.level = static_cast<std::int32_t>(level);
    ctx.baseAmount = 0.0F;
    return data::effect::applyDamageModifiers(detail::bowEffectProgram(detail::kPowerIndex), ctx);
}

// The arrow's base damage after Power scaling: `base * (1 + 0.25*(level+1))`,
// matching the task's `baseDamage*(1+0.25*(level+1))` form. With no Power this is
// `base` unchanged (identity). This is the value the bow bakes into the spawned
// projectile's base-damage field (AbstractArrow#onHitEntity still multiplies it by
// the live impact speed at the moment of the hit, RW-1a #8, so a Power arrow that
// bleeds off speed over a long arc still lands proportionally softer).
[[nodiscard]] inline float powerArrowBaseDamage(float baseDamage, std::uint8_t level) {
    return baseDamage * (1.0F + powerDamageFactor(level));
}

// Convenience over the firing bow stack.
[[nodiscard]] inline float powerArrowBaseDamage(float baseDamage, const ItemStack& bow) {
    return powerArrowBaseDamage(baseDamage, enchantmentLevel(bow, EnchantmentId::Power));
}

// ---------------------------------------------------------------------------
// Punch — extra knockback (PunchEnchantment).
// ---------------------------------------------------------------------------

// The extra knockback STRENGTH Punch `level` contributes: `0.5*level`, in the
// same unit EntitySystem::hurt(extraKnockbackStrength) uses for melee Knockback,
// or 0 for level 0 (identity — a no-enchant shot adds no shove). Driven through
// DDC-2: applyDamageModifiers over the linear curve against a zero base. The
// caller passes this straight into the hit's extraKnockbackStrength argument.
[[nodiscard]] inline float punchKnockbackStrength(std::uint8_t level) {
    if (level == 0U) {
        return 0.0F;
    }
    data::effect::EffectContext ctx{};
    ctx.level = static_cast<std::int32_t>(level);
    ctx.baseAmount = 0.0F;
    return data::effect::applyDamageModifiers(detail::bowEffectProgram(detail::kPunchIndex), ctx);
}

// Convenience over the firing bow stack.
[[nodiscard]] inline float punchKnockbackStrength(const ItemStack& bow) {
    return punchKnockbackStrength(enchantmentLevel(bow, EnchantmentId::Punch));
}

// ---------------------------------------------------------------------------
// Flame — ignite the target (FlameEnchantment).
// ---------------------------------------------------------------------------

// The seconds of burning a Flame bow's arrow inflicts on the entity it strikes,
// driven through DDC-2's post_attack ignite action: FlameEnchantment's flat
// 100-second setSecondsOnFire, level-independent, or 0 (no ignition) when the bow
// carries no Flame. The caller applies it via EntitySystem::setOnFire once the
// arrow's entity hit lands. No RNG — the ignite duration is a constant curve, so
// the same shot always burns for the same 100 seconds.
[[nodiscard]] inline int flameArrowIgniteSeconds(std::uint8_t level) {
    if (level == 0U) {
        return 0;  // identity — a no-Flame arrow never ignites
    }
    const data::effect::EffectProgram& program = detail::bowEffectProgram(detail::kFlameIndex);
    data::effect::EffectContext ctx{};
    ctx.level = static_cast<std::int32_t>(level);
    // No ctx.random: the ignite action carries no random_chance, so the outcome is
    // fully deterministic (a stray RNG draw here would break replay — sabotage ③).
    const data::effect::PostAttackResult result = data::effect::runPostAttack(program, ctx);
    int seconds = 0;
    for (const auto& outcome : result) {
        if (outcome.kind == data::effect::ActionKind::Ignite) {
            seconds = static_cast<int>(outcome.value);
        }
    }
    return seconds;
}

// Convenience over the firing bow stack.
[[nodiscard]] inline int flameArrowIgniteSeconds(const ItemStack& bow) {
    return flameArrowIgniteSeconds(enchantmentLevel(bow, EnchantmentId::Flame));
}

// ---------------------------------------------------------------------------
// Infinity — a survival shot consumes no arrow (InfinityEnchantment).
// ---------------------------------------------------------------------------

// Whether the firing bow's Infinity should let a survival player keep the arrow
// (BowItem#onStoppedUsing's `!bl2` / `!user.abilities.creativeMode` consumption
// branch skips the consume when the bow has Infinity and the arrow is an ordinary
// one). A pure switch: true iff the bow carries Infinity (level >= 1). Infinity
// and Mending are mutually exclusive (ENCH-0's isCompatibleWith already refuses
// the pair). Creative never consumes anyway; this only changes the SURVIVAL path.
// (sabotage ②: a build that still consumed the arrow would fail the "Infinity bow
// keeps its arrow" assertion.)
[[nodiscard]] inline bool infinityKeepsArrow(std::uint8_t level) {
    return level > 0U;
}

// Convenience over the firing bow stack.
[[nodiscard]] inline bool infinityKeepsArrow(const ItemStack& bow) {
    return hasEnchantment(bow, EnchantmentId::Infinity);
}

} // namespace mc::gameplay

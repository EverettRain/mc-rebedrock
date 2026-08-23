#pragma once

// ENCH-1: outgoing melee weapon enchantment effects — the first ENCH effect
// slice. ENCH-0 (Enchantment.hpp) gave every ItemStack an enchantment list and
// the 1.16.1 identity/cost registry; nothing read those levels to change
// gameplay until this file. Scope here is exactly the five *outgoing* melee
// weapon enchants a held weapon contributes at the attack site, transcribed
// from yarn-mapped 1.16.1 sources (DamageEnchantment.java, KnockbackEnchantment
// implicit via EnchantmentHelper#getKnockback, FireAspectEnchantment.java, and
// PlayerEntity#attack's own arithmetic gluing them together):
//
//   Sharpness      DamageEnchantment(typeIndex 0, EntityGroup.DEFAULT)
//   Smite          DamageEnchantment(typeIndex 1, EntityGroup.UNDEAD)
//   Bane of        DamageEnchantment(typeIndex 2, EntityGroup.ARTHROPOD)
//     Arthropods
//   Knockback      EnchantmentHelper.getKnockback -> PlayerEntity#attack's
//                  `i * 0.5F` extra takeKnockback call
//   Fire Aspect    EnchantmentHelper.getFireAspect -> PlayerEntity#attack's
//                  `target.setOnFireFor(k * 4)` once the hit lands
//
// INCOMING armor protection enchantments (Protection family, Thorns) are OUT
// OF SCOPE: this build has no wearable-armor-slot system yet (only the
// EnchantmentCategory::Armor* applicability tags in Enchantment.hpp, nothing
// equipped to read them from). The seam for a future armor-equip node is
// meleeDamageAfterArmor() at the bottom of this file — currently the identity
// function, documented at the call site in Damage.hpp's "armor / toughness"
// stage comment.
//
// Bane of Arthropods' bonus Slowness-on-hit (DamageEnchantment#onTargetDamaged)
// IS wired: EM2's Slowness effect exists, so a landed BoA hit lands Slowness IV
// for a level-scaled duration. The duration draws are deterministic (the
// target's own reproducible RNG stream), never the wall clock — see
// baneOfArthropodsSlownessTicks() and EntitySystem::applyBaneOfArthropodsSlowness.
//
// DEFERRED (not this node): Looting (loot-table integration, does the loot
// slice), Sweeping Edge (needs the sweep-attack mechanic), all mining-tool
// enchants (Efficiency/SilkTouch/Fortune/Unbreaking = ENCH-1b), bow/trident/
// crossbow enchants (need those weapons to exist).

#include "gameplay/Enchantment.hpp"
#include "gameplay/Inventory.hpp"

#include <cstdint>

namespace mc::gameplay {

// DamageEnchantment#getAttackDamage's typeIndex==0 branch:
// `1.0F + Math.max(0, level - 1) * 0.5F` == `0.5*level + 0.5` for level >= 1,
// and 0 for level 0 (no enchant). Re-expressed without the branch since this
// codebase's enchantmentLevel() already returns 0 for "absent".
[[nodiscard]] constexpr float sharpnessBonusDamage(std::uint8_t level) {
    if (level == 0U) {
        return 0.0F;
    }
    return 0.5F * static_cast<float>(level) + 0.5F;
}

// DamageEnchantment#getAttackDamage's typeIndex==1 branch, gated on the
// target being EntityGroup.UNDEAD: `level * 2.5F`, else 0.
[[nodiscard]] constexpr float smiteBonusDamage(std::uint8_t level, bool targetIsUndead) {
    if (level == 0U || !targetIsUndead) {
        return 0.0F;
    }
    return 2.5F * static_cast<float>(level);
}

// DamageEnchantment#getAttackDamage's typeIndex==2 branch, gated on the
// target being EntityGroup.ARTHROPOD: `level * 2.5F`, else 0. The accompanying
// Slowness-on-hit from DamageEnchantment#onTargetDamaged is applied separately
// (baneOfArthropodsSlownessTicks + EntitySystem::applyBaneOfArthropodsSlowness).
[[nodiscard]] constexpr float baneOfArthropodsBonusDamage(std::uint8_t level,
                                                          bool targetIsArthropod) {
    if (level == 0U || !targetIsArthropod) {
        return 0.0F;
    }
    return 2.5F * static_cast<float>(level);
}

// The three DamageEnchantment family members combined, mirroring
// canAccept()'s mutual exclusivity (Enchantment.hpp's isCompatibleWith
// already refuses to let two of these coexist on one stack, but a hand-edited
// or /give-forced stack could still carry more than one — summing every
// applicable contribution matches vanilla's forEachEnchantment loop, which
// does not special-case that).
[[nodiscard]] constexpr float meleeDamageEnchantBonus(const ItemStack& weapon,
                                                       bool targetIsUndead,
                                                       bool targetIsArthropod) {
    float bonus = sharpnessBonusDamage(enchantmentLevel(weapon, EnchantmentId::Sharpness));
    bonus += smiteBonusDamage(enchantmentLevel(weapon, EnchantmentId::Smite), targetIsUndead);
    bonus += baneOfArthropodsBonusDamage(
        enchantmentLevel(weapon, EnchantmentId::BaneOfArthropods), targetIsArthropod);
    return bonus;
}

// EnchantmentHelper.getKnockback -> PlayerEntity#attack's `i * 0.5F` extra
// takeKnockback call, on top of the base 0.4-strength hurt knockback every
// landed hit already applies. Re-expressed as a strength delta the caller
// adds to the existing single knockback application (this codebase applies
// knockback once per hit via EntitySystem::hurt's kKnockbackStrength, rather
// than vanilla's two separate takeKnockback calls that each halve the
// existing velocity) — additive in the strength that reaches pushBetween's
// scale, matching "knockback N increases applied knockback by N" in spirit
// while keeping the single-call shape. JC note: an exact vanilla replay would
// call takeKnockback twice; this is a documented simplification.
[[nodiscard]] constexpr float meleeKnockbackEnchantBonus(const ItemStack& weapon) {
    const std::uint8_t level = enchantmentLevel(weapon, EnchantmentId::Knockback);
    return 0.5F * static_cast<float>(level);
}

// EnchantmentHelper.getFireAspect -> PlayerEntity#attack's
// `target.setOnFireFor(k * 4)` once the hit lands: 4 seconds of burning per
// level, or 0 (no ignition) when the weapon carries no Fire Aspect.
[[nodiscard]] constexpr int meleeFireAspectSeconds(const ItemStack& weapon) {
    const std::uint8_t level = enchantmentLevel(weapon, EnchantmentId::FireAspect);
    return 4 * static_cast<int>(level);
}

// DamageEnchantment#onTargetDamaged (typeIndex==2): Bane of Arthropods lands
// Slowness on an arthropod for `20 + random.nextInt(10 * level)` ticks, at
// amplifier 3 (Slowness IV). This is the pure duration function; `randomDraw`
// is the caller-supplied value in [0, 10*level) drawn from a deterministic
// stream (the target's own RNG in EntitySystem::applyBaneOfArthropodsSlowness),
// so the formula itself carries no RNG and is exactly reproducible per draw.
// Returns 0 (no application) for level 0.
[[nodiscard]] constexpr int baneOfArthropodsSlownessTicks(std::uint8_t level, int randomDraw) {
    if (level == 0U) {
        return 0;
    }
    return 20 + randomDraw;
}

// The Slowness amplifier BoA applies: StatusEffects.SLOWNESS at amplifier 3
// (Slowness IV), a constant in vanilla independent of the enchant level.
inline constexpr std::uint8_t kBaneOfArthropodsSlownessAmplifier = 3U;

// The random bound BoA's Slowness duration draws against: `10 * level`, matching
// DamageEnchantment#onTargetDamaged's `random.nextInt(10 * level)`.
[[nodiscard]] constexpr int baneOfArthropodsSlownessRandomBound(std::uint8_t level) {
    return 10 * static_cast<int>(level);
}

// The armor-protection seam (Damage.hpp's "armor / toughness" stage): once an
// armor-equip system exists, this is where the defender's Protection family
// (Protection/FireProtection/BlastProtection/ProjectileProtection/
// FeatherFalling) and Thorns would read the defender's equipped armor stacks
// and reduce `amount` before it reaches applyDamage's health subtraction.
// Currently the identity function — nothing is equipped, so nothing to
// reduce — kept here rather than omitted so the seam has a name and a single
// call site to redirect when armor lands.
[[nodiscard]] constexpr float meleeDamageAfterArmorProtection(float amount) {
    return amount;
}

} // namespace mc::gameplay

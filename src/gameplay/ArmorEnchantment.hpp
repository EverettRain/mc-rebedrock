#pragma once

// EQ-4: armor enchantment effects — the Protection family EPF + Thorns, the
// top of the ENCH.gate armor half. ENCH-0 gave every ItemStack an enchantment
// list; EQ-1 put four armor slots on the player; EQ-2/EQ-3 filled the armor /
// resistance / absorption damage stages. Nothing read the *armor* enchantments
// to change a hit until this file.
//
// The requirement that makes this node different from ENCH-1's hardcoded melee
// bonuses: EQ-4 is the "真 datapack 化" backfill — the reduction is NOT a
// hand-written `damage *= 1 - epf/25` per enchantment. It is driven by the
// DDC-2 effect-component engine. The Protection-family `damage_protection`
// buckets and Thorns' `post_attack` are JE 26.1's own `effects` component JSON
// (the exact shape a data/minecraft/enchantment/*.json file carries), embedded
// below as the baked floor and compiled once at load through data::effect::
// compileEffectsText into the flat EffectProgram POD, then evaluated at runtime
// by data::effect::applyDamageProtection / runPostAttack — no gameplay-side
// branch on which enchantment it is, no reduction arithmetic outside the vanilla
// EPF fold.
//
// Why embedded here rather than the enchantment JSON's `effects` field: the
// shipped datapack files under resources/data/rebedrock/enchantment/ are held
// byte-identical to what the constexpr ENCH-0 table generates (the
// enchantment_migration golden test enforces it), and ENCH-0's table does not
// model `effects` — so the effect component is the baked floor here, exactly as
// Enchantment.hpp is the baked floor the generated files mirror. The evaluation
// path is identical either way (the same JSON → compileEffects → runtime eval),
// so when a future node teaches the migration bridge to emit `effects` and reads
// rawEffects off the DataStore, this baked floor is a drop-in and the datapack-
// overlay path just supplies the same strings from disk.
//
// Scope (EQ-DESIGN §2.5): Protection / Fire Protection / Blast Protection /
// Projectile Protection / Feather Falling (the five `damage_protection`
// enchantments) + Thorns (`post_attack`). Every other armor enchantment
// (Aqua Affinity / Respiration / Depth Strider / Frost Walker / Soul Speed /
// Curse of Binding) presses on a mechanic that does not exist yet and stays a
// registered-but-unimplemented gate (EQ-DESIGN §5), NOT a stub here.
//
// Determinism: Thorns' random_chance draw and its damage roll both come from
// the EffectContext.random the caller owns and seeds (a JavaRandom derived from
// a reproducible stream), never a wall clock — the RNG rule EQ-DESIGN §3 and
// REGULAR carry. The value path (EPF) is a table subscript + a few multiply-
// adds per worn piece; no allocation on the hot path.

#include "data/effect/EffectCompiler.hpp"
#include "data/effect/EffectRuntime.hpp"
#include "gameplay/DamageType.hpp"
#include "gameplay/Enchantment.hpp"
#include "gameplay/Equipment.hpp"
#include "gameplay/EquipmentSlot.hpp"
#include "world/gen/JavaRandom.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mc::gameplay {

// The six armor-slot enchantments EQ-4 drives through DDC-2. Kept as a small
// fixed list so the per-hit sum walks exactly these, never the whole registry.
inline constexpr std::array<EnchantmentId, 6> kArmorEffectEnchantments{
    EnchantmentId::Protection,       EnchantmentId::FireProtection,
    EnchantmentId::BlastProtection,  EnchantmentId::ProjectileProtection,
    EnchantmentId::FeatherFalling,   EnchantmentId::Thorns,
};

namespace detail {

// The baked-floor `effects` JSON for each armor enchantment — JE 26.1's own
// component shape for each, so a no-datapack build reduces exactly as a vanilla
// data pack would and a future datapack overlay (reading rawEffects off the
// DataStore) is a drop-in for these strings. Compiled through DDC-2, never
// interpreted by hand.
//
// protection: +1 reduction per level on any source that does not bypass the
// invulnerability window (i.e. every survivable hit) — the general shield.
inline constexpr std::string_view kProtectionEffects = R"JSON({
  "minecraft:damage_protection": [
    { "effect": { "type": "minecraft:add",
        "value": { "type": "minecraft:linear", "base": 1.0, "per_level_above_first": 1.0 } },
      "requirements": { "condition": "minecraft:damage_source_properties",
        "predicate": { "tags": [ { "id": "minecraft:bypasses_invulnerability", "expected": false } ] } } }
  ]
})JSON";

// fire_protection: +2 per level, gated on IsFire (and not bypassing).
inline constexpr std::string_view kFireProtectionEffects = R"JSON({
  "minecraft:damage_protection": [
    { "effect": { "type": "minecraft:add",
        "value": { "type": "minecraft:linear", "base": 2.0, "per_level_above_first": 2.0 } },
      "requirements": { "condition": "minecraft:damage_source_properties",
        "predicate": { "tags": [ { "id": "minecraft:is_fire", "expected": true },
                                 { "id": "minecraft:bypasses_invulnerability", "expected": false } ] } } }
  ]
})JSON";

// blast_protection: +2 per level, gated on IsExplosion.
inline constexpr std::string_view kBlastProtectionEffects = R"JSON({
  "minecraft:damage_protection": [
    { "effect": { "type": "minecraft:add",
        "value": { "type": "minecraft:linear", "base": 2.0, "per_level_above_first": 2.0 } },
      "requirements": { "condition": "minecraft:damage_source_properties",
        "predicate": { "tags": [ { "id": "minecraft:is_explosion", "expected": true },
                                 { "id": "minecraft:bypasses_invulnerability", "expected": false } ] } } }
  ]
})JSON";

// projectile_protection: +2 per level, gated on IsProjectile.
inline constexpr std::string_view kProjectileProtectionEffects = R"JSON({
  "minecraft:damage_protection": [
    { "effect": { "type": "minecraft:add",
        "value": { "type": "minecraft:linear", "base": 2.0, "per_level_above_first": 2.0 } },
      "requirements": { "condition": "minecraft:damage_source_properties",
        "predicate": { "tags": [ { "id": "minecraft:is_projectile", "expected": true },
                                 { "id": "minecraft:bypasses_invulnerability", "expected": false } ] } } }
  ]
})JSON";

// feather_falling: +3 per level, gated on IsFall.
inline constexpr std::string_view kFeatherFallingEffects = R"JSON({
  "minecraft:damage_protection": [
    { "effect": { "type": "minecraft:add",
        "value": { "type": "minecraft:linear", "base": 3.0, "per_level_above_first": 3.0 } },
      "requirements": { "condition": "minecraft:damage_source_properties",
        "predicate": { "tags": [ { "id": "minecraft:is_fall", "expected": true },
                                 { "id": "minecraft:bypasses_invulnerability", "expected": false } ] } } }
  ]
})JSON";

// thorns: a random_chance (0.15*level) gated all_of that reflects 1..5 thorns
// damage onto the attacker and spends 2 durability off the victim's armor.
inline constexpr std::string_view kThornsEffects = R"JSON({
  "minecraft:post_attack": [
    { "affected": "attacker", "enchanted": "victim",
      "effect": { "type": "minecraft:all_of", "effects": [
        { "type": "minecraft:damage_entity", "damage_type": "minecraft:thorns",
          "min_damage": 1.0, "max_damage": 5.0 },
        { "type": "minecraft:change_item_damage", "amount": 2.0 } ] },
      "requirements": { "condition": "minecraft:random_chance",
        "chance": { "type": "minecraft:enchantment_level",
          "amount": { "type": "minecraft:linear", "base": 0.15, "per_level_above_first": 0.15 } } } }
  ]
})JSON";

[[nodiscard]] inline std::string_view armorEffectJson(EnchantmentId id) {
    switch (id) {
    case EnchantmentId::Protection: return kProtectionEffects;
    case EnchantmentId::FireProtection: return kFireProtectionEffects;
    case EnchantmentId::BlastProtection: return kBlastProtectionEffects;
    case EnchantmentId::ProjectileProtection: return kProjectileProtectionEffects;
    case EnchantmentId::FeatherFalling: return kFeatherFallingEffects;
    case EnchantmentId::Thorns: return kThornsEffects;
    default: return {};
    }
}

// The compiled program for each armor enchantment, in the order of
// kArmorEffectEnchantments. Compiled once (load-time, off any hot path) through
// DDC-2's compileEffectsText and cached in the function-local static, so per-hit
// evaluation is a subscript, never a re-parse.
[[nodiscard]] inline const std::array<data::effect::EffectProgram, 6>& armorEffectPrograms() {
    static const std::array<data::effect::EffectProgram, 6> programs = [] {
        std::array<data::effect::EffectProgram, 6> built{};
        for (std::size_t index = 0; index < kArmorEffectEnchantments.size(); ++index) {
            const EnchantmentId id = kArmorEffectEnchantments[index];
            built[index] = data::effect::compileEffectsText(
                armorEffectJson(id), static_cast<std::int32_t>(enchantmentMaxLevel(id)));
        }
        return built;
    }();
    return programs;
}

[[nodiscard]] inline const data::effect::EffectProgram& armorEffectProgram(std::size_t index) {
    return armorEffectPrograms()[index];
}

} // namespace detail

// Maps a DamageType to the `minecraft:` source-tag predicate the DDC-2 programs
// query (EffectContext.sourceHasTag). Only the tags the Protection family and
// Thorns reference are answered; anything else is false, which is the correct
// default (an absent tag never gates a predicate true). This is the gameplay-
// side "which live tags does this hit carry" the pure engine asks for — the
// same "caller gathers the numbers, the engine transforms them" split EQ-2's
// armor field and EQ-3's resistanceLevel use.
[[nodiscard]] inline bool damageSourceHasTag(DamageType type, std::string_view tag) {
    if (tag == "minecraft:bypasses_invulnerability") {
        return hasDamageTag(type, DamageTag::BypassesInvulnerability);
    }
    if (tag == "minecraft:is_fire") {
        return hasDamageTag(type, DamageTag::IsFire);
    }
    if (tag == "minecraft:is_explosion") {
        return hasDamageTag(type, DamageTag::IsExplosion);
    }
    if (tag == "minecraft:is_projectile") {
        return hasDamageTag(type, DamageTag::IsProjectile);
    }
    if (tag == "minecraft:is_fall") {
        return hasDamageTag(type, DamageTag::IsFall);
    }
    return false;
}

// EnchantmentHelper#getProtectionAmount, expressed through DDC-2: sums each worn
// armor piece's `damage_protection` contribution for this damage type. For every
// armor slot, for each Protection-family enchantment the piece carries, the
// compiled program's applyDamageProtection is evaluated at that piece's level
// with a context whose sourceHasTag answers for `type` — so Fire Protection only
// adds on an IsFire hit, Feather Falling only on a fall, etc. The result is the
// raw (unclamped) EPF total; the clamp to 20 happens in the fold below, matching
// vanilla's DamageUtil.getInflictedDamage.
[[nodiscard]] inline float enchantmentProtectionFactor(const EquipmentSlots& equipment,
                                                       DamageType type) {
    float total = 0.0F;
    for (const EquipmentSlot slot : kArmorSlots) {
        const ItemStack& piece = equipment.equippedArmor(slot);
        if (piece.empty()) {
            continue;
        }
        for (std::size_t index = 0; index < kArmorEffectEnchantments.size(); ++index) {
            const EnchantmentId id = kArmorEffectEnchantments[index];
            if (id == EnchantmentId::Thorns) {
                continue;  // Thorns is post_attack, not a damage_protection bucket.
            }
            const std::uint8_t level = enchantmentLevel(piece, id);
            if (level == 0U) {
                continue;
            }
            data::effect::EffectContext ctx{};
            ctx.level = static_cast<std::int32_t>(level);
            ctx.baseAmount = 0.0F;  // the pure protection value, not scaled by damage
            ctx.sourceHasTag = [type](std::string_view tag) {
                return damageSourceHasTag(type, tag);
            };
            total += data::effect::applyDamageProtection(detail::armorEffectProgram(index), ctx);
        }
    }
    return total;
}

// The EPF clamp-and-fold (DamageUtil.getInflictedDamage) lives in Damage.hpp's
// damageAfterEnchantmentProtection and runs inside the pipeline's effects stage;
// enchantmentProtectionFactor above only produces the raw summed EPF the
// pipeline folds. Kept separate so the pipeline owns the arithmetic and this
// header owns only the DDC-2 evaluation.

// One Thorns reflection the victim's armor produced: how much damage to deal
// back to the attacker and how much durability to spend on the enchanted piece.
// A POD the caller applies — the mutation (dealing damage, spending durability)
// stays with the gameplay caller, exactly as DDC-2's PostAttackOutcome contract
// intends (the engine owns the decision, gameplay owns the effect).
struct ThornsReflection final {
    bool fired = false;         // the random_chance draw succeeded on some piece
    float attackerDamage = 0.0F; // total damage to reflect onto the attacker
    EquipmentSlot slot = EquipmentSlot::Chest;  // the piece that fired (durability cost)
    float itemDamage = 0.0F;    // durability points to spend on that piece
};

// ThornsEnchantment#onUserDamaged, driven through DDC-2's runPostAttack: walks
// the worn armor, and for the first piece whose Thorns program fires its
// random_chance (0.15*level) reflects the rolled thorns damage back onto the
// attacker and reports the durability to spend. Vanilla only ever fires one
// piece per hit (it picks a random equipped Thorns piece; here the first worn
// Thorns piece in the fixed armor-slot order is used, and the chance/roll are
// the deterministic draws off `random`, so the sequence is reproducible for a
// given seed). Every draw comes from `random`, never a wall clock.
[[nodiscard]] inline ThornsReflection resolveThorns(const EquipmentSlots& equipment,
                                                    world::gen::JavaRandom& random) {
    ThornsReflection reflection{};
    // The compiled Thorns program is the last entry in kArmorEffectEnchantments.
    constexpr std::size_t kThornsIndex = 5;
    static_assert(kArmorEffectEnchantments[kThornsIndex] == EnchantmentId::Thorns,
                  "Thorns must be the last armor effect program");
    const data::effect::EffectProgram& program = detail::armorEffectProgram(kThornsIndex);
    for (const EquipmentSlot slot : kArmorSlots) {
        const ItemStack& piece = equipment.equippedArmor(slot);
        if (piece.empty()) {
            continue;
        }
        const std::uint8_t level = enchantmentLevel(piece, EnchantmentId::Thorns);
        if (level == 0U) {
            continue;
        }
        data::effect::EffectContext ctx{};
        ctx.level = static_cast<std::int32_t>(level);
        ctx.random = &random;
        const data::effect::PostAttackResult result =
            data::effect::runPostAttack(program, ctx);
        if (result.count == 0) {
            continue;  // the random_chance draw missed on this piece
        }
        reflection.fired = true;
        reflection.slot = slot;
        for (const auto& outcome : result) {
            if (outcome.kind == data::effect::ActionKind::DamageEntity) {
                reflection.attackerDamage += outcome.value;
            } else if (outcome.kind == data::effect::ActionKind::ChangeItemDamage) {
                reflection.itemDamage += outcome.value;
            }
        }
        break;  // vanilla reflects one piece per hit
    }
    return reflection;
}

} // namespace mc::gameplay

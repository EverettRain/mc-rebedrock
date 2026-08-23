#pragma once

#include "gameplay/Difficulty.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace mc::gameplay {

// The damage types this game can deal. 26.1 keeps these in a data-driven
// registry (`world/damagesource/DamageTypes.java`, ~50 entries); this is the
// subset that exists here, with the vanilla message ids so the mapping is not
// guesswork. Adding one is a row in the table below.
enum class DamageType : std::uint8_t {
    None,          // no damage this tick / an applyDamage() guard
    Generic,       // untyped damage (command fallback, unclassified)
    EntityAttack,  // vanilla `mob_attack`: a melee hit from another creature
    // RW-0: vanilla `arrow`/`trident` family — a projectile's hit, routed
    // through the same applyDamage() pipeline as a melee hit so a future
    // armored target (EQ-2) automatically takes less arrow damage without RW
    // needing its own reduction logic (RW-DESIGN.md §5).
    Projectile,
    Fall,
    Drown,
    Starve,
    OutOfWorld,    // vanilla `fell_out_of_world`: the void and /kill
    // The three fire families, split the way vanilla does. `OnFire` is the burn
    // tick a creature that caught fire takes every second; `InFire` is standing
    // in a fire block; `Lava` is contact with lava. They share the IsFire tag so
    // fire-immunity/resistance answer them all with one test.
    OnFire,        // vanilla `on_fire`: the per-second burn while ablaze
    InFire,        // vanilla `in_fire`: standing inside a fire block
    Lava,          // vanilla `lava`: touching lava
    Count,
};

inline constexpr std::size_t kDamageTypeCount = static_cast<std::size_t>(DamageType::Count);

// DamageScaling: whether difficulty scales this type at all, and under what
// condition. Vanilla's default for almost everything is
// WHEN_CAUSED_BY_LIVING_NON_PLAYER, which is why falling never scales — the
// world is not a living attacker.
enum class DamageScaling : std::uint8_t {
    Never,
    WhenCausedByLivingNonPlayer,
    Always,
};

// The boolean behaviours of a damage type. 26.1 has 35 of these
// (`tags/DamageTypeTags.java`) and reaches for a tag rather than a field
// whenever the answer is yes/no.
//
// The ones about content this game does not have — armor stands, ender
// dragons, wolf armor, witches, withers, guardians, silverfish, the mace — are
// deliberately absent rather than copied in and left unread: a tag nothing
// consults is a claim the code cannot keep. The mask is 64 bits wide, so each
// of them costs one line on the day it acquires a consumer.
enum class DamageTag : std::uint8_t {
    BypassesArmor,
    BypassesShield,
    // The type ignores the post-hit invulnerability window entirely, which is
    // how /kill and the void kill outright instead of being swallowed by a hit
    // that landed a moment earlier.
    BypassesInvulnerability,
    BypassesCooldown,
    BypassesEffects,
    BypassesResistance,
    IsFall,
    IsDrowning,
    IsFire,
    IsExplosion,
    IsProjectile,
    // The hit does not shove the victim.
    NoKnockback,
    // The hit does not make a neutral mob angry at its source.
    NoAnger,
    Count,
};

[[nodiscard]] constexpr std::uint64_t damageTagBit(DamageTag tag) {
    return std::uint64_t{1} << static_cast<std::uint64_t>(tag);
}

// A tag mask built at compile time, the same uint64 shape BlockTags uses: a tag
// test is one bit test rather than a switch or a set lookup.
template <typename... Tags>
[[nodiscard]] constexpr std::uint64_t damageTags(Tags... tags) {
    return (damageTagBit(tags) | ... | std::uint64_t{0});
}

// 26.1's DamageType record: `record DamageType(String msgId, DamageScaling
// scaling, float exhaustion, DamageEffects effects, DeathMessageType
// deathMessageType)`, plus the tag mask that in vanilla lives beside it in the
// tag registry. `effects` and `deathMessageType` are not here yet — the first
// picks a hurt sound variant (this game plays one hurt sound) and the second
// picks a death message (there are no death messages) — so they would be two
// fields nothing reads.
struct DamageTypeData final {
    std::string_view msgId;
    DamageScaling scaling = DamageScaling::WhenCausedByLivingNonPlayer;
    // How much hunger the hit costs the player. This is the field that used to
    // be one hard-coded 0.1 applied to every source alike.
    float exhaustion = 0.0F;
    std::uint64_t tags = 0U;
};

// The table, with vanilla's values (`DamageTypes.java:61-95`, tag membership
// from `DamageTypeTagsProvider.java`).
inline constexpr std::array<DamageTypeData, kDamageTypeCount> kDamageTypes{{
    // None is the "nothing happened" sentinel and never reaches the pipeline.
    {"none", DamageScaling::Never, 0.0F, 0U},
    {"generic", DamageScaling::WhenCausedByLivingNonPlayer, 0.0F,
     damageTags(DamageTag::BypassesArmor)},
    // `new DamageType("mob", 0.1F)` — the only type here that costs hunger.
    {"mob", DamageScaling::WhenCausedByLivingNonPlayer, 0.1F, 0U},
    // `new DamageType("arrow", 0.1F)` (`DamageTypes.ARROW`): a projectile hit
    // costs the same hunger as a melee swing and is reduced by armor like any
    // other physical hit (no BypassesArmor), unlike the world hazards below.
    {"arrow", DamageScaling::WhenCausedByLivingNonPlayer, 0.1F,
     damageTags(DamageTag::IsProjectile)},
    // `new DamageType("fall", …, 0.0F, …)`. Falling costs no hunger: it is the
    // landing that hurts, not an effort the player made.
    {"fall", DamageScaling::WhenCausedByLivingNonPlayer, 0.0F,
     damageTags(DamageTag::BypassesArmor, DamageTag::IsFall)},
    // `new DamageType("drown", 0.0F, DamageEffects.DROWNING)`.
    {"drown", DamageScaling::WhenCausedByLivingNonPlayer, 0.0F,
     damageTags(DamageTag::BypassesArmor, DamageTag::IsDrowning)},
    // `new DamageType("starve", 0.0F)` — and BYPASSES_EFFECTS. Starving costing
    // hunger would have made starvation feed itself.
    {"starve", DamageScaling::WhenCausedByLivingNonPlayer, 0.0F,
     damageTags(DamageTag::BypassesArmor, DamageTag::BypassesEffects)},
    // `new DamageType("outOfWorld", 0.0F)` + BYPASSES_INVULNERABILITY +
    // BYPASSES_RESISTANCE: the void and /kill are not survivable.
    {"outOfWorld", DamageScaling::WhenCausedByLivingNonPlayer, 0.0F,
     damageTags(DamageTag::BypassesArmor, DamageTag::BypassesInvulnerability,
                DamageTag::BypassesResistance, DamageTag::NoKnockback)},
    // The three fire types. `new DamageType("onFire", 0.0F)` /
    // `new DamageType("inFire", 0.0F)` / `new DamageType("lava", 0.1F)` in
    // vanilla; all carry IS_FIRE and none bypass armor (fire is reduced by
    // armor, unlike the world's other damage). Only lava costs the 0.1 hunger a
    // living attacker's swing does; the passive burn and standing in fire do
    // not. NO_KNOCKBACK: the burn tick and standing in fire do not shove — only
    // an attacker's swing pushes a burning victim, which is a different source.
    {"onFire", DamageScaling::WhenCausedByLivingNonPlayer, 0.0F,
     damageTags(DamageTag::IsFire, DamageTag::NoKnockback)},
    {"inFire", DamageScaling::WhenCausedByLivingNonPlayer, 0.0F,
     damageTags(DamageTag::IsFire)},
    {"lava", DamageScaling::WhenCausedByLivingNonPlayer, 0.1F,
     damageTags(DamageTag::IsFire)},
}};

[[nodiscard]] constexpr const DamageTypeData& damageTypeData(DamageType type) {
    return kDamageTypes[static_cast<std::size_t>(type)];
}

[[nodiscard]] constexpr bool hasDamageTag(DamageType type, DamageTag tag) {
    return (damageTypeData(type).tags & damageTagBit(tag)) != 0U;
}

// LivingEntity#getDamageAfterMagicAbsorb's difficulty step, driven by the
// type's own scaling rather than by the call site remembering to apply it.
// `causedByLivingNonPlayer` is the condition vanilla's default scaling names:
// a mob swung it, so a harder world swings harder. The world itself — falling,
// drowning, starving — is never a living attacker, so those never scale even
// though they carry the same scaling value.
[[nodiscard]] constexpr float scaleDamageForDifficulty(
    DamageType type,
    float amount,
    Difficulty difficulty,
    bool causedByLivingNonPlayer) {
    switch (damageTypeData(type).scaling) {
    case DamageScaling::Never:
        return amount;
    case DamageScaling::WhenCausedByLivingNonPlayer:
        if (!causedByLivingNonPlayer) {
            return amount;
        }
        break;
    case DamageScaling::Always:
        break;
    }
    return scaledDamage(difficulty, amount);
}

} // namespace mc::gameplay

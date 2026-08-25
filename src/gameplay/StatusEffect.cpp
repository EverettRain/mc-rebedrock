#include "gameplay/StatusEffect.hpp"

#include "core/Identifier.hpp"
#include "core/Registry.hpp"

#include <algorithm>

namespace mc::gameplay {

// The registry wrapper around the generic core::Registry, matching the shape the
// entity-type registry uses: a dense table of definitions plus the name -> id
// interner, filed under `rebedrock:` with a `minecraft:` alias.
class StatusEffectRegistry final {
  public:
    core::StatusEffectId registerBuiltin(std::string_view path, const StatusEffectDef& def) {
        const core::Identifier id{core::kNamespace, path};
        const core::StatusEffectId assigned = store_.registerBuiltin(id, def);
        // The `minecraft:` name resolves to the same id so 1.16.1 potion/effect
        // data and translation keys still map.
        store_.alias(core::Identifier{core::kVanillaNamespace, path}, assigned);
        return assigned;
    }

    void freeze() { store_.freeze(); }

    [[nodiscard]] core::StatusEffectId byName(std::string_view name) const {
        return store_.byName(name);
    }
    [[nodiscard]] const StatusEffectDef& get(core::StatusEffectId id) const {
        return store_.get(id);
    }
    [[nodiscard]] const core::Identifier& identifier(core::StatusEffectId id) const {
        return store_.identifier(id);
    }
    [[nodiscard]] std::size_t size() const { return store_.size(); }

  private:
    core::Registry<StatusEffectDef, core::StatusEffectId> store_;
};

namespace {

// The five built-in ids, resolved once when the registry is populated. Content
// nodes hold these rather than doing a name lookup per use.
struct BuiltinEffectIds final {
    core::StatusEffectId poison{};
    core::StatusEffectId regeneration{};
    core::StatusEffectId hunger{};
    core::StatusEffectId speed{};
    core::StatusEffectId slowness{};
    // EQ-3: the two defensive effects. Registered after the original five so
    // their dense ids append rather than renumber the existing ones.
    core::StatusEffectId resistance{};
    core::StatusEffectId fireResistance{};
};

BuiltinEffectIds gBuiltinIds;

// Registers the five built-ins in a fixed order (so their dense ids are stable)
// and freezes the registry. Vanilla numbers: colours off MobEffects.java,
// intervals off Poison/RegenerationMobEffect, speed/slowness factors off the
// MOVEMENT_SPEED modifiers (+0.2 / -0.15, ADD_MULTIPLIED_TOTAL).
StatusEffectRegistry buildRegistry() {
    StatusEffectRegistry registry;
    gBuiltinIds.speed = registry.registerBuiltin(
        "speed", {StatusEffectCategory::Beneficial, 0x3389FFU, EffectKind::None,
                  /*speedModifierPerLevel=*/0.2F});
    gBuiltinIds.slowness = registry.registerBuiltin(
        "slowness", {StatusEffectCategory::Harmful, 0x8BAFE0U, EffectKind::None,
                     /*speedModifierPerLevel=*/-0.15F});
    gBuiltinIds.regeneration = registry.registerBuiltin(
        "regeneration", {StatusEffectCategory::Beneficial, 0xCD5CABU, EffectKind::Regeneration});
    gBuiltinIds.poison = registry.registerBuiltin(
        "poison", {StatusEffectCategory::Harmful, 0x87A363U, EffectKind::Poison});
    gBuiltinIds.hunger = registry.registerBuiltin(
        "hunger", {StatusEffectCategory::Harmful, 0x587653U, EffectKind::Hunger});
    // EQ-3: Resistance and Fire Resistance carry no per-tick behaviour
    // (EffectKind::None) — the damage pipeline consults them directly through
    // resistanceLevel()/isFireImmune(). Colours off MobEffects.java
    // (RESISTANCE 0x99453A, FIRE_RESISTANCE 0xE49A3A), both Beneficial.
    gBuiltinIds.resistance = registry.registerBuiltin(
        "resistance", {StatusEffectCategory::Beneficial, 0x99453AU, EffectKind::None});
    gBuiltinIds.fireResistance = registry.registerBuiltin(
        "fire_resistance", {StatusEffectCategory::Beneficial, 0xE49A3AU, EffectKind::None});
    registry.freeze();
    return registry;
}

} // namespace

StatusEffectRegistry& statusEffectRegistry() {
    // Populated and frozen on first access. The build order above fixes the ids;
    // once frozen, any further registration aborts, which is the R0 guard the
    // fork tests exercise.
    static StatusEffectRegistry registry = buildRegistry();
    return registry;
}

core::StatusEffectId statusEffectByName(std::string_view name) {
    return statusEffectRegistry().byName(name);
}

const StatusEffectDef& statusEffectDef(core::StatusEffectId id) {
    return statusEffectRegistry().get(id);
}

std::string_view statusEffectName(core::StatusEffectId id) {
    if (!id.valid()) {
        return {};
    }
    // The canonical `rebedrock:` path — the stable name a save stores instead of
    // the per-run id.
    return statusEffectRegistry().identifier(id).path;
}

core::StatusEffectId poisonEffect() {
    static_cast<void>(statusEffectRegistry());
    return gBuiltinIds.poison;
}
core::StatusEffectId regenerationEffect() {
    static_cast<void>(statusEffectRegistry());
    return gBuiltinIds.regeneration;
}
core::StatusEffectId hungerEffect() {
    static_cast<void>(statusEffectRegistry());
    return gBuiltinIds.hunger;
}
core::StatusEffectId speedEffect() {
    static_cast<void>(statusEffectRegistry());
    return gBuiltinIds.speed;
}
core::StatusEffectId slownessEffect() {
    static_cast<void>(statusEffectRegistry());
    return gBuiltinIds.slowness;
}
core::StatusEffectId resistanceEffect() {
    static_cast<void>(statusEffectRegistry());
    return gBuiltinIds.resistance;
}
core::StatusEffectId fireResistanceEffect() {
    static_cast<void>(statusEffectRegistry());
    return gBuiltinIds.fireResistance;
}

int effectTickInterval(EffectKind kind, std::uint8_t amplifier) {
    switch (kind) {
    case EffectKind::Poison: {
        // PoisonMobEffect: `25 >> amplification`, clamped to at least 1 so a very
        // high amplifier fires every tick rather than dividing by zero.
        const int interval = 25 >> amplifier;
        return interval > 0 ? interval : 1;
    }
    case EffectKind::Regeneration: {
        const int interval = 50 >> amplifier;
        return interval > 0 ? interval : 1;
    }
    case EffectKind::Hunger:
    case EffectKind::None:
        return 1;
    }
    return 1;
}

namespace {

// Finds the slot holding `id`, or count when absent.
std::size_t indexOf(const ActiveEffects& effects, core::StatusEffectId id) {
    for (std::size_t index = 0; index < effects.count; ++index) {
        if (effects.entries[index].id == id) {
            return index;
        }
    }
    return effects.count;
}

} // namespace

bool applyEffect(ActiveEffects& effects, core::StatusEffectId id, std::int32_t durationTicks,
                 std::uint8_t amplifier) {
    if (!id.valid() || durationTicks <= 0) {
        return false;
    }
    const std::size_t existing = indexOf(effects, id);
    if (existing < effects.count) {
        // MobEffectInstance's merge: a stronger effect always wins; an
        // equal-strength one wins only if it lasts at least as long. A weaker or
        // shorter re-application is ignored, so a lingering strong poison is not
        // downgraded by a weak splash.
        EffectInstance& current = effects.entries[existing];
        const bool stronger = amplifier > current.amplifier;
        const bool sameButLonger =
            amplifier == current.amplifier && durationTicks >= current.durationTicks;
        if (stronger || sameButLonger) {
            current.durationTicks = durationTicks;
            current.amplifier = amplifier;
            return true;
        }
        return false;
    }
    if (effects.count >= ActiveEffects::kMaxEffects) {
        // No free slot. The built-ins never reach this, so rather than evict a
        // possibly-important effect the new one is dropped.
        return false;
    }
    effects.entries[effects.count] = EffectInstance{id, durationTicks, amplifier};
    ++effects.count;
    return true;
}

bool removeEffect(ActiveEffects& effects, core::StatusEffectId id) {
    const std::size_t index = indexOf(effects, id);
    if (index >= effects.count) {
        return false;
    }
    // Compact: move the last live entry into the hole so the array stays dense.
    effects.entries[index] = effects.entries[effects.count - 1U];
    effects.entries[effects.count - 1U] = EffectInstance{};
    --effects.count;
    return true;
}

std::size_t clearEffects(ActiveEffects& effects) {
    const std::size_t cleared = effects.count;
    effects.entries = {};
    effects.count = 0U;
    return cleared;
}

bool hasEffect(const ActiveEffects& effects, core::StatusEffectId id) {
    return indexOf(effects, id) < effects.count;
}

const EffectInstance* getEffect(const ActiveEffects& effects, core::StatusEffectId id) {
    const std::size_t index = indexOf(effects, id);
    return index < effects.count ? &effects.entries[index] : nullptr;
}

std::uint8_t resistanceLevel(const ActiveEffects& effects) {
    const EffectInstance* const instance = getEffect(effects, resistanceEffect());
    if (instance == nullptr) {
        return 0U;
    }
    // Vanilla's `getAmplifier() + 1`: Resistance II (amplifier 1) is level 2.
    return static_cast<std::uint8_t>(instance->amplifier + 1U);
}

bool isFireImmune(const ActiveEffects& effects) {
    return hasEffect(effects, fireResistanceEffect());
}

EffectTickOutcome tickEffects(ActiveEffects& effects, float currentHealth) {
    EffectTickOutcome outcome;
    if (effects.count == 0U) {
        // The common case: an entity with no effects does no work beyond this.
        return outcome;
    }
    // A running health estimate so several damaging effects in one tick each
    // respect poison's "never below one health" floor against the same value the
    // caller will see, not the original.
    float healthEstimate = currentHealth;

    for (std::size_t index = 0; index < effects.count;) {
        EffectInstance& instance = effects.entries[index];
        const StatusEffectDef& def = statusEffectDef(instance.id);

        // The interval effects fire when `durationTicks % interval == 0`, using
        // the remaining duration as the tick counter (MobEffectInstance.tick's
        // `tickCount = this.duration`).
        const int interval = effectTickInterval(def.kind, instance.amplifier);
        const bool fires = interval <= 1 || (instance.durationTicks % interval) == 0;

        if (fires) {
            switch (def.kind) {
            case EffectKind::Poison:
                // Poison never kills: it only bites while health is above one.
                if (healthEstimate > 1.0F) {
                    outcome.damage += 1.0F;
                    outcome.damageType = DamageType::Generic;
                    healthEstimate -= 1.0F;
                }
                break;
            case EffectKind::Regeneration:
                outcome.heal += 1.0F;
                break;
            case EffectKind::Hunger:
                outcome.exhaustion += 0.005F * static_cast<float>(instance.amplifier + 1U);
                break;
            case EffectKind::None:
                break;
            }
        }

        // The movement modifier is continuous, not interval-gated: it applies
        // every tick the effect is present, and vanishes the tick it expires.
        if (def.speedModifierPerLevel != 0.0F) {
            outcome.speedMultiplier *=
                1.0F + def.speedModifierPerLevel * static_cast<float>(instance.amplifier + 1U);
        }

        // Count the tick down; drop the effect when it runs out.
        --instance.durationTicks;
        if (instance.durationTicks <= 0) {
            instance = effects.entries[effects.count - 1U];
            effects.entries[effects.count - 1U] = EffectInstance{};
            --effects.count;
            continue;  // re-examine the entry now sitting in this slot
        }
        ++index;
    }
    return outcome;
}

} // namespace mc::gameplay

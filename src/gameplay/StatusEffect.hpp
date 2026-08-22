#pragma once

// EM-2: the status-effect (MobEffect) system.
//
// 26.1 keeps a `Map<Holder<MobEffect>, MobEffectInstance>` on every
// LivingEntity — a heap map, boxed instances, and a virtual `applyEffectTick`
// per effect class. This is the DOD form of the same idea:
//
//   * The effect *definitions* live in an R0-style dense registry keyed by a
//     `core::StatusEffectId` (deref = one array subscript, holder = id).
//   * The per-tick behaviour is *data*, not a virtual method: a small closed
//     `EffectKind` enum the tick switches on once. The effect kinds are few and
//     fixed (poison, regeneration, hunger, a movement modifier), exactly the
//     case a switch beats a v-table.
//   * A creature's *active* effects are a small fixed inline array, not a map:
//     almost every entity carries none, so the storage is zero heap and zero
//     allocation, and an entity with no effects pays nothing. (Scheme A of the
//     card; scheme B — a sparse map keyed by entity id — was rejected because
//     the effect count is naturally tiny and a per-entity array is cache-local.)
//
// The tick produces an `EffectTickOutcome` — the damage/heal/exhaustion/speed a
// tick wants applied — and hands it back to the caller, so the identical logic
// drives a mob (through EntitySystem's DamageState) and the player (through
// PlayerVitals) with no duplication and no knowledge of either here.

#include "core/ContentId.hpp"
#include "gameplay/DamageType.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace mc::gameplay {

// MobEffectCategory: whether the effect helps or harms. Milk/other cleanses
// (AR-A3) will clear by category; the tint a HUD would show follows it too.
enum class StatusEffectCategory : std::uint8_t {
    Beneficial,
    Harmful,
    Neutral,
};

// The per-tick behaviour of an effect, as a closed enum the tick resolves once.
// A new kind is a row here plus a case in the tick — the same "add content = a
// row, not a v-table" shape the block/damage tables use.
enum class EffectKind : std::uint8_t {
    // Does nothing on its own; only its attribute modifier (if any) matters.
    // Speed and slowness are this kind with a movement-speed factor.
    None,
    // PoisonMobEffect: one point of magic damage every `25 >> amplifier` ticks,
    // but never below one health — poison does not kill.
    Poison,
    // RegenerationMobEffect: heals one point every `50 >> amplifier` ticks.
    Regeneration,
    // HungerMobEffect: drains `0.005 * (amplifier + 1)` food exhaustion every
    // tick (players only; a mob has no hunger, so the outcome is simply unused).
    Hunger,
};

// StatusEffectDef: the immutable definition an id derefs to. Mirrors 26.1's
// `MobEffect` fields that this game reads — category, colour, the per-tick
// behaviour — plus the movement-speed modifier factor that vanilla attaches
// through `addAttributeModifier(MOVEMENT_SPEED, …, ADD_MULTIPLIED_TOTAL)`.
struct StatusEffectDef final {
    StatusEffectCategory category = StatusEffectCategory::Neutral;
    // 0xRRGGBB, the particle/HUD tint vanilla stores on the effect.
    std::uint32_t color = 0xFFFFFFU;
    EffectKind kind = EffectKind::None;
    // The MOVEMENT_SPEED modifier per amplifier level, added as a total-multiply:
    // effective speed is `base * (1 + speedModifierPerLevel * (amplifier + 1))`.
    // +0.2 for speed, -0.15 for slowness, 0 for everything else.
    float speedModifierPerLevel = 0.0F;
    // Whether the effect resolves once on application rather than ticking down
    // (instant health/damage). None of the five built-ins are instant yet, but
    // the flag is the axis the potion content will set, so it is modelled now.
    bool instant = false;
};

// MobEffectInstance, reduced to its three fields. `amplifier` is the level minus
// one (amplifier 0 == "Poison I"), matching vanilla. `durationTicks` counts
// down; the interval effects fire when `durationTicks % interval == 0`, the same
// `tickCount = this.duration` rule MobEffectInstance.tick uses.
struct EffectInstance final {
    core::StatusEffectId id{};
    std::int32_t durationTicks = 0;
    std::uint8_t amplifier = 0U;

    [[nodiscard]] bool active() const { return id.valid() && durationTicks > 0; }
};

// The per-LivingEntity active-effect store: a small fixed array plus a count.
// Four is comfortably above what any single mob carries in practice; a fifth
// concurrent effect is dropped (see applyEffect) rather than allocating. An
// entity with no effects is `count == 0` and costs one integer to skip.
struct ActiveEffects final {
    static constexpr std::size_t kMaxEffects = 4U;

    std::array<EffectInstance, kMaxEffects> entries{};
    std::uint8_t count = 0U;

    [[nodiscard]] bool empty() const { return count == 0U; }
    [[nodiscard]] std::size_t size() const { return count; }
};

// What one tick of the effect set wants the caller to apply. The store itself
// never touches health or movement — it emits this, and the mob's DamageState
// or the player's PlayerVitals applies it, so one implementation serves both.
struct EffectTickOutcome final {
    // Damage to deal this tick (poison), with its type for the pipeline.
    float damage = 0.0F;
    DamageType damageType = DamageType::None;
    // Health to restore this tick (regeneration).
    float heal = 0.0F;
    // Food exhaustion to add this tick (hunger; a mob ignores it).
    float exhaustion = 0.0F;
    // The multiplicative movement-speed factor from speed/slowness, 1.0 when
    // neither is active. Applied on top of the base attribute every tick, so the
    // speed restores itself the instant the effect expires — no modifier to
    // unload, no base value ever mutated.
    float speedMultiplier = 1.0F;
};

// --- The registry (defined in StatusEffect.cpp) ---

// The R0-style dense registry of effect definitions. Populated and frozen on
// first access with the five built-ins, so `byName("poison")` resolves and a
// late/duplicate registration aborts through the registry's own guards.
class StatusEffectRegistry;
[[nodiscard]] StatusEffectRegistry& statusEffectRegistry();

// Convenience resolvers over the singleton. `byName` never aborts (an unknown
// name is an expected miss); `def` derefs an id the caller already holds.
[[nodiscard]] core::StatusEffectId statusEffectByName(std::string_view name);
[[nodiscard]] const StatusEffectDef& statusEffectDef(core::StatusEffectId id);
// The canonical registry path an id was filed under (e.g. "poison"), the stable
// name a save stores instead of the per-run id. Empty for an invalid id.
[[nodiscard]] std::string_view statusEffectName(core::StatusEffectId id);

// The built-in ids, resolved once. These are the handles content nodes name
// (AR-M's hunger, AR-A3's cleanse); holding the id avoids a name lookup per use.
[[nodiscard]] core::StatusEffectId poisonEffect();
[[nodiscard]] core::StatusEffectId regenerationEffect();
[[nodiscard]] core::StatusEffectId hungerEffect();
[[nodiscard]] core::StatusEffectId speedEffect();
[[nodiscard]] core::StatusEffectId slownessEffect();

// --- The per-entity API (free functions over ActiveEffects) ---

// MobEffectInstance's "stronger or longer wins" merge: applying an effect the
// entity already has replaces it only when the new one is a higher amplifier,
// or the same amplifier with at least as much duration. Adding a brand-new
// effect uses a free slot; with none free the weakest is not evicted — the new
// effect is simply dropped (the array is sized so this never happens for the
// built-ins). Returns true when the store changed.
bool applyEffect(ActiveEffects& effects, core::StatusEffectId id, std::int32_t durationTicks,
                 std::uint8_t amplifier);

// Removes one effect by id; returns true when it was present.
bool removeEffect(ActiveEffects& effects, core::StatusEffectId id);

// Removes every effect. Returns the number cleared (AR-A3 milk reports it).
std::size_t clearEffects(ActiveEffects& effects);

[[nodiscard]] bool hasEffect(const ActiveEffects& effects, core::StatusEffectId id);

// The active instance for an id, or nullptr. Const and mutable forms so a caller
// can read a remaining duration or (rarely) adjust one.
[[nodiscard]] const EffectInstance* getEffect(const ActiveEffects& effects,
                                              core::StatusEffectId id);

// Advances every active effect one tick: fires each interval behaviour into the
// returned outcome, decrements durations, and compacts out any that reached
// zero. `currentHealth` gates poison (it never drops the victim below one
// health). Deterministic: no RNG, purely a function of the store and the health.
EffectTickOutcome tickEffects(ActiveEffects& effects, float currentHealth);

// The interval an interval-kind effect fires on at a given amplifier, exposed so
// tests assert the vanilla cadence directly. Non-interval kinds return 1.
[[nodiscard]] int effectTickInterval(EffectKind kind, std::uint8_t amplifier);

} // namespace mc::gameplay

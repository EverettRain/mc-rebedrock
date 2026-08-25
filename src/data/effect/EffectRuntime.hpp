#pragma once

// DDC-2: the run-time effect executor — DOD, zero allocation, near hard-coded.
//
// This is the hot-path half of DDC-DESIGN.md §3. Given a compiled EffectProgram
// (built once at load by EffectCompiler) and a live EffectContext, it walks the
// SoA buckets by array subscript — table[level-1] lookups, a ValueOp switch, a
// flat predicate evaluation over pooled ranges — with no JSON, no variant
// dispatch, no heap allocation. `applyDamageModifiers` is a handful of multiply-
// adds; sabotage ① (leaving the value tree interpreted) is exactly what the
// micro-benchmark in the test measures the absence of.
//
// The engine stays gameplay-free: the context supplies the live numbers (damage-
// source tag membership, entity-type tag membership per slot, a deterministic
// JavaRandom, the base amount and level) as plain callbacks / fields, and
// post_attack actions are *reported* as a small fixed-capacity list of POD
// outcomes for the gameplay caller (the future EQ-4 / ENCH-1b damage-pipeline
// wiring) to apply — the executor itself neither ignites entities nor spends
// durability. That keeps DDC-2 within its scope (no gameplay entity/interaction
// dependency) while giving the回填 lines a clean, reusable evaluation surface.
//
// Determinism: every random_chance draw goes through ctx.random (a
// world::gen::JavaRandom the caller owns and seeds), never a wall clock or a
// global rng — the RNG rule the sabotage ② test replays.

#include "data/effect/EffectIR.hpp"
#include "world/gen/JavaRandom.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace mc::data::effect {

// The live inputs one evaluation needs, filled by the gameplay caller. All
// lookups are cheap function objects the caller binds to its damage source /
// entity state; the executor calls them, never stores strings. Kept as
// std::function so the header has no gameplay type dependency — the calls are off
// the innermost multiply-add loop (once per predicate leaf, not per level), so
// the indirection does not sit on the arithmetic hot path.
struct EffectContext {
    std::int32_t level = 1;      // the enchantment's level on this instance
    float baseAmount = 0.0F;     // the base value ops multiply against (raw damage)
    bool isDirect = true;        // was the hit direct (not a projectile/proxy)?

    // Is damage-source tag `tag` (a `minecraft:` id string) set on this hit? Used
    // by damage_source_properties (protection's bypasses_invulnerability etc.).
    std::function<bool(std::string_view tag)> sourceHasTag =
        [](std::string_view) { return false; };

    // Does the entity in `slot` carry entity-type tag `tag`? Used by
    // entity_properties (smite's undead, bane's arthropod, power's arrows).
    std::function<bool(TargetSlot slot, std::string_view tag)> entityHasTag =
        [](TargetSlot, std::string_view) { return false; };

    // The deterministic stream random_chance draws from. The caller owns and
    // seeds it (per-hit derived seed, never wall clock).
    world::gen::JavaRandom* random = nullptr;
};

namespace detail {

// Evaluates a compiled predicate by index. Flat: combinators iterate a pooled
// child range, no recursion through owning pointers. Reads `chance` for
// random_chance from ctx.random (deterministic).
[[nodiscard]] inline bool evalPredicate(const EffectProgram& program, std::uint32_t index,
                                        EffectContext& ctx) {
    const Predicate& predicate = program.predicates[index];
    switch (predicate.kind) {
        case PredicateKind::AlwaysTrue:
            return true;
        case PredicateKind::AlwaysFalse:
            return false;
        case PredicateKind::DamageSourceTags: {
            for (std::uint32_t i = predicate.tagBegin; i < predicate.tagEnd; ++i) {
                const SourceTagCheck& check = program.sourceTagChecks[i];
                const bool present = ctx.sourceHasTag(program.sourceTags[check.tagId]);
                if (present != check.expected) return false;
            }
            return true;
        }
        case PredicateKind::IsDirect:
            return ctx.isDirect == predicate.expected;
        case PredicateKind::EntityTag:
            return ctx.entityHasTag(predicate.slot, program.entityTags[predicate.tagId]);
        case PredicateKind::RandomChance: {
            if (ctx.random == nullptr) return false;
            const float chance = predicate.chance.eval(ctx.level);
            return ctx.random->nextFloat() < chance;
        }
        case PredicateKind::AllOf: {
            for (std::uint32_t i = predicate.childBegin; i < predicate.childEnd; ++i) {
                if (!evalPredicate(program, i, ctx)) return false;
            }
            return true;
        }
        case PredicateKind::AnyOf: {
            for (std::uint32_t i = predicate.childBegin; i < predicate.childEnd; ++i) {
                if (evalPredicate(program, i, ctx)) return true;
            }
            return predicate.childBegin == predicate.childEnd;  // empty any_of ⇒ true (JE)
        }
        case PredicateKind::Inverted:
            return !evalPredicate(program, predicate.childBegin, ctx);
    }
    return false;
}

// Applies one ValueOp bucket to a running (base,total) pair, honouring each
// term's predicate. Mirrors JE's ValueEffect fold: add/set touch the total,
// multiply_base scales the base then re-adds the delta, multiply_total scales the
// accumulated total.
[[nodiscard]] inline float applyValueBucket(const EffectProgram& program,
                                            const std::vector<ValueTerm>& bucket,
                                            EffectContext& ctx) {
    float total = ctx.baseAmount;
    float base = ctx.baseAmount;
    for (const ValueTerm& term : bucket) {
        if (!evalPredicate(program, term.predicate, ctx)) continue;
        const float value = term.table.at(ctx.level);
        switch (term.op) {
            case ValueOp::Add:
                total += value;
                break;
            case ValueOp::Set:
                total = value;
                break;
            case ValueOp::MultiplyBase: {
                const float delta = base * value;
                base += delta;
                total += delta;
                break;
            }
            case ValueOp::MultiplyTotal:
                total *= value;
                break;
        }
    }
    return total;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Damage / protection — the value hot path.
// ---------------------------------------------------------------------------

// The extra outgoing damage this enchantment's `minecraft:damage` bucket adds on
// top of ctx.baseAmount (which the caller sets to the raw pre-enchant damage, or
// 0 to read the bonus alone). sharpness lvl L ⇒ 0.5*L+0.5 over base 0 — the
// golden the acceptance test asserts. Zero-allocation, table subscript + a few
// multiply-adds.
[[nodiscard]] inline float applyDamageModifiers(const EffectProgram& program,
                                                EffectContext& ctx) {
    return detail::applyValueBucket(program, program.damage, ctx);
}

// The damage-reduction points this enchantment's `minecraft:damage_protection`
// bucket contributes (JE sums these across worn armor, then folds into the
// protection formula). ctx.baseAmount is 0 for the pure protection value.
// protection's bypasses_invulnerability predicate makes a bypassing source
// return 0 (no reduction) — the acceptance test's protection golden.
[[nodiscard]] inline float applyDamageProtection(const EffectProgram& program,
                                                 EffectContext& ctx) {
    return detail::applyValueBucket(program, program.damageProtection, ctx);
}

// ---------------------------------------------------------------------------
// Post-attack — reported as POD outcomes for the gameplay caller to apply.
// ---------------------------------------------------------------------------

// One resolved post_attack effect. The executor produces these; the gameplay
// caller (EQ-4 / ENCH-1b) applies them — igniting the victim, dealing thorns
// damage, spending durability — so DDC-2 owns the *decision* (which action,
// which target, what magnitude, gated on a deterministic chance) and gameplay
// owns the *mutation*. POD, fixed set of fields.
struct PostAttackOutcome final {
    ActionKind kind = ActionKind::Unknown;
    TargetSlot affected = TargetSlot::Victim;
    TargetSlot enchanted = TargetSlot::Attacker;
    float value = 0.0F;      // ignite seconds / mob-effect duration / item damage amount
    float minDamage = 0.0F;  // damage_entity min / mob-effect min amplifier
    float maxDamage = 0.0F;  // damage_entity max / mob-effect max amplifier
    std::uint16_t idRef = 0; // interned id index into EffectProgram::ids
};

// A small fixed-capacity outcome buffer so runPostAttack allocates nothing on the
// hot path. 26.1's post_attack all_of never fans out past a couple of leaf
// actions per enchantment; this is generous.
inline constexpr std::size_t kMaxPostAttackOutcomes = 8;

struct PostAttackResult final {
    std::array<PostAttackOutcome, kMaxPostAttackOutcomes> outcomes{};
    std::size_t count = 0;

    void push(const PostAttackOutcome& outcome) {
        if (count < kMaxPostAttackOutcomes) outcomes[count++] = outcome;
    }
    [[nodiscard]] const PostAttackOutcome* begin() const { return outcomes.data(); }
    [[nodiscard]] const PostAttackOutcome* end() const { return outcomes.data() + count; }
};

namespace detail {

// Emits an action (and its all_of children) into `result` if it is not a no-op.
// Leaf magnitudes are read off the compiled tables at ctx.level.
inline void emitAction(const EffectProgram& program, const Action& action, EffectContext& ctx,
                       PostAttackResult& result) {
    switch (action.kind) {
        case ActionKind::Ignite: {
            PostAttackOutcome outcome{};
            outcome.kind = ActionKind::Ignite;
            outcome.affected = action.affected;
            outcome.enchanted = action.enchanted;
            outcome.value = action.curve.at(ctx.level);  // ignite seconds
            result.push(outcome);
            break;
        }
        case ActionKind::DamageEntity: {
            PostAttackOutcome outcome{};
            outcome.kind = ActionKind::DamageEntity;
            outcome.affected = action.affected;
            outcome.enchanted = action.enchanted;
            outcome.minDamage = action.minDamage;
            outcome.maxDamage = action.maxDamage;
            outcome.idRef = action.idRef;
            // JE rolls a uniform in [min,max] for thorns; deterministic draw.
            if (ctx.random != nullptr && action.maxDamage > action.minDamage) {
                const float span = action.maxDamage - action.minDamage;
                outcome.value = action.minDamage + ctx.random->nextFloat() * span;
            } else {
                outcome.value = action.minDamage;
            }
            result.push(outcome);
            break;
        }
        case ActionKind::ChangeItemDamage: {
            PostAttackOutcome outcome{};
            outcome.kind = ActionKind::ChangeItemDamage;
            outcome.affected = action.affected;
            outcome.enchanted = action.enchanted;
            outcome.value = action.amount;
            result.push(outcome);
            break;
        }
        case ActionKind::ApplyMobEffect: {
            PostAttackOutcome outcome{};
            outcome.kind = ActionKind::ApplyMobEffect;
            outcome.affected = action.affected;
            outcome.enchanted = action.enchanted;
            outcome.idRef = action.idRef;
            outcome.minDamage = action.minDamage;  // amplifier range
            outcome.maxDamage = action.maxDamage;
            outcome.value = action.curve.at(ctx.level);  // duration
            result.push(outcome);
            break;
        }
        case ActionKind::AllOf:
            for (std::uint32_t i = action.childBegin; i < action.childEnd; ++i) {
                emitAction(program, program.actions[i], ctx, result);
            }
            break;
        case ActionKind::Unknown:
            break;  // counted at compile, no-op at runtime
    }
}

}  // namespace detail

// Resolves the `minecraft:post_attack` bucket into the outcome list, honouring
// each action's predicate (thorns' random_chance, fire_aspect's is_direct). No
// heap allocation — the result is a fixed-capacity POD buffer. Deterministic:
// the chance draw and the thorns damage roll both come from ctx.random.
[[nodiscard]] inline PostAttackResult runPostAttack(const EffectProgram& program,
                                                    EffectContext& ctx) {
    PostAttackResult result{};
    for (const Action& action : program.postAttack) {
        if (!detail::evalPredicate(program, action.predicate, ctx)) continue;
        detail::emitAction(program, action, ctx, result);
    }
    return result;
}

}  // namespace mc::data::effect

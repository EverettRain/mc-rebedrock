#pragma once

// DDC-2: the effect-component engine's compiled intermediate representation.
//
// DDC-1 carries JE 26.1's enchantment `effects` object through verbatim as raw
// JSON text (EnchantmentDef::rawEffects), deliberately never interpreting a
// single component type. DDC-2 is the node that finally reads it — but not on a
// hot path. The design (DDC-DESIGN.md §3, "性能评估结论") is a *compiler*: the
// JSON value tree, the level curves, the predicates are all finite sets, so at
// load time they fold into the flat, trivially-copyable POD this header defines,
// and the damage / mining / tick hot paths then execute that POD by array
// subscript — no variant dispatch, no pointer chasing through an object graph,
// no JSON re-parse, no allocation. Near hard-coded (the sabotage ① that leaves
// the value tree as a runtime-interpreted recursion is what this exists to
// avoid; the golden micro-benchmark in the test proves the gap).
//
// The IR is three layers, mirroring 26.1's JSON shape (DDC-DESIGN.md §2):
//
//   * LevelCurve — the folded LevelBasedValue. `linear{base, per_level_above_
//     first}` / `constant` / `clamped{min,max,inner}` / `levels_squared` all
//     reduce to <=4 floats plus a u8 kind. max_level is small (<=255) so a curve
//     also pre-expands into a LevelTable (values[level-1]) the runtime reads with
//     one subscript, never re-running the affine math per hit.
//   * Predicate — a `requirements` loot-condition tree, compiled to a flat POD
//     pool (all_of / any_of / inverted point at child ranges in one vector, not a
//     recursive object graph). The subset the 40 enchantments actually reference:
//     damage_source_properties (tag list + is_direct), entity_properties (a
//     `#tag` type check on a target slot), random_chance (a curve), and the
//     combinators. Unknown conditions compile to an AlwaysFalse sentinel and are
//     counted (never silently dropped — sabotage ③).
//   * ValueOp / Action — `damage` / `damage_protection` fold to a ValueOp term
//     (add / multiply_base / multiply_total over a curve); `post_attack` folds to
//     an Action (ignite / damage_entity / change_item_damage / apply_mob_effect /
//     all_of over a child range). Everything is flattened into pools; a term or
//     action names a target slot (victim / attacker / direct_attacker) as a u8
//     index, never a string, at runtime.
//
// Everything here is a plain aggregate of scalars and pooled indices, so an
// EffectProgram is a handful of contiguous vectors — cache-friendly SoA, movable,
// with value semantics. No gameplay dependency: the runtime context (below, in
// EffectRuntime.hpp) supplies the live numbers; this is pure data.

#include "world/gen/JavaRandom.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mc::data::effect {

// The upper bound on a JE enchantment's max_level worth pre-expanding into a
// per-level table. 26.1's highest is 5 (sharpness); a clamp keeps a hostile
// datapack claiming max_level 9999 from expanding a giant table — beyond this the
// curve is evaluated closed-form at runtime instead (still branch-light).
inline constexpr std::int32_t kMaxExpandedLevel = 32;

// ---------------------------------------------------------------------------
// LevelCurve — the folded LevelBasedValue.
// ---------------------------------------------------------------------------

enum class CurveKind : std::uint8_t {
    Constant = 0,     // value = base                       (JE minecraft:constant / a bare number)
    Linear,           // value = base + (level-1)*perLevel  (JE minecraft:linear)
    LevelsSquared,    // value = base + level*level         (JE minecraft:levels_squared, `added`)
    Clamped,          // value = clamp(inner(level), min, max)  (JE minecraft:clamped)
    Lookup,           // value = level<=n ? table[level-1] : fallback(level)  (JE minecraft:lookup)
};

// A LevelBasedValue folded to <=4 floats. `clamped` and `lookup` wrap an inner
// linear curve (26.1 never nests deeper on the components DDC-2 covers), so the
// inner affine coefficients live in {base, perLevel} and the wrapper bounds /
// table live alongside — no recursion, no separate pool entry for the inner
// curve.
struct LevelCurve final {
    CurveKind kind = CurveKind::Constant;
    float base = 0.0F;        // linear/constant base; levels_squared additive base
    float perLevel = 0.0F;    // linear per_level_above_first
    float lo = 0.0F;          // clamped min
    float hi = 0.0F;          // clamped max
    // Lookup's explicit per-level values (JE minecraft:lookup `values`); beyond
    // its size the {base,perLevel} inner linear is the fallback.
    std::vector<float> lookup;

    [[nodiscard]] float eval(std::int32_t level) const {
        switch (kind) {
            case CurveKind::Constant:
                return base;
            case CurveKind::Linear:
                return base + static_cast<float>(level - 1) * perLevel;
            case CurveKind::LevelsSquared:
                return base + static_cast<float>(level) * static_cast<float>(level);
            case CurveKind::Clamped: {
                const float inner = base + static_cast<float>(level - 1) * perLevel;
                return inner < lo ? lo : (inner > hi ? hi : inner);
            }
            case CurveKind::Lookup: {
                const auto index = static_cast<std::size_t>(level - 1);
                if (level >= 1 && index < lookup.size()) return lookup[index];
                return base + static_cast<float>(level - 1) * perLevel;  // fallback linear
            }
        }
        return 0.0F;
    }

    [[nodiscard]] bool operator==(const LevelCurve&) const = default;
};

// A curve pre-expanded across levels 1..count. The runtime reads values[level-1]
// with one subscript instead of re-running eval() per hit — the "预展开成小表 →
// table[level-1] 查表" of DDC-DESIGN.md §3. `count` is min(max_level,
// kMaxExpandedLevel); levels beyond fall back to the LevelCurve's closed form
// (still cheap), which is why the curve is kept alongside the table.
struct LevelTable final {
    LevelCurve curve{};
    std::array<float, kMaxExpandedLevel> values{};
    std::int32_t count = 0;

    void build(const LevelCurve& source, std::int32_t maxLevel) {
        curve = source;
        count = maxLevel < 1 ? 0 : (maxLevel > kMaxExpandedLevel ? kMaxExpandedLevel : maxLevel);
        for (std::int32_t level = 1; level <= count; ++level) {
            values[static_cast<std::size_t>(level - 1)] = source.eval(level);
        }
    }

    [[nodiscard]] float at(std::int32_t level) const {
        if (level >= 1 && level <= count) return values[static_cast<std::size_t>(level - 1)];
        return curve.eval(level);  // out of the expanded window: closed form
    }
};

// ---------------------------------------------------------------------------
// Target slots — victim / attacker / direct_attacker as a u8, never a string.
// ---------------------------------------------------------------------------

// JE's affected/enchanted target names and predicate `entity` slots, interned to
// a u8 the runtime maps to a context entity index. "this" is the enchanted
// entity's damage-context self (the attacker in a `damage` component); the JSON
// disambiguates per-component and the compiler resolves it.
enum class TargetSlot : std::uint8_t {
    Victim = 0,          // the entity being hurt
    Attacker,            // the entity that dealt the hit (holds the enchantment for `damage`)
    DirectAttacker,      // the projectile / immediate source (arrows, trident)
    Enchanted,           // JE "enchanted" — resolved to attacker/victim at compile
    ThisEntity,          // JE predicate `entity: "this"`
    Count,
};

[[nodiscard]] inline TargetSlot parseTargetSlot(std::string_view name, bool& ok) {
    ok = true;
    if (name == "victim") return TargetSlot::Victim;
    if (name == "attacker") return TargetSlot::Attacker;
    if (name == "direct_attacker") return TargetSlot::DirectAttacker;
    if (name == "enchanted") return TargetSlot::Enchanted;
    if (name == "this") return TargetSlot::ThisEntity;
    ok = false;
    return TargetSlot::ThisEntity;
}

// ---------------------------------------------------------------------------
// Predicate IR — a flat POD pool.
// ---------------------------------------------------------------------------

enum class PredicateKind : std::uint8_t {
    AlwaysTrue = 0,       // an empty `requirements` (the unconditional damage bonus)
    AlwaysFalse,          // an unknown condition compiled to a sentinel (counted)
    DamageSourceTags,     // damage_source_properties: a run of tag {id, expected}
    IsDirect,             // damage_source_properties: is_direct flag
    EntityTag,            // entity_properties: a `#tag` type check on a target slot
    RandomChance,         // random_chance: a curve, drawn from ctx JavaRandom
    AllOf,                // every child in [childBegin, childEnd) is true
    AnyOf,                // some child in [childBegin, childEnd) is true
    Inverted,             // childBegin (single child) is false
};

// One damage-source tag requirement (JE `{ "id": ..., "expected": bool }`). The
// tag id is interned to a u16 into EffectProgram::sourceTags at compile; the
// runtime asks its context "is source tag T set?" and compares to `expected`.
struct SourceTagCheck final {
    std::uint16_t tagId = 0;
    bool expected = true;
    [[nodiscard]] bool operator==(const SourceTagCheck&) const = default;
};

// A compiled predicate node. Which fields are live depends on `kind`; the union
// is expressed as plain sibling fields (trivially copyable, no std::variant) with
// pooled child/tag ranges rather than owning pointers.
struct Predicate final {
    PredicateKind kind = PredicateKind::AlwaysTrue;
    TargetSlot slot = TargetSlot::ThisEntity;  // EntityTag / IsDirect subject
    bool expected = true;                      // IsDirect expected value
    std::uint16_t tagId = 0;                    // EntityTag interned tag id
    std::uint32_t tagBegin = 0;                 // DamageSourceTags range into sourceTagChecks
    std::uint32_t tagEnd = 0;
    std::uint32_t childBegin = 0;               // AllOf/AnyOf/Inverted range into predicates
    std::uint32_t childEnd = 0;
    LevelCurve chance{};                        // RandomChance curve

    [[nodiscard]] bool operator==(const Predicate&) const = default;
};

// ---------------------------------------------------------------------------
// ValueOp terms — the `damage` / `damage_protection` arithmetic.
// ---------------------------------------------------------------------------

enum class ValueOp : std::uint8_t {
    Add = 0,          // total += curve(level)          (JE minecraft:add)
    MultiplyBase,     // base  *= curve(level)          (JE minecraft:multiply_base)
    MultiplyTotal,    // total *= curve(level)          (JE minecraft:multiply_total)
    Set,              // total  = curve(level)          (JE minecraft:set)
};

// A `damage` / `damage_protection` entry: an operation over a per-level table,
// gated by a compiled predicate (index into EffectProgram::predicates, or
// kNoPredicate for the unconditional case).
struct ValueTerm final {
    ValueOp op = ValueOp::Add;
    LevelTable table{};
    std::uint32_t predicate = 0;   // index into predicates; predicates[0] is AlwaysTrue
    [[nodiscard]] bool operator==(const ValueTerm&) const = default;
};

// ---------------------------------------------------------------------------
// Action IR — the `post_attack` effects, flattened into a pool.
// ---------------------------------------------------------------------------

enum class ActionKind : std::uint8_t {
    Ignite = 0,          // set the affected on fire for a curve of seconds
    DamageEntity,        // deal min..max damage of a type to the affected
    ChangeItemDamage,    // spend `amount` durability on the enchanted item
    ApplyMobEffect,      // apply a status effect (id + amplifier + duration curve)
    AllOf,               // run every child action in [childBegin, childEnd)
    Unknown,             // an unrecognised action, compiled as a no-op sentinel (counted)
};

struct Action final {
    ActionKind kind = ActionKind::Unknown;
    TargetSlot affected = TargetSlot::Victim;   // who the action targets
    TargetSlot enchanted = TargetSlot::Attacker; // who holds the enchantment (item damage / self)
    std::uint32_t predicate = 0;                 // gating predicate index

    // Ignite: seconds curve. DamageEntity: min/max damage. ChangeItemDamage:
    // amount. ApplyMobEffect: amplifier + duration curve, effect id interned.
    LevelTable curve{};        // ignite duration / mob-effect duration
    float minDamage = 0.0F;    // damage_entity / apply_mob_effect min amplifier
    float maxDamage = 0.0F;    // damage_entity / apply_mob_effect max amplifier
    float amount = 0.0F;       // change_item_damage
    std::uint16_t idRef = 0;   // interned damage_type / mob_effect / entity id

    std::uint32_t childBegin = 0;  // AllOf range into actions
    std::uint32_t childEnd = 0;

    [[nodiscard]] bool operator==(const Action&) const = default;
};

// ---------------------------------------------------------------------------
// EffectProgram — one enchantment's compiled effects, SoA.
// ---------------------------------------------------------------------------

// Sentinel: a term/action whose `requirements` was absent. predicates[0] is
// always AlwaysTrue so the runtime can subscript unconditionally.
inline constexpr std::uint32_t kAlwaysTruePredicate = 0;

// The compiled form of one enchantment's `effects` object. Buckets are separate
// vectors (SoA): the damage hot path iterates only `damage`, the defence path
// only `damageProtection`, the post-attack path only `postAttack` — each never
// pays for the others. Predicates, child predicate/action fan-out, and the two
// intern tables are shared pools the entries index into.
struct EffectProgram final {
    std::vector<ValueTerm> damage;             // minecraft:damage bucket
    std::vector<ValueTerm> damageProtection;   // minecraft:damage_protection bucket
    std::vector<Action> postAttack;            // minecraft:post_attack top-level actions

    std::vector<Predicate> predicates;         // predicate pool; [0] == AlwaysTrue
    std::vector<SourceTagCheck> sourceTagChecks; // DamageSourceTags fan-out
    std::vector<Action> actions;               // AllOf child-action fan-out pool

    // Intern tables — a string appears once; entries hold the index. Kept for
    // diagnostics / the runtime's tag lookup; never touched on the arithmetic
    // path (which is all u16/float).
    std::vector<std::string> sourceTags;       // damage-source tag ids
    std::vector<std::string> entityTags;       // entity_properties `#tag` ids
    std::vector<std::string> ids;              // damage_type / mob_effect / entity ids

    std::int32_t maxLevel = 1;

    // Forward-compat / sabotage ③: components / value types / conditions /
    // actions DDC-2 does not model. Never silently dropped — counted so DDC-4's
    // compatibility audit sees them and the acceptance test asserts on them.
    std::int32_t unknownComponentCount = 0;
    std::int32_t unknownValueCount = 0;
    std::int32_t unknownConditionCount = 0;
    std::int32_t unknownActionCount = 0;

    EffectProgram() { predicates.push_back(Predicate{}); }  // predicates[0] = AlwaysTrue

    [[nodiscard]] std::int32_t totalUnknown() const {
        return unknownComponentCount + unknownValueCount + unknownConditionCount +
               unknownActionCount;
    }

    [[nodiscard]] bool empty() const {
        return damage.empty() && damageProtection.empty() && postAttack.empty();
    }
};

}  // namespace mc::data::effect

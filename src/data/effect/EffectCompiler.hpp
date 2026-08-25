#pragma once

// DDC-2: the load-time effect compiler.
//
// One function, `compileEffects`, turns JE 26.1's `effects` JSON (the raw text
// DDC-1 passed through as EnchantmentDef::rawEffects) into the flat
// EffectProgram POD the runtime executes. This is the only place JSON is touched
// on the effect path — it runs once, at datapack load, off any hot path
// (DDC-DESIGN.md §3, "加载期编译（一次性，非热路径）"). The runtime never sees
// a core::Json.
//
// It compiles the subset of 26.1's component / value / predicate / action
// vocabulary the 40 shipped enchantments reference on the combat path
// (`minecraft:damage`, `minecraft:damage_protection`, `minecraft:post_attack`)
// — the DDC-2 scope. Components it does not model are counted, never silently
// dropped: EffectProgram's four unknown* counters surface to DDC-4's
// compatibility audit and the acceptance test asserts on them (sabotage ③).
//
// Header-only, like DDC-1's EnchantmentFile / EnchantmentContentStore: it needs
// only core::Json (already in the runtime library) and the IR above, so it adds
// nothing to the build's source list.

#include "core/Json.hpp"
#include "data/effect/EffectIR.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace mc::data::effect {

namespace detail {

// Strips the `minecraft:` (or any) namespace so "minecraft:add" and a bare "add"
// both match. The compiler compares against un-namespaced spellings.
[[nodiscard]] inline std::string_view stripNamespace(std::string_view id) {
    const auto colon = id.find(':');
    return colon == std::string_view::npos ? id : id.substr(colon + 1);
}

// Interns `value` into `pool`, returning its index (added once). Small pools,
// linear scan — this is load time.
[[nodiscard]] inline std::uint16_t intern(std::vector<std::string>& pool, std::string_view value) {
    for (std::size_t index = 0; index < pool.size(); ++index) {
        if (pool[index] == value) return static_cast<std::uint16_t>(index);
    }
    pool.emplace_back(value);
    return static_cast<std::uint16_t>(pool.size() - 1);
}

// Compiles a LevelBasedValue. Accepts either a bare number (constant) or an
// object with a `type`. The affine {base, per_level_above_first} of `linear` is
// what nearly every enchantment uses; `clamped` / `levels_squared` / `lookup`
// wrap it. An unrecognised value type folds to a constant 0 and bumps the
// unknown counter.
inline LevelCurve compileCurve(const core::Json& json, std::int32_t& unknownValues) {
    LevelCurve curve{};
    if (json.isNumber()) {
        curve.kind = CurveKind::Constant;
        curve.base = json.asFloat();
        return curve;
    }
    if (!json.isObject()) {
        curve.kind = CurveKind::Constant;
        return curve;
    }
    const std::string_view type = stripNamespace(json["type"].asString());
    if (type == "linear") {
        curve.kind = CurveKind::Linear;
        curve.base = json["base"].asFloat();
        curve.perLevel = json["per_level_above_first"].asFloat();
    } else if (type == "constant") {
        curve.kind = CurveKind::Constant;
        curve.base = json["value"].asFloat();
    } else if (type == "levels_squared") {
        curve.kind = CurveKind::LevelsSquared;
        curve.base = json["added"].asFloat();
    } else if (type == "clamped") {
        const LevelCurve inner = compileCurve(json["value"], unknownValues);
        curve = inner;                 // carry the inner affine coefficients
        curve.kind = CurveKind::Clamped;
        curve.lo = json["min"].asFloat();
        curve.hi = json["max"].asFloat();
    } else if (type == "lookup") {
        const LevelCurve fallback = compileCurve(json["fallback"], unknownValues);
        curve = fallback;              // fallback affine for out-of-table levels
        curve.kind = CurveKind::Lookup;
        const core::Json& values = json["values"];
        if (values.isArray()) {
            curve.lookup.reserve(values.size());
            for (std::size_t index = 0; index < values.size(); ++index) {
                curve.lookup.push_back(values[index].asFloat());
            }
        }
    } else {
        ++unknownValues;
        curve.kind = CurveKind::Constant;  // safe no-op: contributes 0
    }
    return curve;
}

// Forward declaration: predicate compilation is mutually recursive through the
// combinators (all_of / any_of / inverted).
std::uint32_t compilePredicate(const core::Json& requirements, EffectProgram& program);

// Compiles the leaf `entity_properties` type check. 26.1's damage-modifier
// enchantments (bane_of_arthropods / smite / impaling / power) use it only as a
// `#tag` type test on a target entity ("this" / "direct_attacker"); richer
// entity predicates (flags / movement — wind_burst) are out of DDC-2 scope and
// counted.
inline std::uint32_t compileEntityProperties(const core::Json& term, EffectProgram& program) {
    bool ok = true;
    const TargetSlot slot = parseTargetSlot(term["entity"].asString(), ok);
    const core::Json& predicate = term["predicate"];
    // The only entity predicate shape DDC-2 models is `{ "type": "#tag" }` — a
    // damage-target-type tag. Anything else (flags/movement/vehicle) is unknown.
    if (ok && predicate.isObject() && predicate.contains("type") &&
        predicate["type"].isString() && predicate.asObject().size() == 1) {
        std::string_view tag = predicate["type"].asString();
        if (!tag.empty() && tag.front() == '#') tag.remove_prefix(1);
        Predicate node{};
        node.kind = PredicateKind::EntityTag;
        node.slot = slot;
        node.tagId = intern(program.entityTags, tag);
        program.predicates.push_back(node);
        return static_cast<std::uint32_t>(program.predicates.size() - 1);
    }
    ++program.unknownConditionCount;
    Predicate node{};
    node.kind = PredicateKind::AlwaysFalse;
    program.predicates.push_back(node);
    return static_cast<std::uint32_t>(program.predicates.size() - 1);
}

// Compiles `damage_source_properties` — a tag list ({id, expected}) and/or an
// `is_direct` flag. When both appear the two collapse into an AllOf over the
// tag-run node plus an is_direct node; the common case (tags only, or is_direct
// only) is a single node.
inline std::uint32_t compileDamageSource(const core::Json& term, EffectProgram& program) {
    const core::Json& predicate = term["predicate"];
    std::vector<std::uint32_t> parts;

    if (predicate.contains("tags") && predicate["tags"].isArray()) {
        const core::Json& tags = predicate["tags"];
        const auto begin = static_cast<std::uint32_t>(program.sourceTagChecks.size());
        for (std::size_t index = 0; index < tags.size(); ++index) {
            const core::Json& entry = tags[index];
            SourceTagCheck check{};
            check.tagId = intern(program.sourceTags, entry["id"].asString());
            check.expected = entry["expected"].asBool(true);
            program.sourceTagChecks.push_back(check);
        }
        Predicate node{};
        node.kind = PredicateKind::DamageSourceTags;
        node.tagBegin = begin;
        node.tagEnd = static_cast<std::uint32_t>(program.sourceTagChecks.size());
        program.predicates.push_back(node);
        parts.push_back(static_cast<std::uint32_t>(program.predicates.size() - 1));
    }
    if (predicate.contains("is_direct")) {
        Predicate node{};
        node.kind = PredicateKind::IsDirect;
        node.expected = predicate["is_direct"].asBool(true);
        program.predicates.push_back(node);
        parts.push_back(static_cast<std::uint32_t>(program.predicates.size() - 1));
    }

    if (parts.empty()) return kAlwaysTruePredicate;
    if (parts.size() == 1) return parts.front();

    // Combine the tag-run and is_direct nodes under one AllOf.
    const auto childBegin = static_cast<std::uint32_t>(program.predicates.size());
    for (std::uint32_t part : parts) program.predicates.push_back(program.predicates[part]);
    Predicate all{};
    all.kind = PredicateKind::AllOf;
    all.childBegin = childBegin;
    all.childEnd = static_cast<std::uint32_t>(program.predicates.size());
    program.predicates.push_back(all);
    return static_cast<std::uint32_t>(program.predicates.size() - 1);
}

// Compiles a `random_chance` condition (its `chance` is a LevelBasedValue,
// typically `enchantment_level` linear — thorns' 0.15*level). The draw happens
// at runtime against ctx's JavaRandom (never wall clock — sabotage ②).
inline std::uint32_t compileRandomChance(const core::Json& term, EffectProgram& program) {
    Predicate node{};
    node.kind = PredicateKind::RandomChance;
    // JE nests the level curve under `chance` for enchantment_level; the value
    // itself lives under `amount`. Fall back to `chance` being a bare curve.
    const core::Json& chance = term["chance"];
    const core::Json& amount = chance.contains("amount") ? chance["amount"] : chance;
    node.chance = compileCurve(amount, program.unknownValueCount);
    program.predicates.push_back(node);
    return static_cast<std::uint32_t>(program.predicates.size() - 1);
}

// Compiles a `requirements` loot-condition tree to a predicate index. An absent
// (empty) requirements is the unconditional AlwaysTrue at predicates[0].
inline std::uint32_t compilePredicate(const core::Json& requirements, EffectProgram& program) {
    if (!requirements.isObject() || !requirements.contains("condition")) {
        return kAlwaysTruePredicate;
    }
    const std::string_view condition = stripNamespace(requirements["condition"].asString());
    if (condition == "damage_source_properties") {
        return compileDamageSource(requirements, program);
    }
    if (condition == "entity_properties") {
        return compileEntityProperties(requirements, program);
    }
    if (condition == "random_chance") {
        return compileRandomChance(requirements, program);
    }
    if (condition == "inverted") {
        const std::uint32_t child = compilePredicate(requirements["term"], program);
        // Copy the child into the fan-out pool so Inverted owns a stable range.
        const auto begin = static_cast<std::uint32_t>(program.predicates.size());
        program.predicates.push_back(program.predicates[child]);
        Predicate node{};
        node.kind = PredicateKind::Inverted;
        node.childBegin = begin;
        node.childEnd = static_cast<std::uint32_t>(program.predicates.size());
        program.predicates.push_back(node);
        return static_cast<std::uint32_t>(program.predicates.size() - 1);
    }
    if (condition == "all_of" || condition == "any_of") {
        const char* key = requirements.contains("terms") ? "terms" : "predicates";
        const core::Json& terms = requirements[key];
        std::vector<std::uint32_t> children;
        if (terms.isArray()) {
            children.reserve(terms.size());
            for (std::size_t index = 0; index < terms.size(); ++index) {
                children.push_back(compilePredicate(terms[index], program));
            }
        }
        const auto begin = static_cast<std::uint32_t>(program.predicates.size());
        for (std::uint32_t child : children) program.predicates.push_back(program.predicates[child]);
        Predicate node{};
        node.kind = (condition == "all_of") ? PredicateKind::AllOf : PredicateKind::AnyOf;
        node.childBegin = begin;
        node.childEnd = static_cast<std::uint32_t>(program.predicates.size());
        program.predicates.push_back(node);
        return static_cast<std::uint32_t>(program.predicates.size() - 1);
    }
    // Unknown condition: a counted AlwaysFalse sentinel — never a silent pass.
    ++program.unknownConditionCount;
    Predicate node{};
    node.kind = PredicateKind::AlwaysFalse;
    program.predicates.push_back(node);
    return static_cast<std::uint32_t>(program.predicates.size() - 1);
}

// Compiles one value-modifier entry (a `damage` / `damage_protection` array
// element) into a ValueTerm. The `effect` is `{type: add/multiply_*, value:
// curve}`.
inline void compileValueTerm(const core::Json& entry, EffectProgram& program,
                             std::vector<ValueTerm>& bucket) {
    const core::Json& effect = entry["effect"];
    ValueTerm term{};
    const std::string_view op = stripNamespace(effect["type"].asString());
    if (op == "add") {
        term.op = ValueOp::Add;
    } else if (op == "multiply_base") {
        term.op = ValueOp::MultiplyBase;
    } else if (op == "multiply_total") {
        term.op = ValueOp::MultiplyTotal;
    } else if (op == "set") {
        term.op = ValueOp::Set;
    } else {
        ++program.unknownValueCount;
        return;  // unmodelled value op: do not emit a term (counted)
    }
    const LevelCurve curve = compileCurve(effect["value"], program.unknownValueCount);
    term.table.build(curve, program.maxLevel);
    term.predicate = compilePredicate(entry["requirements"], program);
    bucket.push_back(std::move(term));
}

// Compiles one action (a `post_attack` `effect`, possibly nested under all_of)
// into an Action, appending any children to `pool`. Returns the compiled Action
// by value; the caller places it in the right bucket/pool.
Action compileAction(const core::Json& effect, TargetSlot affected, TargetSlot enchanted,
                     std::uint32_t predicate, EffectProgram& program);

inline Action compileAction(const core::Json& effect, TargetSlot affected, TargetSlot enchanted,
                            std::uint32_t predicate, EffectProgram& program) {
    Action action{};
    action.affected = affected;
    action.enchanted = enchanted;
    action.predicate = predicate;
    const std::string_view type = stripNamespace(effect["type"].asString());
    if (type == "ignite") {
        action.kind = ActionKind::Ignite;
        action.curve.build(compileCurve(effect["duration"], program.unknownValueCount),
                           program.maxLevel);
    } else if (type == "damage_entity") {
        action.kind = ActionKind::DamageEntity;
        action.minDamage = effect["min_damage"].asFloat();
        action.maxDamage = effect["max_damage"].asFloat();
        action.idRef = intern(program.ids, effect["damage_type"].asString());
    } else if (type == "change_item_damage") {
        action.kind = ActionKind::ChangeItemDamage;
        action.amount = effect["amount"].asFloat();
    } else if (type == "apply_mob_effect") {
        action.kind = ActionKind::ApplyMobEffect;
        action.idRef = intern(program.ids, effect["to_apply"].asString());
        action.minDamage = effect["min_amplifier"].asFloat();
        action.maxDamage = effect["max_amplifier"].asFloat();
        action.curve.build(compileCurve(effect["max_duration"], program.unknownValueCount),
                           program.maxLevel);
    } else if (type == "all_of") {
        action.kind = ActionKind::AllOf;
        const core::Json& effects = effect["effects"];
        std::vector<Action> children;
        if (effects.isArray()) {
            children.reserve(effects.size());
            for (std::size_t index = 0; index < effects.size(); ++index) {
                children.push_back(compileAction(effects[index], affected, enchanted,
                                                 kAlwaysTruePredicate, program));
            }
        }
        action.childBegin = static_cast<std::uint32_t>(program.actions.size());
        for (auto& child : children) program.actions.push_back(std::move(child));
        action.childEnd = static_cast<std::uint32_t>(program.actions.size());
    } else {
        ++program.unknownActionCount;
        action.kind = ActionKind::Unknown;  // counted no-op
    }
    return action;
}

}  // namespace detail

// Compiles a parsed `effects` object into an EffectProgram. `maxLevel` is the
// enchantment's max_level (drives per-level table expansion). Non-fatal: an
// unmodelled component / value / condition / action is counted, never dropped
// silently — the returned program's unknown* counters and totalUnknown() report
// exactly what DDC-2 could not model, for DDC-4's audit.
[[nodiscard]] inline EffectProgram compileEffects(const core::Json& effects,
                                                  std::int32_t maxLevel) {
    EffectProgram program{};
    program.maxLevel = maxLevel < 1 ? 1 : maxLevel;
    if (!effects.isObject()) return program;

    for (const auto& [component, entries] : effects.asObject()) {
        const std::string_view kind = detail::stripNamespace(component);
        if (!entries.isArray()) {
            ++program.unknownComponentCount;
            continue;
        }
        if (kind == "damage") {
            for (std::size_t index = 0; index < entries.size(); ++index) {
                detail::compileValueTerm(entries[index], program, program.damage);
            }
        } else if (kind == "damage_protection") {
            for (std::size_t index = 0; index < entries.size(); ++index) {
                detail::compileValueTerm(entries[index], program, program.damageProtection);
            }
        } else if (kind == "post_attack") {
            for (std::size_t index = 0; index < entries.size(); ++index) {
                const core::Json& entry = entries[index];
                bool okA = true;
                bool okE = true;
                const TargetSlot affected =
                    parseTargetSlot(entry["affected"].asString(), okA);
                const TargetSlot enchanted =
                    parseTargetSlot(entry["enchanted"].asString(), okE);
                const std::uint32_t predicate =
                    detail::compilePredicate(entry["requirements"], program);
                program.postAttack.push_back(detail::compileAction(
                    entry["effect"], okA ? affected : TargetSlot::Victim,
                    okE ? enchanted : TargetSlot::Attacker, predicate, program));
            }
        } else {
            // A component DDC-2 does not model (attributes, location_changed,
            // trident_*, projectile_*, crossbow_*, tick, …). Counted per entry.
            program.unknownComponentCount += static_cast<std::int32_t>(entries.size());
        }
    }
    return program;
}

// Convenience: compile straight from the raw JSON text DDC-1 stored
// (EnchantmentDef::rawEffects). Empty text ⇒ an empty program. Malformed text is
// caught (core::Json::parse throws) and yields an empty program flagged with one
// unknown component so a bad blob is visible, never fatal.
[[nodiscard]] inline EffectProgram compileEffectsText(std::string_view rawEffects,
                                                      std::int32_t maxLevel) {
    if (rawEffects.empty()) {
        EffectProgram program{};
        program.maxLevel = maxLevel < 1 ? 1 : maxLevel;
        return program;
    }
    try {
        return compileEffects(core::Json::parse(rawEffects), maxLevel);
    } catch (const std::exception&) {
        EffectProgram program{};
        program.maxLevel = maxLevel < 1 ? 1 : maxLevel;
        ++program.unknownComponentCount;
        return program;
    }
}

}  // namespace mc::data::effect

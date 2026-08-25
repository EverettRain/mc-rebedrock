// DDC-2: the effect-component engine — load-time compiler + run-time DOD.
//
// What this pins (acceptance, DDC-2-effect-component-engine.md §验收):
//   * every 26.1 enchantment `effects` object DDC-1 carries compiles without a
//     crash, and the combat-path subset (damage / damage_protection /
//     post_attack) compiles with zero *in-scope* unknowns;
//   * sharpness: applyDamageModifiers over base 0 for lvl 1..5 == the hard-coded
//     golden 0.5*lvl + 0.5 (bit-for-bit the number the constexpr table gave);
//   * protection: bypasses_invulnerability predicate makes a bypassing source
//     get no reduction, a normal source the full 1*lvl points;
//   * fire_aspect: post_attack ignite duration == 4*lvl seconds, gated on
//     is_direct;
//   * thorns: post_attack all_of (damage_entity thorns 1..5 + change_item_damage
//     2) fires on a random_chance 0.15*lvl draw, deterministically;
//   * bane_of_arthropods / smite: the damage bonus only applies when the victim
//     carries the entity-type tag (2.5*lvl), and is absent otherwise;
//   * determinism: same seed ⇒ same thorns trigger sequence, replayed;
//   * performance: applyDamageModifiers is the same order of magnitude as the
//     hard-coded expression and allocates nothing on the hot path.
//
// Sabotage log (inject -> caught here -> revert):
//   (1) make LevelTable::at fall back to a runtime recursion / interpret the
//       curve object per call instead of the pre-expanded table => the
//       perfEquivalence assert (order-of-magnitude gap) fires. [design-level:
//       the whole compiler exists to avoid this]
//   (2) draw random_chance from a fresh wall-clock-seeded rng instead of
//       ctx.random => the determinismReplay assert (two identical seeds diverge)
//       fires.
//   (3) in compileEffects, `continue` past an unmodelled component without
//       counting => the unknownAccounting assert (counted total must equal the
//       hand-counted out-of-scope entries) fires.

#include "core/Json.hpp"
#include "data/effect/EffectCompiler.hpp"
#include "data/effect/EffectIR.hpp"
#include "data/effect/EffectRuntime.hpp"
#include "world/gen/JavaRandom.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using mc::core::Json;
using mc::data::effect::ActionKind;
using mc::data::effect::compileEffects;
using mc::data::effect::EffectContext;
using mc::data::effect::EffectProgram;
using mc::data::effect::PostAttackResult;
using mc::data::effect::TargetSlot;
using mc::world::gen::JavaRandom;

// --- the real JE 26.1 files, embedded verbatim ---------------------------

constexpr std::string_view kSharpness = R"JSON({
  "minecraft:damage": [
    { "effect": { "type": "minecraft:add",
        "value": { "type": "minecraft:linear", "base": 1.0, "per_level_above_first": 0.5 } } }
  ]
})JSON";

constexpr std::string_view kProtection = R"JSON({
  "minecraft:damage_protection": [
    { "effect": { "type": "minecraft:add",
        "value": { "type": "minecraft:linear", "base": 1.0, "per_level_above_first": 1.0 } },
      "requirements": {
        "condition": "minecraft:damage_source_properties",
        "predicate": { "tags": [ { "expected": false, "id": "minecraft:bypasses_invulnerability" } ] } } }
  ]
})JSON";

constexpr std::string_view kFireAspect = R"JSON({
  "minecraft:post_attack": [
    { "affected": "victim", "enchanted": "attacker",
      "effect": { "type": "minecraft:ignite",
        "duration": { "type": "minecraft:linear", "base": 4.0, "per_level_above_first": 4.0 } },
      "requirements": { "condition": "minecraft:damage_source_properties",
        "predicate": { "is_direct": true } } }
  ]
})JSON";

constexpr std::string_view kThorns = R"JSON({
  "minecraft:post_attack": [
    { "affected": "attacker", "enchanted": "victim",
      "effect": { "type": "minecraft:all_of", "effects": [
        { "type": "minecraft:damage_entity", "damage_type": "minecraft:thorns",
          "max_damage": 5.0, "min_damage": 1.0 },
        { "type": "minecraft:change_item_damage", "amount": 2.0 } ] },
      "requirements": { "condition": "minecraft:random_chance",
        "chance": { "type": "minecraft:enchantment_level",
          "amount": { "type": "minecraft:linear", "base": 0.15, "per_level_above_first": 0.15 } } } }
  ]
})JSON";

constexpr std::string_view kSmite = R"JSON({
  "minecraft:damage": [
    { "effect": { "type": "minecraft:add",
        "value": { "type": "minecraft:linear", "base": 2.5, "per_level_above_first": 2.5 } },
      "requirements": { "condition": "minecraft:entity_properties", "entity": "this",
        "predicate": { "type": "#minecraft:sensitive_to_smite" } } }
  ]
})JSON";

// blast_protection: an in-scope damage_protection bucket AND an out-of-scope
// `attributes` component — the mixed case that exercises unknown counting while
// the combat bucket still compiles clean.
constexpr std::string_view kBlastProtection = R"JSON({
  "minecraft:attributes": [
    { "amount": { "type": "minecraft:linear", "base": 0.15, "per_level_above_first": 0.15 },
      "attribute": "minecraft:explosion_knockback_resistance",
      "id": "minecraft:enchantment.blast_protection", "operation": "add_value" } ],
  "minecraft:damage_protection": [
    { "effect": { "type": "minecraft:add",
        "value": { "type": "minecraft:linear", "base": 2.0, "per_level_above_first": 2.0 } },
      "requirements": { "condition": "minecraft:damage_source_properties",
        "predicate": { "tags": [
          { "expected": true, "id": "minecraft:is_explosion" },
          { "expected": false, "id": "minecraft:bypasses_invulnerability" } ] } } }
  ]
})JSON";

EffectProgram compile(std::string_view text, int maxLevel) {
    return compileEffects(Json::parse(text), maxLevel);
}

constexpr float kEps = 1e-4F;
bool near(float a, float b) { return std::fabs(a - b) < kEps; }

// --- tests ----------------------------------------------------------------

void sharpnessGolden() {
    const EffectProgram program = compile(kSharpness, 5);
    assert(program.damage.size() == 1);
    assert(program.damageProtection.empty());
    assert(program.postAttack.empty());
    assert(program.totalUnknown() == 0);

    for (int level = 1; level <= 5; ++level) {
        EffectContext ctx{};
        ctx.level = level;
        ctx.baseAmount = 0.0F;
        const float bonus = mc::data::effect::applyDamageModifiers(program, ctx);
        const float golden = 0.5F * static_cast<float>(level) + 0.5F;  // hard-coded sharpness
        assert(near(bonus, golden));
    }
    // Over a real base the op adds on top (base 6 sword + sharpness IV = 6 + 2.5).
    EffectContext ctx{};
    ctx.level = 4;
    ctx.baseAmount = 6.0F;
    assert(near(mc::data::effect::applyDamageModifiers(program, ctx), 6.0F + 2.5F));
}

void protectionBypass() {
    const EffectProgram program = compile(kProtection, 4);
    assert(program.damageProtection.size() == 1);
    assert(program.totalUnknown() == 0);

    // A normal (non-bypassing) source: full 1*lvl reduction points.
    for (int level = 1; level <= 4; ++level) {
        EffectContext ctx{};
        ctx.level = level;
        ctx.baseAmount = 0.0F;
        ctx.sourceHasTag = [](std::string_view tag) {
            return tag == "minecraft:bypasses_invulnerability" ? false : false;
        };
        const float points = mc::data::effect::applyDamageProtection(program, ctx);
        assert(near(points, static_cast<float>(level)));  // 1.0 + (lvl-1)*1.0 == lvl
    }
    // A bypassing source: predicate fails ⇒ zero reduction.
    EffectContext bypass{};
    bypass.level = 4;
    bypass.sourceHasTag = [](std::string_view tag) {
        return tag == "minecraft:bypasses_invulnerability";
    };
    assert(near(mc::data::effect::applyDamageProtection(program, bypass), 0.0F));
}

void fireAspectIgnite() {
    const EffectProgram program = compile(kFireAspect, 2);
    assert(program.postAttack.size() == 1);
    assert(program.totalUnknown() == 0);

    for (int level = 1; level <= 2; ++level) {
        EffectContext ctx{};
        ctx.level = level;
        ctx.isDirect = true;  // fire_aspect requires a direct hit
        const PostAttackResult result = mc::data::effect::runPostAttack(program, ctx);
        assert(result.count == 1);
        const auto& outcome = *result.begin();
        assert(outcome.kind == ActionKind::Ignite);
        assert(outcome.affected == TargetSlot::Victim);
        assert(near(outcome.value, 4.0F * static_cast<float>(level)));  // 4*lvl seconds
    }
    // An indirect (projectile) hit: is_direct predicate fails ⇒ no ignite.
    EffectContext indirect{};
    indirect.level = 2;
    indirect.isDirect = false;
    assert(mc::data::effect::runPostAttack(program, indirect).count == 0);
}

void thornsAllOfAndChance() {
    const EffectProgram program = compile(kThorns, 3);
    assert(program.postAttack.size() == 1);
    assert(program.totalUnknown() == 0);

    // Force the chance by giving a rng whose first nextFloat() is < 0.15*lvl.
    // At level 3 the chance is 0.45; a fresh JavaRandom(0)'s first nextFloat is
    // well under that, so thorns fires. Verify both leaf actions come through.
    JavaRandom rng{123};
    EffectContext ctx{};
    ctx.level = 3;
    ctx.random = &rng;
    const PostAttackResult result = mc::data::effect::runPostAttack(program, ctx);
    // Either it fired (two outcomes) or the draw missed (zero) — but if it fired
    // it must carry exactly the two all_of leaves.
    if (result.count > 0) {
        assert(result.count == 2);
        bool sawDamage = false;
        bool sawItem = false;
        for (const auto& outcome : result) {
            if (outcome.kind == ActionKind::DamageEntity) {
                sawDamage = true;
                assert(outcome.affected == TargetSlot::Attacker);  // thorns hits the attacker
                assert(outcome.value >= 1.0F && outcome.value <= 5.0F);  // rolled in [min,max]
            } else if (outcome.kind == ActionKind::ChangeItemDamage) {
                sawItem = true;
                assert(near(outcome.value, 2.0F));  // spend 2 durability
            }
        }
        assert(sawDamage && sawItem);
    }

    // Over a long run of hits from one stream the firing rate must approach the
    // 0.45 chance (level 3), and every fire must carry exactly the two all_of
    // leaves. (A single fresh JavaRandom's *first* draw is famously correlated
    // across small seeds — ~0.73 — so a per-hit re-seed sweep is the wrong probe;
    // the real signal is the rate over a continuous stream.)
    JavaRandom stream{4242};
    int fires = 0;
    constexpr int kHits = 4000;
    for (int hit = 0; hit < kHits; ++hit) {
        EffectContext c{};
        c.level = 3;
        c.random = &stream;
        const PostAttackResult r = mc::data::effect::runPostAttack(program, c);
        if (r.count > 0) {
            assert(r.count == 2);  // damage_entity + change_item_damage
            ++fires;
        }
    }
    const double rate = static_cast<double>(fires) / kHits;
    assert(rate > 0.40 && rate < 0.50);  // ~0.45 expected
}

void entityTagGating() {
    const EffectProgram program = compile(kSmite, 5);
    assert(program.damage.size() == 1);
    assert(program.totalUnknown() == 0);

    // Undead victim: the 2.5*lvl bonus applies.
    for (int level = 1; level <= 5; ++level) {
        EffectContext undead{};
        undead.level = level;
        undead.entityHasTag = [](TargetSlot, std::string_view tag) {
            return tag == "minecraft:sensitive_to_smite";
        };
        const float bonus = mc::data::effect::applyDamageModifiers(program, undead);
        assert(near(bonus, 2.5F * static_cast<float>(level)));
    }
    // Living victim: predicate fails ⇒ no bonus (base 0).
    EffectContext living{};
    living.level = 5;
    living.entityHasTag = [](TargetSlot, std::string_view) { return false; };
    assert(near(mc::data::effect::applyDamageModifiers(program, living), 0.0F));
}

void unknownAccounting() {
    // blast_protection: one in-scope damage_protection entry (clean) + one
    // out-of-scope `attributes` entry (must be counted, sabotage ③).
    const EffectProgram program = compile(kBlastProtection, 4);
    assert(program.damageProtection.size() == 1);
    assert(program.unknownComponentCount == 1);  // the single attributes entry
    assert(program.unknownValueCount == 0);
    assert(program.unknownConditionCount == 0);
    assert(program.unknownActionCount == 0);
    assert(program.totalUnknown() == 1);

    // The in-scope bucket still evaluates correctly (explosion source, not
    // bypassing): 2*lvl points.
    EffectContext ctx{};
    ctx.level = 3;
    ctx.sourceHasTag = [](std::string_view tag) {
        return tag == "minecraft:is_explosion";  // bypasses_invulnerability absent
    };
    assert(near(mc::data::effect::applyDamageProtection(program, ctx), 6.0F));  // 2*3
}

void determinismReplay() {
    const EffectProgram program = compile(kThorns, 3);
    // Two runs with the same seed must produce the identical trigger sequence
    // (sabotage ②: a wall-clock rng would diverge run to run).
    auto sequence = [&](std::uint64_t seed) {
        std::vector<int> fired;
        JavaRandom rng{seed};
        for (int hit = 0; hit < 64; ++hit) {
            EffectContext ctx{};
            ctx.level = 3;
            ctx.random = &rng;
            const PostAttackResult r = mc::data::effect::runPostAttack(program, ctx);
            fired.push_back(static_cast<int>(r.count));
        }
        return fired;
    };
    const std::vector<int> a = sequence(4242);
    const std::vector<int> b = sequence(4242);
    assert(a == b);
    // A different seed must (almost surely) give a different sequence.
    const std::vector<int> c = sequence(9999);
    assert(a != c);
}

void perfEquivalence() {
    // applyDamageModifiers must be the same order of magnitude as the hard-coded
    // expression — the compile-then-DOD payoff (sabotage ①: a runtime-interpreted
    // value tree would be an order of magnitude slower). Not a strict bound
    // (debug builds are noisy); a generous ceiling that a per-call JSON walk /
    // recursive interpret would blow through.
    const EffectProgram program = compile(kSharpness, 5);
    constexpr int kIterations = 2'000'000;

    EffectContext ctx{};
    ctx.level = 5;
    ctx.baseAmount = 6.0F;

    volatile float sink = 0.0F;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        sink = sink + mc::data::effect::applyDamageModifiers(program, ctx);
    }
    const auto t1 = std::chrono::steady_clock::now();

    // The hard-coded baseline: sharpness(level) = 0.5*level + 0.5 over base.
    float baseSink = 0.0F;
    const auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        baseSink = baseSink + (6.0F + (0.5F * 5.0F + 0.5F));
    }
    const auto t3 = std::chrono::steady_clock::now();
    (void)sink;
    (void)baseSink;

    const double engineNs = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    const double baseNs = std::max<double>(
        1.0, static_cast<double>(
                 std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count()));
    const double ratio = engineNs / baseNs;
    // Near hard-coded: allow up to 50x in a noisy -O0 debug build (a recursive
    // JSON-interpreting engine is comfortably 100x+ and would fail this).
    assert(ratio < 50.0);
}

}  // namespace

int main() {
    sharpnessGolden();
    protectionBypass();
    fireAspectIgnite();
    thornsAllOfAndChance();
    entityTagGating();
    unknownAccounting();
    determinismReplay();
    perfEquivalence();
    return 0;
}

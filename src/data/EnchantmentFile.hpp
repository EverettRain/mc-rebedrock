#pragma once

// DDC-1: the JE-compatible enchantment content file format, as D-1 codec
// definitions.
//
// DDC-0 turned the constexpr enchantment table (Enchantment.hpp's
// kEnchantmentTable) into a runtime registry with an External phase and a
// DataStore bridge (EnchantmentRegistry.hpp). This node adds the *reader* for
// that phase: a codec that ingests JE 26.1's `data/minecraft/enchantment/*.json`
// **field for field, unchanged**, so the same files Mojang ships load directly —
// the first of DDC's two hard indicators ("原样加载官方 data/minecraft").
//
// Two things this file deliberately does NOT do, to stay in DDC-1's scope:
//
//  1. It does not *evaluate* `effects`. The 26.1 effect tree (value trees,
//     predicates, component buckets) is DDC-2's compiler. DDC-1 carries the
//     `effects` object through verbatim as raw JSON text (rawEffects), so a
//     later node re-parses and compiles it once at load without DDC-1 having to
//     understand a single component type. Same for `description` — a purely
//     presentational passthrough. Storing them as text (not core::Json) keeps
//     EnchantmentDef copyable and cheap to move through the DataStore.
//
//  2. It does not touch Enchantment.hpp's constexpr accessors or the baked
//     table. Those stay the built-in floor (DDC-3 migrates them later). This is
//     purely the overlay-ingestion half.
//
// The schema mirrors JE exactly (see docs banner in the acceptance test for the
// worked field-by-field correspondence against ENCH-0):
//
//     anvil_cost, weight, max_level : integers
//     min_cost, max_cost           : LevelBasedValue { base, per_level_above_first }
//     slots                        : list of equipment-slot-group strings
//     primary_items, supported_items : an item id or a `#tag` reference (string)
//     exclusive_set                : a `#tag` reference OR an inline id list
//     effects, description         : raw JSON, passed to DDC-2 unevaluated
//
// Data only — identifiers as strings, no gameplay dependency, exactly like
// RecipeFile/LootFile. Resolving a `#tag` / item id to a runtime holder is the
// consumer's job; this header just reads the shape.

#include "core/Json.hpp"
#include "data/Codec.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mc::data {

// JE's LevelBasedValue in its `min_cost` / `max_cost` linear form: the affine
// curve `base + (level - 1) * per_level_above_first`, evaluated per level. This
// is the JSON source of ENCH-0's EnchantmentCostCurve — the numbers Enchantment.
// hpp transcribed by hand from each vanilla subclass now arrive from the file.
// (26.1 `min_cost`/`max_cost` are always this linear shape; a richer value tree
// only appears inside `effects`, which DDC-2 owns.)
struct EnchantmentCostValue final {
    std::int32_t base = 0;
    std::int32_t perLevelAboveFirst = 0;

    // getMinCost/getMaxCost's curve, matching Enchantment.hpp::detail::minPower's
    // base + (level-1)*perLevel exactly, so the JSON-sourced value is byte-for-
    // byte the same integer the constexpr table produced (golden-compared in the
    // acceptance test).
    [[nodiscard]] std::int32_t at(std::int32_t level) const {
        return base + (level - 1) * perLevelAboveFirst;
    }
    [[nodiscard]] bool operator==(const EnchantmentCostValue&) const = default;
};

// An item-set reference as JE spells it: either a single item id
// ("minecraft:diamond_sword") or a tag reference ("#minecraft:enchantable/
// melee_weapon"). Kept as the raw string plus a parsed `isTag` bit so the
// consumer (the enchanting-table candidate generator, a future /enchant) does
// not re-scan for the leading '#'. `exclusive_set` reuses this for its tag-ref
// form and additionally allows an inline list (below).
struct EnchantmentItemSet final {
    std::string value;      // "#minecraft:..." for a tag, "minecraft:..." for one item
    bool isTag = false;     // true iff value began with '#'

    [[nodiscard]] std::string_view tagName() const {
        return isTag ? std::string_view{value}.substr(1) : std::string_view{};
    }
    [[nodiscard]] bool operator==(const EnchantmentItemSet&) const = default;
};

// JE `exclusive_set`: the mutual-exclusion group an enchantment belongs to.
// 26.1 writes it two ways — a tag reference ("#minecraft:exclusive_set/damage",
// the common form) or an inline array of enchantment ids. Both are captured:
// `tag` holds the tag form, `inlineIds` the array form; at most one is
// populated. An absent field leaves both empty (no exclusivity), which is JE's
// default.
struct EnchantmentExclusiveSet final {
    std::string tag;                    // "#minecraft:exclusive_set/..." or empty
    std::vector<std::string> inlineIds; // inline enchantment-id list, or empty

    [[nodiscard]] bool empty() const { return tag.empty() && inlineIds.empty(); }
    [[nodiscard]] std::string_view tagName() const {
        return tag.empty() ? std::string_view{}
                           : std::string_view{tag}.substr(tag.front() == '#' ? 1U : 0U);
    }
    [[nodiscard]] bool operator==(const EnchantmentExclusiveSet&) const = default;
};

// One enchantment definition, JE 26.1 layout. Required fields per the vanilla
// schema: description, supported_items, weight, max_level, min_cost, max_cost,
// anvil_cost, slots. Optional: primary_items, exclusive_set, effects.
struct EnchantmentDef final {
    std::int32_t anvilCost = 0;
    std::int32_t weight = 0;
    std::int32_t maxLevel = 1;
    EnchantmentCostValue minCost{};
    EnchantmentCostValue maxCost{};
    std::vector<std::string> slots;      // "mainhand" / "armor" / "any" / ...
    EnchantmentItemSet supportedItems{}; // required
    EnchantmentItemSet primaryItems{};   // optional; empty ⇒ falls back to supported
    bool hasPrimaryItems = false;
    EnchantmentExclusiveSet exclusiveSet{};
    // Passthrough for DDC-2 — the `effects` object and `description`, verbatim
    // JSON text (empty when the field was absent). DDC-1 never interprets these.
    std::string rawEffects;
    std::string rawDescription;
    // Forward-compatibility diagnostic: how many top-level keys this reader did
    // not recognise (a newer JE datapack introducing a field DDC-1 predates).
    // Skipped, never fatal — the tolerance sabotage ③ verifies. Surfaced for
    // DDC-4's compatibility audit; behaviour never branches on it.
    std::int32_t unknownFieldCount = 0;

    [[nodiscard]] bool operator==(const EnchantmentDef&) const = default;
};

namespace detail {

// The JE top-level keys DDC-1 understands. Any other key on an enchantment
// object counts toward unknownFieldCount (forward compatibility) rather than
// failing the read.
inline constexpr std::string_view kKnownEnchantmentKeys[] = {
    "anvil_cost",  "weight",         "max_level",    "min_cost",
    "max_cost",    "slots",          "supported_items", "primary_items",
    "exclusive_set", "effects",      "description",
};

[[nodiscard]] inline bool isKnownEnchantmentKey(std::string_view key) {
    for (std::string_view known : kKnownEnchantmentKeys) {
        if (key == known) return true;
    }
    return false;
}

} // namespace detail

// LevelBasedValue codec: the `{ "base": N, "per_level_above_first": M }` object.
// Field names are JE's exact spelling — sabotage ① (renaming
// per_level_above_first to perLevel) makes this reject every real JE file.
template <>
struct Codec<EnchantmentCostValue> {
    static core::Json write(const EnchantmentCostValue& value) {
        return ObjectWriter{}
            .field("base", value.base)
            .field("per_level_above_first", value.perLevelAboveFirst)
            .take();
    }
    static bool read(const core::Json& json, EnchantmentCostValue& out) {
        ObjectReader reader{json};
        reader.field("base", out.base)
            .optionalField("per_level_above_first", out.perLevelAboveFirst);
        return reader.ok();
    }
};

// An item-set is a bare string in JE; the leading '#' is what distinguishes a
// tag reference from a single item id.
template <>
struct Codec<EnchantmentItemSet> {
    static core::Json write(const EnchantmentItemSet& set) { return core::Json{set.value}; }
    static bool read(const core::Json& json, EnchantmentItemSet& out) {
        std::string text;
        if (!Codec<std::string>::read(json, text)) return false;
        out.isTag = !text.empty() && text.front() == '#';
        out.value = std::move(text);
        return true;
    }
};

// exclusive_set: a tag string ("#...") OR an inline array of enchantment ids.
// Sabotage ② (dropping the array branch, or not recording the tag) breaks the
// enchanting-table exclusivity table the candidate generator reads.
template <>
struct Codec<EnchantmentExclusiveSet> {
    static core::Json write(const EnchantmentExclusiveSet& set) {
        if (!set.inlineIds.empty()) {
            return Codec<std::vector<std::string>>::write(set.inlineIds);
        }
        return core::Json{set.tag};
    }
    static bool read(const core::Json& json, EnchantmentExclusiveSet& out) {
        out.tag.clear();
        out.inlineIds.clear();
        if (json.isString()) {
            out.tag = json.asString();
            return true;
        }
        if (json.isArray()) {
            return Codec<std::vector<std::string>>::read(json, out.inlineIds);
        }
        return false;
    }
};

template <>
struct Codec<EnchantmentDef> {
    static core::Json write(const EnchantmentDef& def) {
        ObjectWriter writer;
        writer.field("anvil_cost", def.anvilCost)
            .field("weight", def.weight)
            .field("max_level", def.maxLevel)
            .field("min_cost", def.minCost)
            .field("max_cost", def.maxCost)
            .field("slots", def.slots)
            .field("supported_items", def.supportedItems);
        if (def.hasPrimaryItems) {
            writer.field("primary_items", def.primaryItems);
        }
        if (!def.exclusiveSet.empty()) {
            writer.field("exclusive_set", def.exclusiveSet);
        }
        // Re-emit the passthrough blobs so a written-then-read round trip keeps
        // them (they were parsed off the source object as raw sub-values).
        core::Json out = writer.take();
        core::Json::Object members = out.asObject();
        if (!def.rawEffects.empty()) {
            members.emplace_back("effects", core::Json::parse(def.rawEffects));
        }
        if (!def.rawDescription.empty()) {
            members.emplace_back("description", core::Json::parse(def.rawDescription));
        }
        return core::Json{std::move(members)};
    }

    static bool read(const core::Json& json, EnchantmentDef& out) {
        ObjectReader reader{json};
        reader.field("anvil_cost", out.anvilCost)
            .field("weight", out.weight)
            .field("max_level", out.maxLevel)
            .field("min_cost", out.minCost)
            .field("max_cost", out.maxCost)
            .field("slots", out.slots)
            .field("supported_items", out.supportedItems);
        // primary_items / exclusive_set are optional; track presence.
        out.hasPrimaryItems = false;
        if (reader.ok() && json.contains("primary_items")) {
            if (!Codec<EnchantmentItemSet>::read(json["primary_items"], out.primaryItems)) {
                return false;
            }
            out.hasPrimaryItems = true;
        }
        reader.optionalField("exclusive_set", out.exclusiveSet);
        if (!reader.ok()) return false;

        // Passthrough blobs — captured as raw JSON text for DDC-2, not evaluated.
        out.rawEffects = json.contains("effects") ? json["effects"].dump() : std::string{};
        out.rawDescription =
            json.contains("description") ? json["description"].dump() : std::string{};

        // Forward compatibility: count (and otherwise ignore) unrecognised keys.
        out.unknownFieldCount = 0;
        if (json.isObject()) {
            for (const auto& [key, value] : json.asObject()) {
                (void)value;
                if (!detail::isKnownEnchantmentKey(key)) {
                    ++out.unknownFieldCount;
                }
            }
        }
        return true;
    }
};

} // namespace mc::data

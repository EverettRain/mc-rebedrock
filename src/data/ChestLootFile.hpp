#pragma once

// STRUCT-1: the chest loot-table format — pools, weighted entries, and the
// `set_count` function — as a flat POD lowered from the vanilla JE chest JSON.
//
// This is deliberately separate from data/LootFile.hpp. Block loot (D-4) is a
// direct `block -> fixed stacks` table with no rolls (its REGULAR contract). A
// structure chest is the opposite: `rolls` draws, weighted entries, a count
// range. Rather than widen the block table (and drag every block through an
// evaluator it never needs), chest loot gets its own richer form and its own
// evaluator (gameplay/ChestLootTable), and the two live side by side.
//
// Reduction stance mirrors gameplay/JeDataMapping.hpp's jeBlockLoot: the JE JSON
// is read at the load boundary into this POD, and anything this build cannot yet
// represent is skipped at the narrowest scope — an unknown *function* drops off
// its entry, an unknown *entry kind* drops out of its pool — rather than failing
// the whole table. A chest missing one of its many items is acceptable; a chest
// that fails to load is not. Item identifiers stay strings here; resolution to a
// runtime ItemStack is the evaluator's job (as LootTable resolves block loot).
//
// Scope of this pass (decision, 2026-08-26): `set_count` + the `uniform`/constant
// number providers, which cover ~80% of the shipped chest entries. The enchant
// functions (enchant_with_levels/enchant_randomly/set_enchantments) are the ENCH
// down-stream fill; the long tail (set_potion/set_stew_effect/…) is deferred.
// Both are represented as `Unsupported` and skipped, so a table carrying them
// still loads and yields its remaining items.

#include "core/Json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mc::data {

// A loot number: a constant, or a uniform range [min, max]. Mirrors vanilla's
// ConstantValue / UniformGenerator (the two that appear in chest tables). A bare
// JSON number decodes as Constant(min==max==value).
enum class ChestNumberKind : std::uint8_t { Constant, Uniform };
struct ChestNumber final {
    ChestNumberKind kind = ChestNumberKind::Constant;
    float min = 1.0F;
    float max = 1.0F;

    [[nodiscard]] bool operator==(const ChestNumber&) const = default;
};

// A single loot function. Only set_count is modelled this pass; everything else
// is Unsupported and skipped by the evaluator. `add` is set_count's flag: false
// replaces the stack count, true adds to it.
enum class ChestFunctionKind : std::uint8_t { SetCount, Unsupported };
struct ChestFunction final {
    ChestFunctionKind kind = ChestFunctionKind::Unsupported;
    ChestNumber count;
    bool add = false;

    [[nodiscard]] bool operator==(const ChestFunction&) const = default;
};

// A pool entry. `Item` yields the named item; `Empty` yields nothing (a real
// vanilla entry kind — the pool rolled "no item"). Other kinds (loot_table,
// dynamic, tag, alternatives) are not produced: an unsupported kind is dropped
// from the pool at load.
enum class ChestEntryKind : std::uint8_t { Item, Empty };
struct ChestLootEntry final {
    ChestEntryKind kind = ChestEntryKind::Empty;
    std::string name;   // item identifier for Item; empty otherwise
    std::int32_t weight = 1;
    std::vector<ChestFunction> functions;

    [[nodiscard]] bool operator==(const ChestLootEntry&) const = default;
};

struct ChestLootPool final {
    ChestNumber rolls;
    std::vector<ChestLootEntry> entries;

    [[nodiscard]] bool operator==(const ChestLootPool&) const = default;
};

struct ChestLootTableDef final {
    std::string identifier; // e.g. "minecraft:chests/igloo_chest"
    std::vector<ChestLootPool> pools;

    [[nodiscard]] bool operator==(const ChestLootTableDef&) const = default;
};

namespace detail {

inline std::string_view stripNamespace(std::string_view id) {
    const auto colon = id.find(':');
    return colon == std::string_view::npos ? id : id.substr(colon + 1);
}

// A number field: a bare number (constant) or a provider object
// ({type:uniform,min,max} / {type:constant,value}). An unrecognised object shape
// falls back to constant 1 so a pool still rolls once rather than not at all.
inline ChestNumber readNumber(const core::Json& json) {
    ChestNumber number;
    if (json.isNumber()) {
        number.kind = ChestNumberKind::Constant;
        number.min = number.max = static_cast<float>(json.asNumber());
        return number;
    }
    if (json.isObject()) {
        const std::string_view type = stripNamespace(json["type"].asString());
        if (type == "uniform") {
            number.kind = ChestNumberKind::Uniform;
            number.min = static_cast<float>(json["min"].asNumber(1.0));
            number.max = static_cast<float>(json["max"].asNumber(1.0));
            return number;
        }
        if (type == "constant") {
            number.kind = ChestNumberKind::Constant;
            number.min = number.max = static_cast<float>(json["value"].asNumber(1.0));
            return number;
        }
    }
    return number; // default constant 1
}

inline ChestFunction readFunction(const core::Json& json) {
    ChestFunction function;
    if (stripNamespace(json["function"].asString()) == "set_count") {
        function.kind = ChestFunctionKind::SetCount;
        function.count = readNumber(json["count"]);
        function.add = json["add"].asBool(false);
    }
    return function; // Unsupported otherwise
}

// One entry, or nullopt when its kind is not one this build produces.
inline std::optional<ChestLootEntry> readEntry(const core::Json& json) {
    ChestLootEntry entry;
    const std::string_view type = stripNamespace(json["type"].asString());
    if (type == "item") {
        entry.kind = ChestEntryKind::Item;
        entry.name = json["name"].asString();
        if (entry.name.empty()) {
            return std::nullopt;
        }
    } else if (type == "empty") {
        entry.kind = ChestEntryKind::Empty;
    } else {
        return std::nullopt; // loot_table / tag / dynamic / alternatives: skip
    }
    entry.weight = static_cast<std::int32_t>(json["weight"].asNumber(1.0));
    if (entry.weight < 1) {
        entry.weight = 1;
    }
    const core::Json& functions = json["functions"];
    if (functions.isArray()) {
        for (std::size_t index = 0; index < functions.size(); ++index) {
            ChestFunction function = readFunction(functions[index]);
            if (function.kind != ChestFunctionKind::Unsupported) {
                entry.functions.push_back(function);
            }
        }
    }
    return entry;
}

} // namespace detail

// Reduces a vanilla JE chest loot table to a ChestLootTableDef, or nullopt when
// the JSON is not a loot table at all. A table with no usable pools is a valid
// (empty) result — it simply yields nothing. `identifier` is supplied by the
// caller (it comes from the file path, not the JSON body).
[[nodiscard]] inline std::optional<ChestLootTableDef> jeChestLoot(const core::Json& json,
                                                                  std::string identifier) {
    if (!json.isObject()) {
        return std::nullopt;
    }
    ChestLootTableDef def;
    def.identifier = std::move(identifier);
    const core::Json& pools = json["pools"];
    if (!pools.isArray()) {
        return def; // no pools: yields nothing
    }
    for (std::size_t poolIndex = 0; poolIndex < pools.size(); ++poolIndex) {
        const core::Json& poolJson = pools[poolIndex];
        ChestLootPool pool;
        pool.rolls = detail::readNumber(poolJson["rolls"]);
        const core::Json& entries = poolJson["entries"];
        if (entries.isArray()) {
            for (std::size_t entryIndex = 0; entryIndex < entries.size(); ++entryIndex) {
                if (auto entry = detail::readEntry(entries[entryIndex]); entry.has_value()) {
                    pool.entries.push_back(std::move(*entry));
                }
            }
        }
        def.pools.push_back(std::move(pool));
    }
    return def;
}

} // namespace mc::data

#pragma once

// D-5: the data side of Java-Edition interop — ingesting vanilla `minecraft:`
// datapack JSON (recipes, loot, tags) into the rebedrock definitions D-1..D-4
// resolve. This is the feasibility layer, not a wired loader: it converts a JE
// file to a rebedrock Def (or reports that the file needs conversion), so the
// interop milestone can decide between offline conversion and a live compat
// reader from a working, tested basis. The world/save side (block-state palette,
// scheduled ticks, block-entity NBT, Anvil/DataFixer) is deliberately out of
// scope — that is R0-3 + W-2 and a separate milestone.
//
// Findings the samples in je_mapping_test pin down:
//   * Tags ingest DIRECTLY — the JE tag file *is* the rebedrock TagFile format
//     (`{replace, values}` with `#` references), so data::Codec<data::TagFile>
//     reads a vanilla tag with no conversion. BlockTags already loads the 26.1
//     tags this way.
//   * Recipes ingest through a THIN ADAPTER — JE `crafting_shaped` (pattern+key),
//     `crafting_shapeless` and `smelting` map onto CraftingRecipeDef /
//     FurnaceRecipeDef. Gaps: ingredient tags other than `planks` (vanilla uses a
//     per-wood `#oak_logs` etc. this build has no group for) and content this
//     build lacks — those return nullopt.
//   * Loot ingests the DETERMINISTIC subset — a plain item entry, or an
//     `alternatives` whose non-enchant branch is deterministic (stone ->
//     cobblestone, dropping the silk-touch branch). Random rolls, count ranges
//     and Fortune bonuses have no rebedrock evaluator (D-4 keeps loot minimal),
//     so those return nullopt: needs conversion.

#include "core/Json.hpp"
#include "data/LootFile.hpp"
#include "data/RecipeFile.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "world/Block.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mc::gameplay::je {

// The `minecraft:` name a piece of content maps to — the interop anchor. Every
// built-in block and item mirrors vanilla, so this is its own path under the
// vanilla namespace (or the explicit `vanilla` override a renamed block carries).
// je_mapping_test asserts this round-trips for every block and item, which is the
// vanilla-name completeness audit.
[[nodiscard]] inline std::string vanillaName(world::Block block) {
    const auto& definition = world::blockDefinition(block);
    if (!definition.vanilla.empty()) {
        return definition.vanilla.toString();
    }
    return std::string{core::kVanillaNamespace} + ":" + std::string{definition.identifier.path};
}
[[nodiscard]] inline std::string vanillaName(const Item& item) {
    // vanillaAlias for vanilla-backed content, the rebedrock id for original.
    return item.translationIdentifier().toString();
}

namespace detail {

[[nodiscard]] inline std::string_view stripNamespace(std::string_view type) {
    const auto colon = type.find(':');
    return colon == std::string_view::npos ? type : type.substr(colon + 1U);
}

[[nodiscard]] inline bool contentExists(const std::string& id) {
    return world::blockFromIdentifier(id).has_value() || itemFromIdentifier(id) != nullptr;
}

// How the conditions on an entry classify it for deterministic reduction.
//   Plain  - no conditions, or only `survives_explosion` (a no-op for a mine):
//            this is a base drop.
//   Tool   - a `match_tool` gate (silk touch / a tier requirement): the tool-only
//            branch of an alternatives, dropped from the deterministic outcome.
//   Other  - any other condition (a block-state age gate, a random chance, a
//            table bonus): not reducible — the table needs conversion.
enum class ConditionClass : std::uint8_t { Plain, Tool, Other };

[[nodiscard]] inline ConditionClass classifyConditions(const core::Json& entry) {
    const core::Json& conditions = entry["conditions"];
    ConditionClass result = ConditionClass::Plain;
    for (std::size_t i = 0; i < conditions.size(); ++i) {
        const std::string_view type = detail::stripNamespace(conditions[i]["condition"].asString());
        if (type == "survives_explosion") {
            continue;
        }
        if (type == "match_tool") {
            result = ConditionClass::Tool;
            continue;
        }
        return ConditionClass::Other;
    }
    return result;
}

// A functions array carrying anything but `explosion_decay` (which is a no-op for
// a plain mine) — set_count with a range, apply_bonus (Fortune), etc. Those vary
// the count, which the minimal loot model cannot express: needs conversion.
[[nodiscard]] inline bool hasVaryingFunction(const core::Json& entry) {
    const core::Json& functions = entry["functions"];
    for (std::size_t i = 0; i < functions.size(); ++i) {
        if (detail::stripNamespace(functions[i]["function"].asString()) != "explosion_decay") {
            return true;
        }
    }
    return false;
}

// The result stack of a recipe: 26.1 spells it `id`, older packs `item`.
[[nodiscard]] inline bool readResult(const core::Json& result, std::string& id,
                                     std::uint8_t& count) {
    if (result["id"].isString()) {
        id = result["id"].asString();
    } else if (result["item"].isString()) {
        id = result["item"].asString();
    } else {
        return false;
    }
    count = result.contains("count") ? static_cast<std::uint8_t>(result["count"].asNumber()) : 1U;
    return contentExists(id);
}

} // namespace detail

// One JE ingredient (a key value or a shapeless entry): a bare string, or an
// object with `item` or `tag`. A `#tag` maps to the plank group when it is
// `planks`, and is a gap otherwise; a bare id maps to a block ingredient when it
// names a block, else an item. nullopt is a gap (unknown content or tag).
[[nodiscard]] inline std::optional<data::IngredientDef> jeIngredient(const core::Json& json) {
    std::string ref;
    bool isTag = false;
    if (json.isString()) {
        ref = json.asString();
        if (!ref.empty() && ref.front() == '#') {
            isTag = true;
            ref.erase(ref.begin());
        }
    } else if (json.isObject()) {
        if (json["tag"].isString()) {
            ref = json["tag"].asString();
            isTag = true;
        } else if (json["item"].isString()) {
            ref = json["item"].asString();
        } else if (json["id"].isString()) {
            ref = json["id"].asString();
        } else {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    if (ref.empty()) {
        return std::nullopt;
    }
    if (isTag) {
        if (ref == "minecraft:planks" || ref == "planks") {
            return data::IngredientDef{data::IngredientDefKind::Planks, {}};
        }
        return std::nullopt; // a tag group this build has no equivalent for: gap
    }
    if (world::blockFromIdentifier(ref).has_value()) {
        return data::IngredientDef{data::IngredientDefKind::Block, ref};
    }
    if (itemFromIdentifier(ref) != nullptr) {
        return data::IngredientDef{data::IngredientDefKind::Item, ref};
    }
    return std::nullopt; // unknown content: gap
}

// A JE crafting recipe (`crafting_shaped` / `crafting_shapeless`) as a rebedrock
// CraftingRecipeDef, or nullopt when any ingredient/result is a gap or the type
// is not a crafting recipe. `allowMirror` is always false: vanilla shaped recipes
// are not mirrored (rebedrock's own tool recipes add that as an embellishment).
[[nodiscard]] inline std::optional<data::CraftingRecipeDef>
jeCraftingRecipe(const core::Json& json) {
    const std::string_view type = detail::stripNamespace(json["type"].asString());
    data::CraftingRecipeDef def;
    def.allowMirror = false;
    if (type == "crafting_shaped") {
        const core::Json& pattern = json["pattern"];
        if (!pattern.isArray() || pattern.size() == 0U) {
            return std::nullopt;
        }
        const core::Json& key = json["key"];
        std::size_t width = 0;
        for (std::size_t y = 0; y < pattern.size(); ++y) {
            width = std::max(width, pattern[y].asString().size());
        }
        def.shapeless = false;
        def.width = static_cast<std::uint8_t>(width);
        def.height = static_cast<std::uint8_t>(pattern.size());
        for (std::size_t y = 0; y < pattern.size(); ++y) {
            const std::string& row = pattern[y].asString();
            for (std::size_t x = 0; x < width; ++x) {
                const char cell = x < row.size() ? row[x] : ' ';
                if (cell == ' ') {
                    def.ingredients.push_back(data::IngredientDef{});
                    continue;
                }
                auto ingredient = jeIngredient(key[std::string(1, cell)]);
                if (!ingredient.has_value()) {
                    return std::nullopt;
                }
                def.ingredients.push_back(*ingredient);
            }
        }
    } else if (type == "crafting_shapeless") {
        const core::Json& ingredients = json["ingredients"];
        if (!ingredients.isArray()) {
            return std::nullopt;
        }
        def.shapeless = true;
        def.width = 0U;
        def.height = 0U;
        for (std::size_t i = 0; i < ingredients.size(); ++i) {
            auto ingredient = jeIngredient(ingredients[i]);
            if (!ingredient.has_value()) {
                return std::nullopt;
            }
            def.ingredients.push_back(*ingredient);
        }
    } else {
        return std::nullopt;
    }
    if (!detail::readResult(json["result"], def.output, def.count)) {
        return std::nullopt;
    }
    return def;
}

// A JE `smelting` recipe as a FurnaceRecipeDef, or nullopt for a gap or a
// non-smelting type.
[[nodiscard]] inline std::optional<data::FurnaceRecipeDef>
jeSmeltingRecipe(const core::Json& json) {
    if (detail::stripNamespace(json["type"].asString()) != "smelting") {
        return std::nullopt;
    }
    data::FurnaceRecipeDef def;
    auto input = jeIngredient(json["ingredient"]);
    if (!input.has_value()) {
        return std::nullopt;
    }
    def.input = *input;
    if (!detail::readResult(json["result"], def.output, def.count)) {
        return std::nullopt;
    }
    def.cookTicks =
        json.contains("cookingtime") ? static_cast<std::int32_t>(json["cookingtime"].asNumber()) : 200;
    def.experience = json["experience"].asFloat(0.0F);
    return def;
}

namespace detail {

// The deterministic drop of one loot entry, or nullopt when the entry is not
// reducible (needs conversion). An `id`-empty result means "this entry is a
// tool-gated / unknown branch that contributes no base drop", which the caller
// skips without failing.
// A drop with an empty id is a legal "no base drop from this entry" (a tool-only
// branch, or a drop this build lacks) that the caller skips; nullopt means the
// entry is not reducible and the whole table needs conversion.
[[nodiscard]] inline std::optional<data::LootDropDef> jeLootEntry(const core::Json& entry) {
    const std::string_view type = detail::stripNamespace(entry["type"].asString());
    if (type == "item") {
        if (detail::hasVaryingFunction(entry)) {
            return std::nullopt; // a count range / Fortune bonus: no rebedrock evaluator
        }
        switch (detail::classifyConditions(entry)) {
            case detail::ConditionClass::Tool:
                return data::LootDropDef{{}, 0U}; // the silk-touch branch: not the base drop
            case detail::ConditionClass::Other:
                return std::nullopt; // an age / chance gate: not deterministic
            case detail::ConditionClass::Plain:
                break;
        }
        const std::string name = entry["name"].asString();
        if (!detail::contentExists(name)) {
            return data::LootDropDef{{}, 0U}; // a drop this build lacks: skip
        }
        return data::LootDropDef{name, 1U};
    }
    if (type == "alternatives") {
        // A silk/non-silk pair: the deterministic outcome is the branch that is
        // not tool-gated. Any branch with some other gate (an age or chance
        // condition) makes the alternatives irreducible.
        const core::Json& children = entry["children"];
        for (std::size_t i = 0; i < children.size(); ++i) {
            auto drop = jeLootEntry(children[i]);
            if (!drop.has_value()) {
                return std::nullopt; // an irreducible branch: needs conversion
            }
            if (!drop->id.empty()) {
                return drop; // the first plain branch is the base drop
            }
        }
        return std::nullopt; // every branch was tool-gated: no base drop
    }
    return std::nullopt; // an entry kind (loot_table ref, group, tag, ...) we do not reduce
}

} // namespace detail

// A JE block loot table as a rebedrock LootTableDef, or nullopt when it is not
// reducible to a direct drop list (random rolls, a count range, a Fortune bonus,
// or an entry kind with no deterministic outcome). An empty drops list is a valid
// result: the block breaks into nothing.
[[nodiscard]] inline std::optional<data::LootTableDef> jeBlockLoot(const core::Json& json) {
    if (detail::stripNamespace(json["type"].asString()) != "block") {
        return std::nullopt;
    }
    // A table-level function that varies the count (anything but explosion_decay)
    // makes the whole table non-deterministic.
    if (detail::hasVaryingFunction(json)) {
        return std::nullopt;
    }
    data::LootTableDef def;
    const core::Json& pools = json["pools"];
    if (!pools.isArray()) {
        return def; // no pools: drops nothing
    }
    for (std::size_t p = 0; p < pools.size(); ++p) {
        const core::Json& pool = pools[p];
        if (pool["rolls"].asNumber(1.0) != 1.0) {
            return std::nullopt; // a random roll count: needs conversion
        }
        // A pool that only fires under a condition (an age gate, a chance) is a
        // conditional extra drop with no deterministic reduction.
        if (pool["conditions"].size() != 0U || detail::hasVaryingFunction(pool)) {
            return std::nullopt;
        }
        const core::Json& entries = pool["entries"];
        for (std::size_t e = 0; e < entries.size(); ++e) {
            auto drop = detail::jeLootEntry(entries[e]);
            if (!drop.has_value()) {
                return std::nullopt;
            }
            if (!drop->id.empty()) {
                def.drops.push_back(*drop);
            }
        }
    }
    return def;
}

} // namespace mc::gameplay::je

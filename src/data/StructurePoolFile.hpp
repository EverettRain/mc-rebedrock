#pragma once

// STRUCT-3a: the jigsaw template pool, lowered from a vanilla
// `worldgen/template_pool/*.json` into a flat POD. A jigsaw structure (village,
// pillager outpost, …) is a graph of templates connected at jigsaw blocks; a pool
// is the weighted set of templates a given connection may draw from. This is the
// data half — the expansion algorithm (STRUCT-3b) walks these pools and the
// jigsaw blocks the NBT reader lifts from each template.
//
// Reduction stance mirrors jeChestLoot/jeBlockLoot: read at the load boundary into
// a flat structure-of-arrays-friendly POD, skip what this build cannot represent
// (a `list`/`feature` element kind is dropped rather than failing the pool), keep
// identifiers as strings for the runtime to resolve. The Java side is a
// `StructureTemplatePool` of `Holder`/`Either` pool elements + a
// `SimpleWeightedRandomList`; none of that indirection crosses into the engine.

#include "core/Json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mc::data {

// How a piece meets the terrain: `rigid` keeps the template's own Y, `terrain_matching`
// drapes it onto the surface. STRUCT-3b honours it at placement.
enum class PoolProjection : std::uint8_t { Rigid, TerrainMatching };

// One weighted pool element that names a single template. Only the single-template
// element kinds (`single_pool_element` / `legacy_single_pool_element`) are modelled;
// `list`/`feature`/`empty` are not produced (empty is the fallback, tracked on the
// pool itself).
struct StructurePoolElement final {
    std::string location;     // template id, e.g. "minecraft:village/plains/houses/plains_small_house_1"
    std::int32_t weight = 1;
    PoolProjection projection = PoolProjection::Rigid;
    std::string processors;   // processor-list reference id, or empty for an inline/empty list

    [[nodiscard]] bool operator==(const StructurePoolElement&) const = default;
};

struct StructurePoolDef final {
    std::string id;        // e.g. "minecraft:village/plains/town_centers"
    std::string fallback;  // fallback pool id (usually "minecraft:empty")
    std::vector<StructurePoolElement> elements;

    [[nodiscard]] bool operator==(const StructurePoolDef&) const = default;
};

namespace detail {

inline PoolProjection projectionFromString(std::string_view value) {
    return value == "terrain_matching" ? PoolProjection::TerrainMatching : PoolProjection::Rigid;
}

// `processors` is either a string reference ("minecraft:mossify_20_percent") or an
// inline object ({"processors":[...]}); the reference is kept, an inline list is
// left empty (STRUCT-5's processor engine handles both, and an empty ref means
// "no named list").
inline std::string processorsRef(const core::Json& json) {
    return json.isString() ? json.asString() : std::string{};
}

} // namespace detail

// Reduces a vanilla template pool to a StructurePoolDef, or nullopt when the JSON
// is not a pool. Elements this build does not model are skipped; a pool with none
// left is still valid (it just yields nothing but its fallback).
[[nodiscard]] inline std::optional<StructurePoolDef> jeStructurePool(const core::Json& json,
                                                                     std::string id) {
    if (!json.isObject()) {
        return std::nullopt;
    }
    StructurePoolDef def;
    def.id = std::move(id);
    def.fallback = json["fallback"].asString();
    const core::Json& elements = json["elements"];
    if (!elements.isArray()) {
        return def;
    }
    for (std::size_t index = 0; index < elements.size(); ++index) {
        const core::Json& entry = elements[index];
        const core::Json& element = entry["element"];
        const std::string_view type = [&] {
            std::string_view t = element["element_type"].asString();
            const auto colon = t.find(':');
            return colon == std::string_view::npos ? t : t.substr(colon + 1);
        }();
        if (type != "single_pool_element" && type != "legacy_single_pool_element") {
            continue; // list/feature/empty: not modelled (empty is the fallback)
        }
        StructurePoolElement out;
        out.location = element["location"].asString();
        if (out.location.empty()) {
            continue;
        }
        out.weight = static_cast<std::int32_t>(entry["weight"].asNumber(1.0));
        if (out.weight < 1) {
            out.weight = 1;
        }
        out.projection = detail::projectionFromString(element["projection"].asString());
        out.processors = detail::processorsRef(element["processors"]);
        def.elements.push_back(std::move(out));
    }
    return def;
}

} // namespace mc::data

#pragma once

// The reference definition D-1's codec + baking infra is exercised against.
//
// D-1 is infrastructure: the real definitions (recipes, loot, tags) arrive in
// D-3/D-4/D-2, which instantiate Codec<> and DataStore<> for their own PODs.
// Until then this DemoRecord is the conformance fixture — it carries one field of
// every primitive the codec supports (int, string, list, bool, optional) so a
// single round-trip covers them all — and it is the payload the baking generator
// and the overlay loader run end to end. It lives under tests/, never in the
// shipped runtime library: it is scaffolding for the infra, not game content.

#include "core/Identifier.hpp"
#include "core/Json.hpp"
#include "data/Codec.hpp"
#include "data/DataStore.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mc::data::demo {

// The runtime, owning form: what the codec reads to and the DataStore holds.
struct DemoRecord final {
    std::int32_t weight = 0;
    std::string label;
    std::vector<std::int32_t> steps;
    bool enabled = false;
    std::optional<double> scale;

    [[nodiscard]] bool operator==(const DemoRecord&) const = default;
};

// A phantom-tagged id so the R0 registration hook has a concrete registry to
// register demo additions into.
using DemoId = core::ContentId<struct DemoRecordTag>;

inline constexpr std::string_view kDemoPrefix{"demo"};

} // namespace mc::data::demo

namespace mc::data {

template <>
struct Codec<demo::DemoRecord> {
    static core::Json write(const demo::DemoRecord& record) {
        return ObjectWriter{}
            .field("weight", record.weight)
            .field("label", record.label)
            .field("steps", record.steps)
            .field("enabled", record.enabled)
            .field("scale", record.scale)
            .take();
    }
    static bool read(const core::Json& json, demo::DemoRecord& out) {
        ObjectReader reader{json};
        reader.field("weight", out.weight)
            .field("label", out.label)
            .field("steps", out.steps)
            .field("enabled", out.enabled)
            .optionalField("scale", out.scale);
        return reader.ok();
    }
};

} // namespace mc::data

namespace mc::data::demo {

// The baked, constexpr-friendly form: string_view and span instead of string and
// vector, so the whole table is a compile-time aggregate in `.rodata` and laying
// the floor parses nothing. The generator emits an array of these; see
// DemoBakedData.inc.
struct BakedDemoRecord final {
    std::string_view name;
    std::int32_t weight;
    std::string_view label;
    std::span<const std::int32_t> steps;
    bool enabled;
    bool hasScale;
    double scale;
};

// Converts one baked record into the owning runtime form. Allocates the string
// and vector, but performs no JSON parse — the point of baking.
[[nodiscard]] inline DemoRecord toRecord(const BakedDemoRecord& baked) {
    DemoRecord record;
    record.weight = baked.weight;
    record.label = std::string{baked.label};
    record.steps.assign(baked.steps.begin(), baked.steps.end());
    record.enabled = baked.enabled;
    record.scale = baked.hasScale ? std::optional<double>{baked.scale} : std::nullopt;
    return record;
}

// Lays a baked table down as a DataStore's built-in floor. No parsing happens.
inline void bakeInto(DataStore<DemoRecord>& store, std::span<const BakedDemoRecord> baked) {
    for (const auto& record : baked) {
        store.bakeBuiltin(std::string{record.name}, toRecord(record));
    }
}

} // namespace mc::data::demo

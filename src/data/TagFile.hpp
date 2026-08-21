#pragma once

// The datapack tag-file format, defined once as a D-1 codec.
//
// A tag file is `{ "replace": <bool>, "values": [ <entry>, ... ] }`, where an
// entry is either a bare identifier string or the object form
// `{ "id": "...", "required": <bool> }` the format allows for an entry a pack
// tolerates being absent. `#`-prefixed values are references to other tags,
// expanded by the loader (BlockTags), not here — this codec only reads the file
// shape; the recursion, the low-to-high merge and the `replace` truncation are
// tag policy that stays with the per-id bitset in R0-5.
//
// This replaces the hand-rolled `root["values"]` / `entryIdentifier` walking
// BlockTags used to do inline: the file format is now one type-safe codec, so
// item and entity tags (later) read the same shape without a second reader.

#include "data/Codec.hpp"

#include <string>
#include <vector>

namespace mc::data {

// One `values` entry: the referenced identifier, and whether a pack requires it
// to resolve. `required` is carried for fidelity but membership never depends on
// it — an identifier this build has no content for is skipped either way, which
// is exactly what `required: false` asks for and what a vanilla tag naming
// hundreds of absent blocks needs regardless.
struct TagEntry final {
    std::string id;
    bool required = true;

    [[nodiscard]] bool operator==(const TagEntry&) const = default;
};

struct TagFile final {
    bool replace = false;
    std::vector<TagEntry> values;

    [[nodiscard]] bool operator==(const TagFile&) const = default;
};

template <>
struct Codec<TagEntry> {
    static core::Json write(const TagEntry& entry) {
        // The bare-string form when required (the common case), the object form
        // only when a pack marked the entry optional — so a round-trip keeps the
        // compact spelling vanilla files overwhelmingly use.
        if (entry.required) {
            return core::Json{entry.id};
        }
        return ObjectWriter{}.field("id", entry.id).field("required", entry.required).take();
    }
    static bool read(const core::Json& json, TagEntry& out) {
        if (json.isString()) {
            out.id = json.asString();
            out.required = true;
            return true;
        }
        if (json.isObject()) {
            ObjectReader reader{json};
            out.required = true; // default when the object omits it
            reader.field("id", out.id).optionalField("required", out.required);
            return reader.ok();
        }
        return false;
    }
};

template <>
struct Codec<TagFile> {
    static core::Json write(const TagFile& tag) {
        return ObjectWriter{}.field("replace", tag.replace).field("values", tag.values).take();
    }
    static bool read(const core::Json& json, TagFile& out) {
        ObjectReader reader{json};
        // Both are optional: a file may omit `replace` (defaults false) and, in
        // the degenerate case, `values` (an empty tag). A present-but-mistyped
        // field still fails the read, which the loader treats as "supplied no
        // members", the same as vanilla's tolerance of a values-less file.
        reader.optionalField("replace", out.replace).optionalField("values", out.values);
        return reader.ok();
    }
};

} // namespace mc::data

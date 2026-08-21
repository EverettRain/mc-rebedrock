#pragma once

// The block loot-table file format, as D-1 codec definitions.
//
// D-4's rule is a minimal evaluator: a block's loot is a *direct table* of the
// items it drops, nothing more. There is deliberately no chance, no count range,
// no Fortune/Silk-Touch/enchantment condition and no `explosion_decay` — this
// build has none of the systems those would feed, so a richer format would only
// re-encode the same deterministic result behind an evaluator that cannot yet
// vary. The handful of blocks whose vanilla loot really is random (leaves and
// crops) keep their procedural handler in MiningSystem; they are not this table.
//
// A LootDrop is one guaranteed output; an empty drop list is a block that breaks
// into nothing (glass without silk touch). Identifiers as strings, no Item*/Block
// — resolution to the runtime stack is gameplay's job, in LootTable.

#include "data/Codec.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mc::data {

struct LootDropDef final {
    std::string id;            // the dropped item/block identifier
    std::uint8_t count = 1U;

    [[nodiscard]] bool operator==(const LootDropDef&) const = default;
};

struct LootTableDef final {
    std::string identifier;    // the block this table drops for (optional in a file)
    std::vector<LootDropDef> drops;

    [[nodiscard]] bool operator==(const LootTableDef&) const = default;
};

template <>
struct Codec<LootDropDef> {
    static core::Json write(const LootDropDef& drop) {
        return ObjectWriter{}.field("id", drop.id).field("count", drop.count).take();
    }
    static bool read(const core::Json& json, LootDropDef& out) {
        ObjectReader reader{json};
        reader.field("id", out.id).optionalField("count", out.count);
        return reader.ok();
    }
};

template <>
struct Codec<LootTableDef> {
    static core::Json write(const LootTableDef& table) {
        return ObjectWriter{}
            .field("identifier", table.identifier)
            .field("drops", table.drops)
            .take();
    }
    static bool read(const core::Json& json, LootTableDef& out) {
        ObjectReader reader{json};
        // `drops` is optional so a file can state an empty table (drops nothing)
        // by omitting it as well as by an empty array.
        reader.optionalField("identifier", out.identifier).optionalField("drops", out.drops);
        return reader.ok();
    }
};

} // namespace mc::data

namespace mc::data::loot {

// The baked, constexpr-friendly forms: string_view and span, so the built-in
// loot floor is a compile-time table in `.rodata` resolved with no parsing.
struct BakedLootDrop final {
    std::string_view id;
    std::uint8_t count;
};

struct BakedLootTable final {
    std::string_view block; // the block identifier this table drops for
    std::span<const BakedLootDrop> drops;
};

[[nodiscard]] inline LootTableDef toDef(const BakedLootTable& baked) {
    LootTableDef def;
    def.identifier = std::string{baked.block};
    def.drops.reserve(baked.drops.size());
    for (const auto& drop : baked.drops) {
        def.drops.push_back(LootDropDef{std::string{drop.id}, drop.count});
    }
    return def;
}

} // namespace mc::data::loot

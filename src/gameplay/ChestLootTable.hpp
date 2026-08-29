#pragma once

// STRUCT-1: the process-level chest loot table and its evaluator.
//
// This is the structure-chest counterpart to gameplay/LootTable (block drops).
// The two are deliberately separate: block loot is a direct, roll-free lookup by
// BlockId (D-4's contract); a chest table is keyed by its loot-table identifier
// (e.g. "minecraft:chests/igloo_chest" — the string a structure block entity or a
// metadata marker names) and *rolls*: pools draw a count, entries are picked by
// weight, `set_count` sizes the stack. Reading and reduction of the vanilla JE
// JSON is data/ChestLootFile.hpp's jeChestLoot; this owns the loaded tables and
// turns one into a list of ItemStacks.
//
// There is no baked floor here: chest content is datapack content loaded at
// runtime (the STRUCT decision), so an installation with no `data/` simply has no
// chest tables and structures place empty chests — never a crash. The evaluator
// is deterministic in its `mc::rng` state: the same seed yields the same stacks,
// which is all STRUCT needs (a chest seeded from its world position rolls the same
// on every visit). It does not reproduce Java's exact draw order bit-for-bit —
// there is no runnable vanilla to parity-check chest contents against, and no
// user-facing expectation that a given seed's chest matches Java's.

#include "assets/ResourceProvider.hpp"
#include "data/ChestLootFile.hpp"
#include "gameplay/Inventory.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::gameplay {

class ChestLootTable final {
  public:
    // Chests have no built-in floor (content is runtime); provided for symmetry
    // with LootTable and to reset the table. A no-`data/` build stays empty.
    void loadBuiltinDefaults();

    // Loads every `loot_tables/chests/<name>.json` through the provider stack,
    // reducing each with jeChestLoot and keying it by its loot-table identifier.
    void load(const assets::ResourceProvider& resources);

    // The table for `identifier` (e.g. "minecraft:chests/igloo_chest"), or nullptr
    // when this build has no such table (an empty chest is placed).
    [[nodiscard]] const data::ChestLootTableDef* find(std::string_view identifier) const;

    // Rolls a table into the stacks it yields: for each pool, `rolls` draws, each
    // a weight-picked entry, `set_count` sizing the stack. `state` is advanced;
    // the same starting state gives the same result. An entry naming an item this
    // build lacks is skipped after its rng draws, so content gaps do not shift the
    // sequence. STRUCT-2 distributes the returned stacks into container slots.
    [[nodiscard]] std::vector<ItemStack> roll(const data::ChestLootTableDef& table,
                                              std::uint64_t& state) const;

    // Convenience: roll by identifier. An unknown identifier yields no stacks.
    [[nodiscard]] std::vector<ItemStack> roll(std::string_view identifier,
                                              std::uint64_t& state) const;

    // Rolls `table` and scatters the stacks into the empty cells of `slots`, the
    // way vanilla LootTable.fill shuffles a chest's contents rather than packing
    // them from slot 0. Non-empty slots are left alone; surplus stacks past the
    // free-slot count are dropped. This is how a structure chest is filled at
    // creation (deterministic from its seed), so no loot state has to persist.
    void fillSlots(std::span<ItemStack> slots, const data::ChestLootTableDef& table,
                   std::uint64_t& state) const;

    [[nodiscard]] std::size_t size() const { return tables_.size(); }

  private:
    void applyOverlay(const assets::ResourceProvider& resources);

    std::unordered_map<std::string, data::ChestLootTableDef> tables_;
};

// The process-wide chest loot table. Empty until `load` runs against the pack
// stack, the way LootTable's overlay is applied once the packs are up.
[[nodiscard]] ChestLootTable& chestLootTable();

} // namespace mc::gameplay

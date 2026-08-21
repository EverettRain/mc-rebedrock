#pragma once

// The process-level block loot table: the baked built-in floor plus a datapack
// overlay, the two-layer shape BlockTags and recipes use. D-4 moved the
// deterministic block drops out of MiningSystem's hand-written handlers into
// this data table; a block's `getDrops` slot reads it. The random-loot blocks
// (leaves, gravel, crops) are not here — they keep their procedural handler, per
// D-4's rule that the evaluator stays a direct block -> drops lookup with no
// chance/Fortune machinery.
//
// A block with an entry drops exactly that entry's stacks (an empty entry drops
// nothing, e.g. glass). A block with no entry is not in the table at all and
// falls back to dropping itself — so the vast majority of blocks need no entry.

#include "assets/ResourceProvider.hpp"
#include "gameplay/Inventory.hpp"
#include "world/Block.hpp"

#include <optional>
#include <vector>

namespace mc::gameplay {

// One block's resolved loot: the exact stacks it drops. Deterministic — no rolls.
struct LootEntry final {
    std::vector<ItemStack> stacks;
};

class LootTable final {
  public:
    // Resolves the baked constexpr floor. No parsing; the whole table for a
    // headless caller or a pack that ships only `assets/`.
    void loadBuiltinDefaults();

    // The floor, then a datapack overlay: a `loot_tables/blocks/<name>.json`
    // whose block matches an entry replaces it, a new block's file adds one.
    void load(const assets::ResourceProvider& resources);

    // The loot entry for `block`, or nullptr when the block has none (drops
    // itself). A returned entry may hold no stacks — that is "drops nothing".
    [[nodiscard]] const LootEntry* find(world::Block block) const;

  private:
    void applyOverlay(const assets::ResourceProvider& resources);
    void set(world::Block block, LootEntry entry);

    // Indexed by BlockId::index(); nullopt means "no entry, drops itself".
    std::vector<std::optional<LootEntry>> entries_;
};

// The table MiningSystem's drop handler reads. Built-in defaults on first use,
// so drops work with no wiring; Application loads the overlay once the pack
// stack is up, exactly as it does for tags and recipes.
[[nodiscard]] LootTable& lootTable();

} // namespace mc::gameplay

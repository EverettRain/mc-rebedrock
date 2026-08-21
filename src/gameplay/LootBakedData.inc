// The baked block-loot floor: the blocks whose drop differs from "itself".
//
// Derived from the deterministic drop handlers that used to live in
// MiningSystem (stone -> cobblestone, the ores -> their item, glass -> nothing,
// and so on). A block absent from this table drops itself when it dropsItem —
// that default stays in code, so the overwhelming majority of blocks need no
// entry. The random-loot blocks (leaves, gravel, crops) are not here; they keep
// their procedural handler. Constexpr, in `.rodata`, resolved with no parse.
//
// These exact drops are pinned independently by block_behavior_test's
// testDropsAbsoluteValues, so an error here trips that test.
#pragma once

namespace mc::data::loot {

inline constexpr BakedLootDrop kDrop_cobblestone[] = {{"rebedrock:cobblestone", 1}};
inline constexpr BakedLootDrop kDrop_dirt[] = {{"rebedrock:dirt", 1}};
inline constexpr BakedLootDrop kDrop_coal[] = {{"rebedrock:coal", 1}};
inline constexpr BakedLootDrop kDrop_diamond[] = {{"rebedrock:diamond", 1}};
inline constexpr BakedLootDrop kDrop_emerald[] = {{"rebedrock:emerald", 1}};
inline constexpr BakedLootDrop kDrop_book[] = {{"rebedrock:book", 3}};
inline constexpr BakedLootDrop kDrop_torch[] = {{"rebedrock:torch", 1}};

inline constexpr BakedLootTable kBakedLootTables[] = {
    {"rebedrock:stone", kDrop_cobblestone},
    {"rebedrock:grass_block", kDrop_dirt},
    {"rebedrock:podzol", kDrop_dirt},
    {"rebedrock:farmland", kDrop_dirt},
    {"rebedrock:coal_ore", kDrop_coal},
    {"rebedrock:diamond_ore", kDrop_diamond},
    {"rebedrock:emerald_ore", kDrop_emerald},
    {"rebedrock:bookshelf", kDrop_book},
    {"rebedrock:wall_torch", kDrop_torch},
    {"rebedrock:glass", {}}, // silk-touch only: breaks into nothing
};

} // namespace mc::data::loot

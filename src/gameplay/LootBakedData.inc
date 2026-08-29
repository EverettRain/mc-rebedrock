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
inline constexpr BakedLootDrop kDrop_cobbled_deepslate[] = {{"rebedrock:cobbled_deepslate", 1}};
inline constexpr BakedLootDrop kDrop_dirt[] = {{"rebedrock:dirt", 1}};
inline constexpr BakedLootDrop kDrop_coal[] = {{"rebedrock:coal", 1}};
inline constexpr BakedLootDrop kDrop_diamond[] = {{"rebedrock:diamond", 1}};
inline constexpr BakedLootDrop kDrop_emerald[] = {{"rebedrock:emerald", 1}};
// The ore drops (26.1). iron/gold/copper mine to a raw ore item; lapis/redstone/
// quartz drop their material. rebedrock's loot floor is deterministic (no roll),
// so the ores vanilla gives a range drop a fixed typical count: copper 3 (2-5),
// redstone 4 (4-5), lapis 5 (4-9). The deepslate ore variants share these drops.
inline constexpr BakedLootDrop kDrop_raw_iron[] = {{"rebedrock:raw_iron", 1}};
inline constexpr BakedLootDrop kDrop_raw_copper[] = {{"rebedrock:raw_copper", 3}};
inline constexpr BakedLootDrop kDrop_raw_gold[] = {{"rebedrock:raw_gold", 1}};
inline constexpr BakedLootDrop kDrop_redstone[] = {{"rebedrock:redstone", 4}};
inline constexpr BakedLootDrop kDrop_lapis[] = {{"rebedrock:lapis_lazuli", 5}};
inline constexpr BakedLootDrop kDrop_quartz[] = {{"rebedrock:quartz", 1}};
inline constexpr BakedLootDrop kDrop_book[] = {{"rebedrock:book", 3}};
inline constexpr BakedLootDrop kDrop_torch[] = {{"rebedrock:torch", 1}};
inline constexpr BakedLootDrop kDrop_redstone_torch[] = {{"rebedrock:redstone_torch", 1}};

inline constexpr BakedLootTable kBakedLootTables[] = {
    {"rebedrock:stone", kDrop_cobblestone},
    {"rebedrock:deepslate", kDrop_cobbled_deepslate}, // like stone -> cobblestone
    {"rebedrock:grass_block", kDrop_dirt},
    {"rebedrock:podzol", kDrop_dirt},
    {"rebedrock:farmland", kDrop_dirt},
    // Ores -> their 26.1 item (the deepslate variants drop the same thing).
    {"rebedrock:coal_ore", kDrop_coal},
    {"rebedrock:iron_ore", kDrop_raw_iron},
    {"rebedrock:copper_ore", kDrop_raw_copper},
    {"rebedrock:gold_ore", kDrop_raw_gold},
    {"rebedrock:redstone_ore", kDrop_redstone},
    {"rebedrock:lapis_ore", kDrop_lapis},
    {"rebedrock:diamond_ore", kDrop_diamond},
    {"rebedrock:emerald_ore", kDrop_emerald},
    {"rebedrock:nether_quartz_ore", kDrop_quartz},
    {"rebedrock:deepslate_coal_ore", kDrop_coal},
    {"rebedrock:deepslate_iron_ore", kDrop_raw_iron},
    {"rebedrock:deepslate_copper_ore", kDrop_raw_copper},
    {"rebedrock:deepslate_gold_ore", kDrop_raw_gold},
    {"rebedrock:deepslate_redstone_ore", kDrop_redstone},
    {"rebedrock:deepslate_lapis_ore", kDrop_lapis},
    {"rebedrock:deepslate_diamond_ore", kDrop_diamond},
    {"rebedrock:deepslate_emerald_ore", kDrop_emerald},
    {"rebedrock:bookshelf", kDrop_book},
    {"rebedrock:wall_torch", kDrop_torch},
    // The redstone wall torch, like the wall torch, comes back as its standing
    // item (there is no wall-variant item to hold).
    {"rebedrock:redstone_wall_torch", kDrop_redstone_torch},
    {"rebedrock:glass", {}}, // silk-touch only: breaks into nothing
};

} // namespace mc::data::loot

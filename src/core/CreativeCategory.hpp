#pragma once

// The creative-inventory tab an item or block is filed under.
//
// Lives in core/ (rather than gameplay/Item.hpp, where it used to live) so that
// both world/Block.hpp and gameplay/Item.hpp can declare a block or item's tab
// membership without a circular include: Item.hpp already includes
// world/Block.hpp (a BlockItem wraps a block's registry entry), so Block.hpp
// cannot include Item.hpp back. core/ is beneath both, exactly like ContentId.
//
// AR-CI: `Redstone` is a bounded addition mirroring vanilla 26.1's distinct
// REDSTONE_BLOCKS tab (CreativeModeTabs.java) — the redstone component family
// (wire, torch, repeater, comparator, observer, piston family, lever, button,
// pressure plate, redstone block) is large and coherent enough to earn its own
// tab without reworking the other six. Full alignment to 26.1's complete tab
// taxonomy (Colored Blocks / Natural Blocks / Ingredients / Operator
// Utilities / ...) is deferred — see the AR-CI report for the debt note.
//
// EQ-0: `Combat` is the same bounded addition for vanilla 26.1's distinct
// COMBAT tab (armor, swords, and other player-defense/offense gear) — armor
// is new content this node adds and none of the existing seven tabs fit it,
// so it earns its own tab the same way Redstone did rather than forcing a
// taxonomy rework.
//
// `Hidden` is a sentinel, not a real tab: it is every block's default (a block
// that never calls `.creative(...)` is technical/unobtainable and stays out of
// every catalog) and is declared *after* `Count` so it can never be used to
// index a `Count`-sized catalog array — ContentRegistry::registerBlock checks
// for it explicitly and skips registration instead of storing it.

#include <cstdint>

namespace mc::core {

enum class CreativeCategory : std::uint8_t {
    BuildingBlocks,
    Decoration,
    Functional,
    Materials,
    Food,
    Tools,
    SpawnEggs,
    Redstone,
    Combat,
    Count,
    // Sentinel: deliberately not in [0, Count) so it can never alias a real
    // catalog slot. Every block defaults to this; declaring a tab via
    // BlockProperties::creative() is what makes a block reachable in creative.
    Hidden,
};

} // namespace mc::core

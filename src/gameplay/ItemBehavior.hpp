#pragma once

// The item behaviour table — the item-side counterpart of BlockBehavior.hpp, and
// the same DOD dispatch shape. Where blocks resolve behaviour through a table
// indexed by BlockId with a pre-filter bitset, items resolve theirs through a
// table indexed by ItemId. This retires the hand-rolled `if (item == &Bucket)
// … else if (toolType == Hoe) …` chain itemUseOn used to walk on every
// right-click: each item's useOn handler is classified once, at table build, and
// dispatch is one indexed load plus a pre-filter bit — no scan, no per-click
// identity comparisons, and one behaviour-mount model shared with blocks rather
// than "blocks go through a table, items through a pointer chain".
//
// The table is built and the slots are wired in ItemPlacement.cpp, where the
// concrete useOn handlers live; this header declares the shape and the accessors
// so tests and callers can reach them.

#include "core/ContentId.hpp"
#include "gameplay/ItemUse.hpp" // ItemUseFn

#include <cstdint>
#include <vector>

namespace mc::gameplay {

// The pre-filter bits, one per behaviour class the dispatch wants to reject
// without touching a slot. Only useOn exists today; the enum leaves room for a
// `HasUse` (right-click in air: eating, throwing) the moment an item needs one,
// so adding a bit never reshuffles the indices a frozen table depends on.
enum class ItemBehaviorBit : std::uint8_t {
    // Right-clicking a block with this item does something item-side (place,
    // bucket collect/pour, plant a seed, till with a hoe, spawn from an egg).
    HasUseOn = 0,
};

// A packed set of the bits above, mirroring BlockBehaviorPrefilter.
struct ItemBehaviorPrefilter final {
    std::uint8_t bits = 0U;

    [[nodiscard]] constexpr bool has(ItemBehaviorBit bit) const {
        return (bits & maskOf(bit)) != 0U;
    }
    constexpr void set(ItemBehaviorBit bit, bool on) {
        const auto mask = maskOf(bit);
        bits = on ? static_cast<std::uint8_t>(bits | mask)
                  : static_cast<std::uint8_t>(bits & static_cast<std::uint8_t>(~mask));
    }
    [[nodiscard]] constexpr bool operator==(const ItemBehaviorPrefilter&) const = default;

  private:
    [[nodiscard]] static constexpr std::uint8_t maskOf(ItemBehaviorBit bit) {
        return static_cast<std::uint8_t>(1U << static_cast<unsigned>(bit));
    }
};

// One item's behaviour: the slot pointer plus the pre-filter. An entry with a
// null slot and a clear bit is an item that does nothing on right-click, which
// dispatch costs nothing for.
struct ItemBehavior final {
    // Item#useOn, resolved once at table build from the item's class/identity.
    ItemUseFn useOn = nullptr;
    ItemBehaviorPrefilter prefilter{};
};

// The process-wide behaviour table, built once on first use from the frozen item
// registry and indexed by ItemId. Header-only lazy singleton, like
// blockBehaviorTable(); defined in ItemPlacement.cpp.
[[nodiscard]] const std::vector<ItemBehavior>& itemBehaviorTable();

// The behaviour of one item. An invalid or out-of-range id reads as the empty
// behaviour rather than out of bounds — the same defensive guard behaviorFor
// gives blocks.
[[nodiscard]] const ItemBehavior& itemBehaviorFor(core::ItemId id);

// The pre-filter query: one masked load, no slot touched.
[[nodiscard]] bool itemHasBehavior(core::ItemId id, ItemBehaviorBit bit);

} // namespace mc::gameplay

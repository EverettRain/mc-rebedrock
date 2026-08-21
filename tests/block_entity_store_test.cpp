// BE1 storage generalization: BlockEntityStore keyed on the unified world::BlockPos.
//
// Before BE1 each block-entity system minted its own ChestPosition /
// FurnacePosition {x,y,z}; BE1 folds them onto one world::BlockPos so the store
// keys on a single type. This pins that the store's four operations — find,
// place-if-absent, remove-and-return, find-or-create — address a cell by its full
// three coordinates, so a swap of two components lands on the wrong cell (or
// misses) and is caught here rather than as a chest that opens someone else's
// inventory.

#include "gameplay/BlockEntityStore.hpp"
#include "world/BlockPos.hpp"

#include <cassert>
#include <cstdio>

namespace {

// The minimal shape BlockEntityStore requires: a `position` member of the key
// type, plus whatever payload the real block entities carry (a tag here, so a
// find returns something we can tell apart).
struct TestBlockEntity final {
    mc::world::BlockPos position{};
    int tag = 0;
};

} // namespace

int main() {
    using mc::world::BlockPos;
    mc::gameplay::BlockEntityStore<BlockPos, TestBlockEntity> store;

    // A cell whose three coordinates are all different, so a swap of any two is a
    // different cell: the discriminating case for "keyed on the full position".
    const BlockPos a{1, 2, 3};
    const BlockPos swappedXY{2, 1, 3};
    const BlockPos swappedYZ{1, 3, 2};
    const BlockPos swappedXZ{3, 2, 1};
    // Cells that differ from `a` in exactly one axis, so dropping *that* axis from
    // the key comparison — not a swap, a single dropped coordinate — falsely
    // aliases them onto `a`. One per axis so no single component can be ignored.
    const BlockPos xOnly{7, 2, 3};
    const BlockPos yOnly{1, 7, 3};
    const BlockPos zOnly{1, 2, 7};

    // --- place inserts once, and rejects a second placement over the same cell so
    //     an existing entity is never orphaned. ---
    assert(store.place(a));
    assert(!store.place(a));

    // --- find hits the exact cell and only the exact cell. A swapped or a
    //     single-axis-different cell must miss; if find ignored or crossed an
    //     axis, one of these would falsely hit. ---
    assert(store.find(a) != nullptr);
    assert(store.find(swappedXY) == nullptr);
    assert(store.find(swappedYZ) == nullptr);
    assert(store.find(swappedXZ) == nullptr);
    assert(store.find(xOnly) == nullptr);
    assert(store.find(yOnly) == nullptr);
    assert(store.find(zOnly) == nullptr);
    // place must agree with find on identity: a cell that differs in one axis is a
    // new cell, so its placement succeeds rather than colliding with `a`.
    assert(store.place(xOnly));
    assert(store.place(yOnly));
    assert(store.place(zOnly));
    assert(store.find(a) != nullptr); // still its own cell after the neighbours

    // Tag through the mutable handle, read back through the const one: same cell.
    store.find(a)->tag = 42;
    const auto& constStore = store;
    assert(constStore.find(a) != nullptr && constStore.find(a)->tag == 42);

    // --- Two nearby cells that differ only in one axis stay distinct. ---
    assert(store.place(swappedYZ));
    assert(store.find(a)->tag == 42);        // untouched
    assert(store.find(swappedYZ)->tag == 0); // the new, empty cell

    // --- findOrCreate returns the existing entity, or mints an empty one for a
    //     cell that has none (a block loaded from a pre-block-entity save). ---
    assert(store.findOrCreate(a).tag == 42);
    auto& created = store.findOrCreate(BlockPos{9, 8, 7});
    assert(created.tag == 0);
    created.tag = 7;
    assert(store.find(BlockPos{9, 8, 7})->tag == 7);

    // --- remove hands back the entity at a cell and only that cell; a second
    //     remove of the same cell is empty. ---
    const auto removed = store.remove(a);
    assert(removed.has_value() && removed->tag == 42);
    assert(store.find(a) == nullptr);
    assert(!store.remove(a).has_value());
    assert(!store.remove(swappedXY).has_value());
    // The neighbours the remove must not have disturbed.
    assert(store.find(swappedYZ) != nullptr);
    assert(store.find(BlockPos{9, 8, 7})->tag == 7);

    std::puts("block_entity_store_test: OK");
    return 0;
}

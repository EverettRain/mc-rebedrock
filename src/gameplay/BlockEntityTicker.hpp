#pragma once

// The block-entity ticker behaviour table: BE2's statement of "which block
// entities do something every game tick, and skip the ones that don't".
//
// Before this, GameSession::tick() reached into each system by hand —
// furnaceSystem_.tick(); chestSystem_.tick(); — so adding a block entity meant
// finding and editing that call site, and a tickless block entity still cost a
// container walk. This replaces the hand-list with a table keyed by
// BlockEntityTypeId, exactly the DOD shape kRandomTickTable proved for blocks: a
// null slot is the hot-path pre-filter that skips a whole type's container, and
// the dispatch is a fn-ptr load rather than a virtual call.
//
// One wrinkle from the storage decision (BE1 方案 A: each kind keeps its own
// BlockEntityStore<BlockPos, XxxBE>): the containers are heterogeneous, so the
// iteration over them is type-specific. The table slot therefore ticks a whole
// container, not a single entity — the same way kRandomTickTable's slot takes a
// context rather than a bare block. The per-entity logic (a chest's lid ease, a
// furnace's burn) is unchanged: each slot just drives its system's tick().

#include "core/ContentId.hpp"
#include "gameplay/ChestSystem.hpp"
#include "gameplay/FurnaceSystem.hpp"
#include "world/BlockEntityType.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace mc::gameplay {

// Everything a per-type ticker needs to advance its whole container one tick.
// Passed by reference so a dispatch stays a single indirect call with one
// argument, whatever a future ticker ends up needing.
struct BlockEntityTickContext final {
    ChestSystem& chests;
    ChestSystem& trappedChests;
    FurnaceSystem& furnaces;
};

using BlockEntityTickerFn = void (*)(const BlockEntityTickContext&);

// The per-type ticker slots. Each advances its kind's container one game tick,
// the behaviour lifted verbatim from the system's tick(): a chest eases its lids
// toward their open target, a furnace burns fuel and smelts its input. Kept as
// thin forwarders so the lid/burn algorithm stays in the system that owns the
// data (BE2 moves the *drive*, not the logic).
inline void tickChestEntities(const BlockEntityTickContext& context) {
    context.chests.tick();
}
inline void tickTrappedChestEntities(const BlockEntityTickContext& context) {
    // A trapped chest's lid eases exactly like a chest's — same ChestSystem tick,
    // its own container.
    context.trappedChests.tick();
}
inline void tickFurnaceEntities(const BlockEntityTickContext& context) {
    context.furnaces.tick();
}

// The behaviour table, indexed by BlockEntityTypeId: a null slot means the type
// has no ticker and its whole container is stepped over — the pre-filter, so a
// tickless block entity (a sign, a flower pot, when they land in BE3) costs
// nothing per tick. constexpr in the header for the same reason kRandomTickTable
// is: the drive runs every game tick and the reject must fold to one indexed
// load. Built from the enum-ordinal-equals-id guarantee blockEntityTypeId gives,
// so the row for a kind lands at that kind's own id.
inline constexpr std::array<BlockEntityTickerFn, world::kBuiltinBlockEntityTypeCount>
    kBlockEntityTickerTable = [] {
        std::array<BlockEntityTickerFn, world::kBuiltinBlockEntityTypeCount> entries{};
        entries[world::blockEntityTypeId(world::BlockEntityKind::Chest).index()] =
            &tickChestEntities;
        entries[world::blockEntityTypeId(world::BlockEntityKind::TrappedChest).index()] =
            &tickTrappedChestEntities;
        entries[world::blockEntityTypeId(world::BlockEntityKind::Furnace).index()] =
            &tickFurnaceEntities;
        return entries;
    }();

// Whether a block-entity type ticks at all — the cheapest statement of "does
// this block entity do anything every tick", and the pre-filter the drive tests
// before touching a type's container. Mirrors WorldSimulation::isRandomlyTicking.
[[nodiscard]] constexpr bool hasTicker(core::BlockEntityTypeId type) {
    const auto index = type.index();
    return index < kBlockEntityTickerTable.size() && kBlockEntityTickerTable[index] != nullptr;
}

// What one drive did. The acceptance wants to assert a tickless type is stepped
// over rather than infer it, so the counts are surfaced.
struct BlockEntityTickStats final {
    int ticked = 0;  // types whose ticker slot ran
    int skipped = 0; // types with no ticker, whose container was left untouched
};

// Advances every block-entity container whose type has a ticker, in ascending
// BlockEntityTypeId order. The order is the array-index order, never a map's, so
// two runs from the same state step the same containers in the same sequence —
// the determinism iron law, and what keeps a multi-entity tick reproducible. The
// table is a parameter, defaulting to the built-in one, so a test can drive a
// table with a slot punched out and watch the container it guards go untouched.
inline BlockEntityTickStats tickBlockEntities(
    const BlockEntityTickContext& context,
    std::span<const BlockEntityTickerFn> table = kBlockEntityTickerTable) {
    BlockEntityTickStats stats;
    for (const BlockEntityTickerFn ticker : table) {
        if (ticker == nullptr) {
            ++stats.skipped;
            continue;
        }
        ticker(context);
        ++stats.ticked;
    }
    return stats;
}

} // namespace mc::gameplay

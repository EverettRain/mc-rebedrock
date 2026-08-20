// R0-2 Block de-enum: BlockId is the runtime identity, and the tables that used
// to be sized to `Block::Count` and indexed by the enum ordinal now index by
// BlockId and size to the registry.
//
// This covers the four moving parts R0-2 touched that are headless-verifiable:
//   1. the Block <-> BlockId bridge (ordinal identity, both directions a cast);
//   2. the interned state metadata, now keyed by BlockId, round-trips;
//   3. the save palette (DensePalette) is domain-sized at construction with no
//      256 ceiling — it holds ids well past 256 and rejects one past its domain;
//   4. block tags are BlockId-indexed and sized to the registry.

#include "gameplay/BlockTags.hpp"
#include "persistence/SaveStream.hpp"
#include "world/Block.hpp"
#include "world/BlockRegistry.hpp"
#include "world/BlockState.hpp"
#include "world/BlockStateTable.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace {

using mc::core::BlockId;
using mc::world::Block;

// The bridge is an ordinal identity in both directions, and matches the frozen
// registry's ids exactly (BlockRegistry asserts the same on load).
void testBridge() {
    for (std::size_t i = 0; i < mc::world::kBuiltinBlockCount; ++i) {
        const auto block = static_cast<Block>(i);
        const BlockId id = mc::world::blockId(block);
        assert(id.valid());
        assert(id.index() == i);
        assert(mc::world::blockFromId(id) == block);
        // The bridge agrees with the runtime registry it stands in for.
        assert(mc::world::blockRegistry().identifier(id) ==
               mc::world::blockDefinition(block).identifier);
    }
    // The registry size is the identity count the runtime tables cut to; with no
    // external content it equals the built-in count.
    assert(mc::world::blockCount() == mc::world::kBuiltinBlockCount);
    assert(mc::world::blockId(Block::Air).value() == 0);
}

// Every interned state carries its block as a BlockId, and that identity agrees
// with the enum-narrowed view and with the block's default-state range. A
// misplaced range boundary in the metadata build shows up here as a state whose
// recorded block does not match the range it fell in.
void testStateMetadataByBlockId() {
    for (std::uint32_t id = 0; id < mc::world::kBlockStateCount; ++id) {
        const auto stateId = static_cast<std::uint16_t>(id);
        const BlockId byId = mc::world::blockIdOfState(stateId);
        assert(byId.valid());
        // BlockId view and enum view name the same block.
        assert(mc::world::blockFromId(byId) == mc::world::blockOfState(stateId));
        // The state lies inside its block's declared id range.
        const auto kind = byId.index();
        assert(stateId >= mc::world::kBlockStateRangeStarts[kind]);
        assert(stateId < mc::world::kBlockStateRangeStarts[kind + 1U]);
    }
    // The BlockId-keyed default-state accessor agrees with the enum one for every
    // block, and lands on that block's first state.
    for (std::size_t i = 0; i < mc::world::kBuiltinBlockCount; ++i) {
        const auto block = static_cast<Block>(i);
        const auto viaEnum = mc::world::defaultBlockStateId(block);
        const auto viaId = mc::world::defaultBlockStateId(mc::world::blockId(block));
        assert(viaEnum == viaId);
        assert(mc::world::blockIdOfState(viaEnum) == mc::world::blockId(block));
    }
}

// The palette's reverse index is sized at construction, not to a 256 ceiling, so
// it holds ids far past 256 and hands out dense, stable indices. An id at or past
// the domain is a caller bug and throws rather than colliding.
void testPaletteNoLonger256Capped() {
    constexpr std::size_t kDomain = 1000; // deliberately > 256
    mc::persistence::DensePalette<BlockId> palette{BlockId::of(0), kDomain};

    // Empty (id 0) is always index 0.
    assert(palette.indexOf(BlockId::of(0)) == 0);
    // Ids past the old 256 ceiling index without throwing, and are stable.
    const auto a = palette.indexOf(BlockId::of(300));
    const auto b = palette.indexOf(BlockId::of(999));
    assert(a == 1 && b == 2);
    assert(palette.indexOf(BlockId::of(300)) == a);
    assert(palette.indexOf(BlockId::of(999)) == b);
    assert(palette.entries().size() == 3);

    // An id at the domain boundary is outside the palette and rejected.
    bool threw = false;
    try {
        static_cast<void>(palette.indexOf(BlockId::of(kDomain)));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

// Tags are BlockId-indexed and sized to the registry; the enum and BlockId
// overloads agree, and set() grows the table to cover any id the registry hands
// out (the property external content will lean on).
void testTagsByBlockId() {
    mc::gameplay::BlockTagTable table;
    table.loadBuiltinDefaults();

    // Every registered block has a slot after the registry-sized default load,
    // and the two overloads answer identically.
    for (std::size_t i = 0; i < mc::world::blockCount(); ++i) {
        const auto block = static_cast<Block>(i);
        const BlockId id = mc::world::blockId(block);
        assert(table.has(block, mc::gameplay::BlockTag::Logs) ==
               table.has(id, mc::gameplay::BlockTag::Logs));
    }

    // Oak log is a log; grass is not — a spot check that the BlockId path reads
    // the same membership the enum path does.
    assert(table.has(mc::world::blockId(Block::OakLog), mc::gameplay::BlockTag::Logs));
    assert(!table.has(mc::world::blockId(Block::Grass), mc::gameplay::BlockTag::Logs));

    // Setting by BlockId sticks and is visible through both overloads.
    table.set(mc::world::blockId(Block::Stone), mc::gameplay::BlockTag::MineableWithPickaxe);
    assert(table.has(Block::Stone, mc::gameplay::BlockTag::MineableWithPickaxe));
}

} // namespace

int main() {
    testBridge();
    testStateMetadataByBlockId();
    testPaletteNoLonger256Capped();
    testTagsByBlockId();
    return 0;
}

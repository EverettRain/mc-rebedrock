#include "world/BlockStateTable.hpp"
#include "world/ChunkSection.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>

// ChunkSection now stores one interned state id per cell instead of three
// parallel arrays. That is meant to be a pure representation change, so what is
// pinned here is the behaviour the three arrays had — plus the invariant the
// new storage relies on and the old one did not.
int main() {
    using namespace mc::world;

    // --- Every block's default orientation is inside its own state range. ---
    // This is the invariant the collapse rests on. Interned storage can only
    // hold states a block declares, so if a block's *default* fell outside its
    // range, every freshly placed one would decode as something else. Walking
    // the registry catches a mis-sized range the moment it is added, rather
    // than when someone notices logs facing the wrong way.
    for (std::size_t kind = 0; kind < kBlockKindCount; ++kind) {
        const auto block = static_cast<Block>(kind);
        const auto fallback = defaultOrientation(block);
        const BlockState state{block, fallback};
        assert(state.block() == block);
        assert(state.orientation() == fallback);
    }

    // The families that actually use the state slot must have room for the
    // whole range their writers produce: crop age and farmland moisture reach
    // 7, the log axis is stored as Up (index 4), leaves flag persistence as
    // East (index 1), and a horizontal facing reaches West (index 3).
    {
        assert((BlockState{Block::WheatCrops, cropOrientation(7)}.orientation() ==
                cropOrientation(7)));
        assert((BlockState{Block::Farmland, farmlandOrientation(7)}.orientation() ==
                farmlandOrientation(7)));
        assert((BlockState{Block::OakLog, BlockOrientation::Up}.orientation() ==
                BlockOrientation::Up));
        assert((BlockState{Block::OakLeaves, kPersistentLeavesState}.orientation() ==
                kPersistentLeavesState));
        assert((BlockState{Block::Furnace, BlockOrientation::West}.orientation() ==
                BlockOrientation::West));
    }

    // --- An unallocated section is air everywhere, and stays unallocated. ---
    {
        ChunkSection section;
        assert(section.empty());
        assert(section.block(0, 0, 0) == Block::Air);
        assert(section.orientation(3, 4, 5) == BlockOrientation::North);
        assert(section.fluidLevel(3, 4, 5) == 0U);

        // Writing air, or a default orientation, must not force the array to
        // materialise — an all-air section is the common case in a surface
        // world and paying 8 KB for each one would undo the collapse.
        section.setBlock(1, 1, 1, Block::Air);
        section.setOrientation(1, 1, 1, BlockOrientation::North);
        section.setFluidLevel(1, 1, 1, 0U);
        assert(section.empty());
    }

    // --- The three accessors still agree with each other after a write. ---
    {
        ChunkSection section;
        section.setBlock(2, 3, 4, Block::Furnace);
        section.setOrientation(2, 3, 4, BlockOrientation::South);
        assert(!section.empty());
        assert(section.block(2, 3, 4) == Block::Furnace);
        assert(section.orientation(2, 3, 4) == BlockOrientation::South);
        assert((section.state(2, 3, 4) == BlockState{Block::Furnace, BlockOrientation::South}));

        // Replacing the block resets the state slot, exactly as the separate
        // orientation array used to on every setBlock.
        section.setBlock(2, 3, 4, Block::Stone);
        assert(section.orientation(2, 3, 4) == defaultOrientation(Block::Stone));

        // A log keeps its axis rather than the generic North default.
        section.setBlock(5, 5, 5, Block::OakLog);
        assert(section.orientation(5, 5, 5) == BlockOrientation::Up);
    }

    // --- Fluid level rides in the same cell and clears with the block. ---
    {
        ChunkSection section;
        section.setBlock(6, 7, 8, Block::Water);
        section.setFluidLevel(6, 7, 8, 5U);
        assert(section.fluidLevel(6, 7, 8) == 5U);
        assert(section.state(6, 7, 8).fluidLevel() == 5U);

        section.setBlock(6, 7, 8, Block::Stone);
        assert(section.fluidLevel(6, 7, 8) == 0U);
    }

    // --- The non-air count drives empty(), and survives state edits. ---
    {
        ChunkSection section;
        section.setBlock(0, 0, 0, Block::Stone);
        assert(!section.empty());
        // Editing the state slot of an existing block must not double-count it.
        section.setBlock(0, 0, 0, Block::Furnace);
        section.setOrientation(0, 0, 0, BlockOrientation::East);
        section.setBlock(0, 0, 0, Block::Air);
        assert(section.empty());
    }

    // --- Out-of-bounds reads are air rather than a crash. ---
    {
        const ChunkSection section;
        assert(section.block(-1, 0, 0) == Block::Air);
        assert(section.block(0, 16, 0) == Block::Air);
        assert(section.fluidLevel(0, 0, 99) == 0U);
    }

    // --- An all-air section costs nothing; the flat array cost 8 KB. ---
    // This is the memory the paletted storage exists to recover: a surface
    // world is mostly empty sections, and the previous single-array layout paid
    // a full 4096x2 bytes for the first non-air block written to a section.
    {
        ChunkSection section;
        assert(section.stateHeapBytes() == 0U);
        assert(section.bitsPerEntry() == 0U);
        assert(section.paletteSize() == 0U);
        // Air and default writes keep it uniform and free.
        section.setBlock(4, 4, 4, Block::Air);
        assert(section.stateHeapBytes() == 0U);
        assert(section.bitsPerEntry() == 0U);
    }

    // --- The palette widens one step at a time as distinct states appear. ---
    // Each widening re-packs every cell already written, so the assertion that
    // matters is that earlier cells still decode after the bits grow under them.
    {
        ChunkSection section;
        section.setState(0, 0, 0, BlockState{Block::Stone});
        // { air, stone } -> 1 bit.
        assert(section.bitsPerEntry() == 1U);
        section.setState(1, 0, 0, BlockState{Block::Dirt});
        // { air, stone, dirt } -> 2 bits, and the stone written at 1 bit must
        // survive the re-pack.
        assert(section.bitsPerEntry() == 2U);
        assert(section.block(0, 0, 0) == Block::Stone);
        assert(section.block(1, 0, 0) == Block::Dirt);
        section.setState(2, 0, 0, BlockState{Block::Sand});
        section.setState(3, 0, 0, BlockState{Block::Gravel});
        section.setState(4, 0, 0, BlockState{Block::Cobblestone});
        // Five distinct plus air is six entries -> 3 bits.
        assert(section.bitsPerEntry() == 3U);
        assert(section.block(0, 0, 0) == Block::Stone);
        assert(section.block(1, 0, 0) == Block::Dirt);
        assert(section.block(2, 0, 0) == Block::Sand);
        assert(section.block(3, 0, 0) == Block::Gravel);
        assert(section.block(4, 0, 0) == Block::Cobblestone);
    }

    // --- Filling every cell round-trips through the bit packing. ---
    // A cyclic fill drives the palette up to five bits and crosses every 64-bit
    // word boundary (64/5 = 12 entries per word, 4 bits spilled each), so a
    // straddling read or a mis-sized word count shows up as one wrong cell.
    {
        const BlockState palette[] = {
            BlockState{Block::Stone},
            BlockState{Block::Dirt},
            BlockState{Block::Water, BlockOrientation::North, 0U},
            BlockState{Block::Water, BlockOrientation::North, 4U},
            BlockState{Block::Water, BlockOrientation::North, 8U},
            BlockState{Block::Furnace, BlockOrientation::North},
            BlockState{Block::Furnace, BlockOrientation::South},
            BlockState{Block::Furnace, BlockOrientation::East}.withLit(true),
            BlockState{Block::Furnace, BlockOrientation::West}.withLit(true),
            BlockState{Block::WheatCrops, cropOrientation(0)},
            BlockState{Block::WheatCrops, cropOrientation(3)},
            BlockState{Block::WheatCrops, cropOrientation(7)},
            BlockState{Block::Farmland, farmlandOrientation(7)},
            BlockState{Block::OakLog, BlockOrientation::Up},
            BlockState{Block::OakLeaves, kPersistentLeavesState},
            BlockState{Block::Sand},
            BlockState{Block::Gravel},
            BlockState{Block::Cobblestone},
            BlockState{Block::Glass},
            BlockState{Block::Chest},
        };
        constexpr int kCount = static_cast<int>(sizeof(palette) / sizeof(palette[0]));

        ChunkSection section;
        for (int y = 0; y < 16; ++y) {
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    const int cell = (y * 16 + z) * 16 + x;
                    section.setState(x, y, z, palette[cell % kCount]);
                }
            }
        }
        // Twenty distinct states plus air -> 5 bits, and 4096 five-bit indices
        // are far under the flat 8 KB (about 2.6 KB here).
        assert(section.bitsPerEntry() == 5U);
        assert(section.stateHeapBytes() < 4096U);
        bool allMatched = true;
        for (int y = 0; y < 16; ++y) {
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    const int cell = (y * 16 + z) * 16 + x;
                    allMatched = allMatched && section.state(x, y, z) == palette[cell % kCount];
                }
            }
        }
        assert(allMatched);
        // Every cell is non-air, so the count reflects the whole section.
        assert(!section.empty());
    }

    return 0;
}

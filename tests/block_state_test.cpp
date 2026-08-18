#include "world/BlockState.hpp"
#include "world/BlockStateTable.hpp"
#include "world/Chunk.hpp"
#include "world/MutationFlags.hpp"
#include "world/World.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

// BlockState and MutationFlags exist so that WorldMutationService, the block
// behaviour callbacks and the tick scheduler can be written against their final
// signatures once. The representation behind BlockState is about to change
// (T0.4 swapped the packing for the interned table in BlockStateTable.hpp);
// everything asserted here is contract, not layout, so these assertions survived
// that swap unchanged. Keep it that way.
int main() {
    using namespace mc::world;

    // --- The three fields round-trip through the opaque value. ---
    {
        const BlockState state{Block::Furnace, BlockOrientation::East, 0U};
        assert(state.block() == Block::Furnace);
        assert(state.orientation() == BlockOrientation::East);
        assert(state.fluidLevel() == 0U);

        // Flowing water at the widest level the save format admits (0-8).
        const BlockState water{Block::Water, BlockOrientation::North, 8U};
        assert(water.block() == Block::Water);
        assert(water.fluidLevel() == 8U);

        // A crop's age is its own property with its own range, not a value
        // smuggled through the six-value direction enum. Asking a crop for a
        // facing gets north whatever its age, which is the whole point.
        for (int age = 0; age <= 7; ++age) {
            const auto crop = BlockState{Block::WheatCrops}.withAge(age);
            assert(crop.age() == age);
            assert(crop.orientation() == BlockOrientation::North);
            assert(crop.has(StateProperty::Age));
            assert(!crop.has(StateProperty::Facing));
        }
        // Farmland's moisture likewise, and neither block's property leaks into
        // the other's.
        for (int moisture = 0; moisture <= 7; ++moisture) {
            const auto tilled = BlockState{Block::Farmland}.withMoisture(moisture);
            assert(tilled.moisture() == moisture);
            assert(tilled.age() == 0);
        }
        // Leaves carry PERSISTENT, and a hand-placed leaf says so without
        // borrowing a direction to mean it.
        assert(BlockState{Block::OakLeaves}.withPersistent(true).persistent());
        assert(!BlockState{Block::OakLeaves}.persistent());
        assert(BlockState{Block::OakLeaves}.withPersistent(true).orientation() ==
               BlockOrientation::North);
        // Age and moisture saturate rather than wrapping: they are counters that
        // growth code increments, and one past the end must not reset the crop
        // to a seedling.
        assert(BlockState{Block::WheatCrops}.withAge(99).age() == 7);
        assert(BlockState{Block::WheatCrops}.withAge(-1).age() == 0);
        // A property the block does not declare reads as zero and writing it
        // changes nothing, so no caller has to check first.
        assert(BlockState{Block::Stone}.withAge(5).age() == 0);
        assert(BlockState{Block::Stone}.withAge(5) == BlockState{Block::Stone});
    }

    // --- Value semantics: equality compares the whole state, not the block. ---
    {
        const BlockState east{Block::Furnace, BlockOrientation::East};
        const BlockState west{Block::Furnace, BlockOrientation::West};
        assert(east != west);
        assert((east == BlockState{Block::Furnace, BlockOrientation::East}));
        assert(east.with(BlockOrientation::West) == west);
        assert(BlockState{Block::Water}.withFluidLevel(3U).fluidLevel() == 3U);
        // `with` keeps everything it was not asked to change.
        assert(east.with(BlockOrientation::West).block() == Block::Furnace);

        // A default state is air, so a zero-initialised cell means "nothing
        // here" without anyone writing that rule down.
        assert(BlockState{}.block() == Block::Air);
    }

    // --- isSameBlock ignores the state slot. ---
    // This is the predicate that decides a block entity's fate on a write: same
    // block keeps it, different block destroys and recreates it. A furnace that
    // merely changes facing must not lose its smelting progress.
    {
        const BlockState east{Block::Furnace, BlockOrientation::East};
        const BlockState west{Block::Furnace, BlockOrientation::West};
        assert(east.isSameBlock(west));
        assert(!east.isSameBlock(BlockState{Block::Chest}));

        // The lit furnace used to be its own Block, which made a burning
        // furnace read as a *different* block: a WorldMutationService comparing
        // old and new would have destroyed and recreated the block entity on
        // every burn swap, losing the smelt in progress. Now `lit` is a state
        // of the one furnace block, so lighting it changes nothing about its
        // identity. This assertion was deliberately pinned inverted before the
        // change so that flipping it had to be a conscious edit.
        assert(east.isSameBlock(east.withLit(true)));
        assert(east.withLit(true) != east);
        // ...and the light comes from the state, not from a second block.
        assert(east.emittedLight() == 0U);
        assert(east.withLit(true).emittedLight() == 13U);
    }

    // --- The states that used to be blocks of their own. ---
    // These two are why the interned table had to land before the furnace
    // block entity: while `lit` was a separate Block, every ignition read as a
    // block change, and while each wall torch facing was a separate Block, the
    // enum paid one member per direction.
    {
        // A furnace is one block whether or not it burns, so its facing rides
        // through the swap and its identity never changes.
        const BlockState cold{Block::Furnace, BlockOrientation::South};
        const BlockState burning = cold.withLit(true);
        assert(burning.block() == Block::Furnace);
        assert(burning.orientation() == BlockOrientation::South);
        assert(burning.isSameBlock(cold));
        assert(burning != cold);
        assert(burning.withLit(false) == cold);

        // Only the burning state emits; AbstractFurnaceBlock's 13.
        assert(cold.emittedLight() == 0U);
        assert(burning.emittedLight() == 13U);

        // A block with no LIT property is never lit, so callers never have to
        // ask whether the property exists.
        assert(!BlockState{Block::Stone}.lit());
        assert(!BlockState{Block::Stone}.withLit(true).lit());

        // All four wall torch facings are one block now, and all of them glow:
        // a torch's light does not depend on any state.
        for (const auto facing : {BlockOrientation::North, BlockOrientation::East,
                                  BlockOrientation::South, BlockOrientation::West}) {
            const BlockState torch{Block::WallTorch, facing};
            assert(torch.block() == Block::WallTorch);
            assert(torch.orientation() == facing);
            assert(torch.emittedLight() == 14U);
            assert(torch.isSameBlock(BlockState{Block::WallTorch}));
        }
        assert((BlockState{Block::WallTorch, BlockOrientation::East} !=
                BlockState{Block::WallTorch, BlockOrientation::West}));
    }

    // --- SlabBlock.TYPE: bottom/top/double round-trip, default is bottom. ---
    {
        // Exactly three states per slab (type only; waterlogged is a later
        // slice), and no other axis leaks in.
        assert(kBlockRegistry[static_cast<std::size_t>(Block::OakSlab)].states.stateCount() == 3U);
        assert(kBlockRegistry[static_cast<std::size_t>(Block::StoneSlab)].states.stateCount() == 3U);

        // A freshly placed slab is the block's default state: the bottom half.
        const BlockState fresh{Block::OakSlab};
        assert(fresh.slabPortion() == SlabPortion::Bottom);
        assert(!fresh.isFullCubeState());

        for (const auto portion : {SlabPortion::Bottom, SlabPortion::Top, SlabPortion::Double}) {
            const auto slab = BlockState{Block::OakSlab}.withSlabPortion(portion);
            assert(slab.block() == Block::OakSlab);
            assert(slab.slabPortion() == portion);
            // Only a double slab fills its whole cell.
            assert(slab.isFullCubeState() == (portion == SlabPortion::Double));
            // The SlabType axis does not disturb the block's other reads.
            assert(slab.orientation() == BlockOrientation::North);
        }

        // The three portions are three distinct states of the one block.
        assert(BlockState{Block::OakSlab}.withSlabPortion(SlabPortion::Top) !=
               BlockState{Block::OakSlab}.withSlabPortion(SlabPortion::Bottom));
        assert(BlockState{Block::OakSlab}.withSlabPortion(SlabPortion::Double)
                   .isSameBlock(BlockState{Block::OakSlab}));

        // A non-slab block reads back the zero portion and ignores the write.
        assert(BlockState{Block::Stone}.slabPortion() == SlabPortion::Bottom);
        assert(BlockState{Block::Stone}.withSlabPortion(SlabPortion::Top) ==
               BlockState{Block::Stone});
        assert(BlockState{Block::Stone}.isFullCubeState());
    }

    // --- The raw id survives a round trip, for the save palette. ---
    {
        const BlockState state{Block::Farmland, static_cast<BlockOrientation>(7), 0U};
        assert(BlockState::fromRawId(state.rawId()) == state);
        // Corrupt/out-of-version ids fail closed to air instead of indexing
        // outside the compiled metadata table.
        assert(BlockState::fromRawId(UINT16_MAX).block() == Block::Air);
    }

    // --- The interned table numbers every state exactly once. ---
    // This is the property the whole representation rests on: walk every block
    // and every state it declares, and every id must be distinct and must
    // decode back to what produced it. A range-start table off by one anywhere
    // would alias two blocks' states onto each other, which is the kind of bug
    // that shows up much later as a block turning into a different block.
    {
        std::vector<bool> seen(kBlockStateCount, false);
        std::uint32_t counted = 0U;
        for (std::size_t kind = 0; kind < kBlockKindCount; ++kind) {
            const auto block = static_cast<Block>(kind);
            const auto& schema = kBlockRegistry[kind].states;
            // The walk is driven by the block's declared schema rather than by
            // a fixed nest of loops over three known axes. That is what makes it
            // survive a fourth property: the previous version enumerated slot x
            // fluid x lit by hand, and an axis the table numbered but the walk
            // did not enumerate is exactly how two states end up sharing an id.
            for (std::uint16_t index = 0; index < schema.stateCount(); ++index) {
                auto state = BlockState{block};
                for (std::size_t axisIndex = 0; axisIndex < schema.size(); ++axisIndex) {
                    const auto axis = schema.axis(axisIndex);
                    const auto digit = static_cast<std::uint8_t>(
                        (index / schema.stride(axisIndex)) % axis.valueCount);
                    state = state.with(axis.property, digit);
                }
                const auto id = state.rawId();
                assert(id < kBlockStateCount);
                assert(!seen[id]);  // no two states share an id
                seen[id] = true;
                ++counted;
                // And the id decodes back to exactly what built it.
                assert(state.block() == block);
                for (std::size_t axisIndex = 0; axisIndex < schema.size(); ++axisIndex) {
                    const auto axis = schema.axis(axisIndex);
                    const auto digit = static_cast<std::uint8_t>(
                        (index / schema.stride(axisIndex)) % axis.valueCount);
                    assert(state.value(axis.property) == digit);
                }
            }
        }
        // Every id in the table is reachable: no gaps, no overrun.
        assert(counted == kBlockStateCount);
    }

    // --- A state a block cannot be in falls back to that block's default. ---
    // Stone has one state, so asking for a facing or a fluid level on it is not
    // a different stone — it is the same stone. Making the invalid combination
    // unnumberable is what keeps the table dense.
    {
        assert((BlockState{Block::Stone, BlockOrientation::East, 4U} == BlockState{Block::Stone}));
        // A furnace has four facings, so Up (index 4) is out of its range.
        assert((BlockState{Block::Furnace, BlockOrientation::Up} ==
                BlockState{Block::Furnace, BlockOrientation::North}));
        // ...but West (index 3) is inside it and stays distinct.
        assert((BlockState{Block::Furnace, BlockOrientation::West} !=
                BlockState{Block::Furnace, BlockOrientation::North}));
        // Only fluids carry a level; stone quietly has none.
        assert(BlockState{Block::Stone}.fluidLevel() == 0U);
        assert(BlockState{Block::Water}.withFluidLevel(8U).fluidLevel() == 8U);
    }

    // --- World::state / setState compose and decompose the live storage. ---
    {
        World world;
        world.setChunk({0, 0}, Chunk{});

        assert(world.setState(3, 5, 7, BlockState{Block::Furnace, BlockOrientation::South}));
        assert((world.state(3, 5, 7) == BlockState{Block::Furnace, BlockOrientation::South}));
        // The composed accessor agrees with the three it is built from.
        assert(world.block(3, 5, 7) == Block::Furnace);
        assert(world.orientation(3, 5, 7) == BlockOrientation::South);

        // Fluid level rides along rather than being cleared by the block write.
        assert(world.setState(4, 5, 7, BlockState{Block::Water, BlockOrientation::North, 5U}));
        assert(world.state(4, 5, 7).fluidLevel() == 5U);
        assert(world.fluidLevel(4, 5, 7) == 5U);

        // LIT survives the round trip through live storage. state() used to
        // recompose from block()/orientation()/fluidLevel(), none of which
        // carries LIT, so a furnace written burning read back cold: the mesher
        // saw an unlit front and the light engine saw no emission. There is no
        // per-axis lit() setter on World precisely so this is the only path.
        assert(world.setState(5, 5, 7, BlockState{Block::Furnace, BlockOrientation::South}.withLit(true)));
        assert(world.state(5, 5, 7).lit());
        assert(world.state(5, 5, 7).emittedLight() == 13U);
        assert(world.state(5, 5, 7).orientation() == BlockOrientation::South);
        // ...and it clears again, so extinguishing is not a no-op either.
        assert(world.setState(5, 5, 7, BlockState{Block::Furnace, BlockOrientation::South}));
        assert(!world.state(5, 5, 7).lit());
        assert(world.state(5, 5, 7).emittedLight() == 0U);

        // Out of the world (below the new bottom) and outside a loaded chunk
        // both read as air and refuse the write, exactly as block()/setBlock()
        // already do. −1 is inside the 384-tall column now, so the test uses a
        // row below kMinY.
        assert(!world.setState(3, kMinY - 1, 7, BlockState{Block::Stone}));
        assert(world.state(3, kMinY - 1, 7).block() == Block::Air);
        assert(world.state(1000, 5, 1000).block() == Block::Air);
    }

    // --- MutationFlags is a bitmask with the composites vanilla names. ---
    {
        // The ordinary edit notifies neighbours and clients, and nothing else.
        assert(hasFlag(MutationFlags::All, MutationFlags::NotifyNeighbors));
        assert(hasFlag(MutationFlags::All, MutationFlags::NotifyClients));
        assert(!hasFlag(MutationFlags::All, MutationFlags::SuppressDrops));

        // World generation notifies nobody and attaches no block entity.
        assert(!hasFlag(MutationFlags::Generation, MutationFlags::NotifyNeighbors));
        assert(!hasFlag(MutationFlags::Generation, MutationFlags::NotifyClients));
        assert(hasFlag(MutationFlags::Generation, MutationFlags::SkipBlockEntity));
        assert(hasFlag(MutationFlags::Generation, MutationFlags::SkipOnPlace));

        assert(hasFlag(MutationFlags::SkipAllSideEffects, MutationFlags::SuppressDrops));
        assert(hasFlag(MutationFlags::SkipAllSideEffects, MutationFlags::SkipBlockEntity));

        // Combining and masking behave like the bit operations they are.
        auto flags = MutationFlags::None;
        flags |= MutationFlags::NotifyNeighbors;
        assert(hasFlag(flags, MutationFlags::NotifyNeighbors));
        flags &= ~MutationFlags::NotifyNeighbors;
        assert(flags == MutationFlags::None);

        // hasFlag asks for *every* named bit, not any of them.
        assert(!hasFlag(MutationFlags::NotifyNeighbors, MutationFlags::All));
        assert(hasFlag(MutationFlags::All, MutationFlags::All));

        // There is deliberately no "do not update light" and no "do not rebuild
        // mesh" bit: both are derived from whether the state actually changed,
        // so a caller cannot forget them. 1.16.1's bit 32 meant "skip light";
        // 26.1 reclaimed that value for SuppressDrops, which is what it means
        // here too.
        static_assert(static_cast<std::uint16_t>(MutationFlags::SuppressDrops) == 32U);
    }

    return 0;
}

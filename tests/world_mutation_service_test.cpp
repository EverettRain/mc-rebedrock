#include "world/Chunk.hpp"
#include "world/NeighborUpdater.hpp"
#include "world/World.hpp"
#include "world/WorldMutationService.hpp"

#include <cassert>
#include <vector>

namespace {

using namespace mc::world;

// Records which consequences the service dispatched, so a test can assert the
// exact set the flags and the state diff should produce.
struct RecordingSink final : MutationSink {
    int blockEntityReplaced = 0;
    int neighborChanged = 0;
    int shapeUpdated = 0;
    int sectionDirty = 0;
    int dropsRequested = 0;
    int blockPlaced = 0;
    BlockState lastRemoved{};
    MutationCause lastCause = MutationCause::PlayerBreak;
    std::vector<BlockPos> shapeOrder;

    void onBlockEntityReplaced(BlockPos, BlockState, BlockState) override { ++blockEntityReplaced; }
    void onNeighborChanged(BlockPos, BlockPos) override { ++neighborChanged; }
    void onNeighborShapeUpdate(BlockPos neighbor, BlockPos) override {
        ++shapeUpdated;
        shapeOrder.push_back(neighbor);
    }
    void onSectionDirty(BlockPos) override { ++sectionDirty; }
    void onDropsRequested(BlockPos, BlockState removed, MutationCause cause) override {
        ++dropsRequested;
        lastRemoved = removed;
        lastCause = cause;
    }
    void onBlockPlaced(BlockPos, BlockState, BlockState) override { ++blockPlaced; }
};

[[nodiscard]] World loadedWorld() {
    World world;
    world.setChunk({0, 0}, Chunk{});
    return world;
}

} // namespace

int main() {
    using namespace mc::world;
    const BlockPos pos{3, 5, 7};

    WorldMutationService service;

    // --- Breaking a block: the full set of consequences. ---
    {
        World world = loadedWorld();
        assert(world.setState(pos.x, pos.y, pos.z, BlockState{Block::Stone}));
        RecordingSink sink;
        const auto result = service.setBlock(world, pos, BlockState{Block::Air},
                                             MutationFlags::All, MutationCause::PlayerBreak, sink);
        assert(result.changed);
        assert(result.previous.block() == Block::Stone && result.current.block() == Block::Air);
        assert(world.block(pos.x, pos.y, pos.z) == Block::Air);   // actually written
        assert(sink.blockEntityReplaced == 1);                    // stone -> air is a block swap
        assert(sink.dropsRequested == 1 && sink.lastRemoved.block() == Block::Stone);
        assert(sink.lastCause == MutationCause::PlayerBreak);
        assert(sink.sectionDirty == 1);                           // unconditional on change
        assert(sink.neighborChanged == 6);                        // six orthogonal neighbours
    }

    // --- Lighting a furnace: same block, so the entity (and smelt) survives. ---
    // This is the case the whole interned-state rework existed for: a state
    // change that keeps the block must not destroy the block entity, must not
    // drop anything, but must still relight (the front glows).
    {
        World world = loadedWorld();
        const BlockState cold{Block::Furnace, BlockOrientation::South};
        assert(world.setState(pos.x, pos.y, pos.z, cold));
        RecordingSink sink;
        const auto result = service.setBlock(world, pos, cold.withLit(true), MutationFlags::All,
                                             MutationCause::Command, sink);
        assert(result.changed);                     // the state differs (LIT)
        assert(sink.blockEntityReplaced == 0);      // ...but the block is the same
        assert(sink.dropsRequested == 0);           // nothing removed
        assert(sink.sectionDirty == 1);             // the glow still needs relighting
        assert(sink.neighborChanged == 6);
        assert(world.state(pos.x, pos.y, pos.z).lit());
    }

    // --- Placing into air: no drops (nothing was removed). ---
    {
        World world = loadedWorld();
        RecordingSink sink;
        const auto result = service.setBlock(world, pos, BlockState{Block::Stone},
                                             MutationFlags::All, MutationCause::PlayerPlace, sink);
        assert(result.changed);
        assert(sink.dropsRequested == 0);           // air had nothing to drop
        assert(sink.sectionDirty == 1);
        assert(sink.neighborChanged == 6);
    }

    // --- Writing the same state is a free no-op. ---
    {
        World world = loadedWorld();
        assert(world.setState(pos.x, pos.y, pos.z, BlockState{Block::Stone}));
        RecordingSink sink;
        const auto result = service.setBlock(world, pos, BlockState{Block::Stone},
                                             MutationFlags::All, MutationCause::Command, sink);
        assert(!result.changed);
        assert(sink.blockEntityReplaced == 0 && sink.neighborChanged == 0 &&
               sink.sectionDirty == 0 && sink.dropsRequested == 0);
    }

    // --- Generation flags: dirty the section, but tell no neighbours, attach no
    //     block entity, roll no drops. ---
    {
        World world = loadedWorld();
        assert(world.setState(pos.x, pos.y, pos.z, BlockState{Block::Stone}));
        RecordingSink sink;
        const auto result = service.setBlock(world, pos, BlockState{Block::Dirt},
                                             MutationFlags::Generation, MutationCause::Generation,
                                             sink);
        assert(result.changed);
        assert(sink.neighborChanged == 0);          // NotifyNeighbors not set
        assert(sink.blockEntityReplaced == 0);      // SkipBlockEntity
        assert(sink.dropsRequested == 0);           // generation does not drop
        assert(sink.sectionDirty == 1);             // the mesh still needs building
    }

    // --- SuppressDrops replaces a block without rolling its loot. ---
    {
        World world = loadedWorld();
        assert(world.setState(pos.x, pos.y, pos.z, BlockState{Block::Stone}));
        RecordingSink sink;
        const auto result =
            service.setBlock(world, pos, BlockState{Block::Air},
                             MutationFlags::NotifyNeighbors | MutationFlags::SuppressDrops,
                             MutationCause::Explosion, sink);
        assert(result.changed);
        assert(sink.dropsRequested == 0);           // suppressed
        assert(sink.blockEntityReplaced == 1);      // still a real block swap
        assert(sink.neighborChanged == 6);
    }

    // --- The shape pass: every neighbour recomputes its shape in
    //     kShapeUpdateOrder, before the reaction pass, on any real change. ---
    {
        World world = loadedWorld();
        assert(world.setState(pos.x, pos.y, pos.z, BlockState{Block::Stone}));
        RecordingSink sink;
        const auto result = service.setBlock(world, pos, BlockState{Block::Air},
                                             MutationFlags::All, MutationCause::PlayerBreak, sink);
        assert(result.changed);
        assert(sink.shapeUpdated == 6);            // all six neighbours
        assert(sink.shapeOrder.size() == 6U);
        for (std::size_t i = 0; i < 6U; ++i) {
            const auto& offset = kShapeUpdateOrder[i];
            assert(sink.shapeOrder[i] ==
                   BlockPos(pos.x + offset.x, pos.y + offset.y, pos.z + offset.z));
        }
    }

    // --- KnownShape is the caller's promise no shape changed: the shape pass is
    //     skipped, but neighbours still react. ---
    {
        World world = loadedWorld();
        assert(world.setState(pos.x, pos.y, pos.z, BlockState{Block::Stone}));
        RecordingSink sink;
        const auto result = service.setBlock(
            world, pos, BlockState{Block::Air},
            MutationFlags::KnownShape | MutationFlags::NotifyNeighbors, MutationCause::Command, sink);
        assert(result.changed);
        assert(sink.shapeUpdated == 0);            // shape pass skipped
        assert(sink.neighborChanged == 6);         // reaction pass still runs
    }

    // --- Generation (which includes KnownShape) dirties the mesh but runs no
    //     shape pass and no reactions. ---
    {
        World world = loadedWorld();
        assert(world.setState(pos.x, pos.y, pos.z, BlockState{Block::Stone}));
        RecordingSink sink;
        static_cast<void>(service.setBlock(world, pos, BlockState{Block::Dirt},
                                           MutationFlags::Generation, MutationCause::Generation,
                                           sink));
        assert(sink.shapeUpdated == 0);
        assert(sink.neighborChanged == 0);
        assert(sink.sectionDirty == 1);
    }

    // --- An out-of-world write changes nothing and dispatches nothing. ---
    {
        World world = loadedWorld();
        RecordingSink sink;
        const auto result = service.setBlock(world, {3, kMinY - 1, 7}, BlockState{Block::Stone},
                                             MutationFlags::All, MutationCause::Command, sink);
        assert(!result.changed);
        assert(sink.sectionDirty == 0 && sink.neighborChanged == 0);
    }

    // --- W-x-1: updateNeighborsAtExcept — the same fan-out with one cell held
    // back, Java's updateNeighborsAtExceptFromFacing. A diode waking the cell in
    // front of it uses this so that cell's fan-out does not come straight back
    // at the diode. ---
    {
        auto world = loadedWorld();
        WorldMutationService service;

        RecordingSink all;
        service.updateNeighborsAt({4, 64, 4}, all);
        assert(all.neighborChanged == 6);

        RecordingSink except;
        service.updateNeighborsAtExcept({4, 64, 4}, {5, 64, 4}, except);
        assert(except.neighborChanged == 5); // exactly one held back

        // A `skip` that is not one of the six neighbours holds nothing back, so
        // the exclusion is by position and not an off-by-one on the order.
        RecordingSink elsewhere;
        service.updateNeighborsAtExcept({4, 64, 4}, {9, 9, 9}, elsewhere);
        assert(elsewhere.neighborChanged == 6);
    }

    // --- W-8: onBlockPlaced is Java's setPlacedBy, not its onPlace. It fires
    // when the block *kind* at the cell changes and not on a state-only write.
    //
    // This is the only place the distinction is directly observable. In the
    // behaviour it currently drives — a diode's self-start — widening it to
    // every write does not change any output: the schedule is deduplicated, the
    // `shouldTurnOn` guard rejects most of it, and a redundant diode tick writes
    // nothing. The cost is wasted scheduled ticks rather than a wrong answer,
    // which is exactly why it needs pinning here instead of being left to a
    // circuit fixture that cannot tell the two apart. The next user of the slot
    // may well not be so forgiving.
    {
        auto world = loadedWorld();
        WorldMutationService service;
        const BlockPos pos{4, 64, 4};

        RecordingSink placing;
        static_cast<void>(service.setBlock(world, pos, BlockState{Block::Furnace},
                                           MutationFlags::All, MutationCause::PlayerPlace,
                                           placing));
        assert(placing.blockPlaced == 1); // air -> furnace: a real placement

        RecordingSink stateOnly;
        static_cast<void>(service.setBlock(world, pos,
                                           BlockState{Block::Furnace}.withLit(true),
                                           MutationFlags::All, MutationCause::ScheduledTick,
                                           stateOnly));
        assert(stateOnly.sectionDirty == 1);  // it really did change something...
        assert(stateOnly.blockPlaced == 0);   // ...but nothing was placed

        RecordingSink breaking;
        static_cast<void>(service.setBlock(world, pos, BlockState{Block::Air},
                                           MutationFlags::All, MutationCause::PlayerBreak,
                                           breaking));
        assert(breaking.blockPlaced == 1); // furnace -> air is a kind change too

        // Worldgen opts out through SkipOnPlace: a chunk being built has nobody
        // to run placement behaviour for.
        RecordingSink generated;
        static_cast<void>(service.setBlock(world, pos, BlockState{Block::Stone},
                                           MutationFlags::Generation,
                                           MutationCause::Generation, generated));
        assert(generated.blockPlaced == 0);
    }

    return 0;
}

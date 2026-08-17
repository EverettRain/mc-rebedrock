#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/WorldMutationService.hpp"

#include <cassert>

namespace {

using namespace mc::world;

// Records which consequences the service dispatched, so a test can assert the
// exact set the flags and the state diff should produce.
struct RecordingSink final : MutationSink {
    int blockEntityReplaced = 0;
    int neighborChanged = 0;
    int sectionDirty = 0;
    int dropsRequested = 0;
    BlockState lastRemoved{};
    MutationCause lastCause = MutationCause::PlayerBreak;

    void onBlockEntityReplaced(BlockPos, BlockState, BlockState) override { ++blockEntityReplaced; }
    void onNeighborChanged(BlockPos, BlockPos) override { ++neighborChanged; }
    void onSectionDirty(BlockPos) override { ++sectionDirty; }
    void onDropsRequested(BlockPos, BlockState removed, MutationCause cause) override {
        ++dropsRequested;
        lastRemoved = removed;
        lastCause = cause;
    }
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

    // --- An out-of-world write changes nothing and dispatches nothing. ---
    {
        World world = loadedWorld();
        RecordingSink sink;
        const auto result = service.setBlock(world, {3, kMinY - 1, 7}, BlockState{Block::Stone},
                                             MutationFlags::All, MutationCause::Command, sink);
        assert(!result.changed);
        assert(sink.sectionDirty == 0 && sink.neighborChanged == 0);
    }

    return 0;
}

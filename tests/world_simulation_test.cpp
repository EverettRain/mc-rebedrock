#include "gameplay/WorldSimulation.hpp"

#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/BlockPlacement.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <cassert>
#include <cstdlib>
#include <iterator>
#include <utility>

int main() {
    mc::world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    mc::world::World world;
    world.setChunk({0, 0}, std::move(chunk));
    mc::gameplay::WorldSimulation simulation;

    world.setBlock(4, 5, 4, mc::world::Block::Sand);
    world.setBlock(4, 6, 4, mc::world::Block::Sand);
    world.setBlock(4, 7, 4, mc::world::Block::Sand);
    simulation.notifyPlaced({4, 5, 4}, mc::world::Block::Sand);
    static_cast<void>(simulation.tick(world));
    assert(world.block(4, 5, 4) == mc::world::Block::Air);
    assert(simulation.fallingBlocks().size() == 1U);
    for (int tick = 1; tick < 30; ++tick) {
        static_cast<void>(simulation.tick(world));
    }
    assert(world.block(4, 1, 4) == mc::world::Block::Sand);
    assert(world.block(4, 2, 4) == mc::world::Block::Sand);
    assert(world.block(4, 3, 4) == mc::world::Block::Sand);
    assert(world.block(4, 5, 4) == mc::world::Block::Air);
    assert(world.block(4, 6, 4) == mc::world::Block::Air);
    assert(world.block(4, 7, 4) == mc::world::Block::Air);

    // Sweep every Y layer crossed during a fast tick. These heights exercise
    // different sub-block phases; the old endpoint sample missed the one-layer
    // floor for several of them and discarded the entity below the world.
    for (const int startingY : {14, 21, 25, 31, 39, 49, 63, 95, 127, 191, 255}) {
        mc::world::Chunk highChunk;
        highChunk.setBlock(4, 0, 4, mc::world::Block::Stone);
        highChunk.setBlock(4, startingY, 4, mc::world::Block::Sand);
        mc::world::World highWorld;
        highWorld.setChunk({0, 0}, std::move(highChunk));
        mc::gameplay::WorldSimulation highSimulation;
        highSimulation.notifyPlaced({4, startingY, 4}, mc::world::Block::Sand);
        for (int tick = 0; tick < 180; ++tick) {
            static_cast<void>(highSimulation.tick(highWorld));
        }
        assert(highWorld.block(4, 1, 4) == mc::world::Block::Sand);
        assert(highSimulation.fallingBlocks().empty());
    }

    // An unloaded owner chunk is unknown, not air. Freeze an in-flight block
    // until its column returns, then let it finish the same fall.
    mc::world::Chunk streamedChunk;
    streamedChunk.setBlock(4, 0, 4, mc::world::Block::Stone);
    streamedChunk.setBlock(4, 14, 4, mc::world::Block::Sand);
    mc::world::World streamedWorld;
    streamedWorld.setChunk({0, 0}, std::move(streamedChunk));
    mc::gameplay::WorldSimulation streamedSimulation;
    streamedSimulation.notifyPlaced({4, 14, 4}, mc::world::Block::Sand);
    static_cast<void>(streamedSimulation.tick(streamedWorld));
    assert(streamedSimulation.fallingBlocks().size() == 1U);
    const auto pausedPosition = streamedSimulation.fallingBlocks().front().position;
    const float pausedVelocity = streamedSimulation.fallingBlocks().front().verticalVelocity;
    assert(streamedWorld.removeChunk({0, 0}));
    for (int tick = 0; tick < 20; ++tick) {
        static_cast<void>(streamedSimulation.tick(streamedWorld));
    }
    assert(streamedSimulation.fallingBlocks().size() == 1U);
    assert(streamedSimulation.fallingBlocks().front().position == pausedPosition);
    assert(streamedSimulation.fallingBlocks().front().verticalVelocity == pausedVelocity);
    mc::world::Chunk reloadedChunk;
    reloadedChunk.setBlock(4, 0, 4, mc::world::Block::Stone);
    streamedWorld.setChunk({0, 0}, std::move(reloadedChunk));
    for (int tick = 0; tick < 100; ++tick) {
        static_cast<void>(streamedSimulation.tick(streamedWorld));
    }
    assert(streamedWorld.block(4, 1, 4) == mc::world::Block::Sand);
    assert(streamedSimulation.fallingBlocks().empty());

    world.setBlock(10, 5, 10, mc::world::Block::Gravel);
    world.setBlock(12, 5, 10, mc::world::Block::RedSand);
    simulation.notifyPlaced({10, 5, 10}, mc::world::Block::Gravel);
    simulation.notifyPlaced({12, 5, 10}, mc::world::Block::RedSand);
    for (int tick = 0; tick < 30; ++tick) {
        static_cast<void>(simulation.tick(world));
    }
    assert(world.block(10, 1, 10) == mc::world::Block::Gravel);
    assert(world.block(12, 1, 10) == mc::world::Block::RedSand);

    // A floating patch of sand collapses as a whole: when one cell starts
    // falling it notifies its neighbours (FallingBlock#getStateForNeighborUpdate),
    // so an unsupported block beside it falls too instead of hanging in the air.
    mc::world::Chunk patchChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            patchChunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    mc::world::World patchWorld;
    patchWorld.setChunk({0, 0}, std::move(patchChunk));
    mc::gameplay::WorldSimulation patchSimulation;
    patchWorld.setBlock(4, 5, 4, mc::world::Block::Sand);
    patchWorld.setBlock(5, 5, 4, mc::world::Block::Sand);
    patchSimulation.notifyPlaced({4, 5, 4}, mc::world::Block::Sand);
    for (int tick = 0; tick < 30; ++tick) {
        static_cast<void>(patchSimulation.tick(patchWorld));
    }
    assert(patchWorld.block(4, 1, 4) == mc::world::Block::Sand);
    assert(patchWorld.block(5, 1, 4) == mc::world::Block::Sand);
    assert(patchWorld.block(4, 5, 4) == mc::world::Block::Air);
    assert(patchWorld.block(5, 5, 4) == mc::world::Block::Air);

    // Generated floating sand hangs until a neighbour changes it, exactly like
    // vanilla: generation writes through ProtoChunk#setBlockState, which never
    // calls FallingBlock#onBlockAdded, so an unsupported cell the surface pass
    // leaves behind stays put (and costs no simulation time) until an edit
    // activates it. Only after notifyNeighborChanged does it start falling.
    mc::world::Chunk hangingChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            hangingChunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    hangingChunk.setBlock(6, 5, 6, mc::world::Block::Sand); // never notified
    mc::world::World hangingWorld;
    hangingWorld.setChunk({0, 0}, std::move(hangingChunk));
    mc::gameplay::WorldSimulation hangingSimulation;
    for (int tick = 0; tick < 60; ++tick) {
        static_cast<void>(hangingSimulation.tick(hangingWorld));
    }
    assert(hangingWorld.block(6, 5, 6) == mc::world::Block::Sand);
    assert(hangingSimulation.fallingBlocks().empty());
    // The same floating cell starts falling the moment a neighbour is notified.
    hangingSimulation.notifyNeighborChanged(hangingWorld, {6, 4, 6});
    for (int tick = 0; tick < 30; ++tick) {
        static_cast<void>(hangingSimulation.tick(hangingWorld));
    }
    assert(hangingWorld.block(6, 5, 6) == mc::world::Block::Air);
    assert(hangingWorld.block(6, 1, 6) == mc::world::Block::Sand);

    world.setBlock(8, 1, 8, mc::world::Block::Water);
    simulation.notifyPlaced({8, 1, 8}, mc::world::Block::Water);
    for (int tick = 0; tick < 40; ++tick) {
        static_cast<void>(simulation.tick(world));
    }
    assert(world.block(9, 1, 8) == mc::world::Block::Water);
    assert(world.block(7, 1, 8) == mc::world::Block::Water);
    assert(world.block(8, 1, 9) == mc::world::Block::Water);
    assert(world.block(8, 1, 7) == mc::world::Block::Water);
    assert(world.block(1, 1, 8) == mc::world::Block::Water);
    assert(world.fluidLevel(8, 1, 8) == 0U);
    assert(world.fluidLevel(1, 1, 8) == 7U);
    assert(world.block(0, 1, 8) == mc::world::Block::Air);
    assert(mc::gameplay::isCollectableWaterSource(world, {8, 1, 8}));
    assert(!mc::gameplay::isCollectableWaterSource(world, {1, 1, 8}));
    int sourceCount = 0;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            if (world.block(x, 1, z) != mc::world::Block::Water) {
                continue;
            }
            assert(std::abs(x - 8) + std::abs(z - 8) <= 7);
            sourceCount += world.fluidLevel(x, 1, z) == 0U ? 1 : 0;
        }
    }
    // Flowing levels must never silently acquire source behavior.  A single
    // bucket creates exactly one collectible level-0 cell.
    assert(sourceCount == 1);
    world.setBlock(8, 1, 8, mc::world::Block::Air);
    simulation.notifyNeighborChanged(world, {8, 1, 8});
    for (int tick = 0; tick < 80; ++tick) {
        static_cast<void>(simulation.tick(world));
    }
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            assert(world.block(x, 1, z) != mc::world::Block::Water);
        }
    }

    mc::world::Chunk slopeChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            slopeChunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    slopeChunk.setBlock(10, 0, 8, mc::world::Block::Air);
    mc::world::World slopeWorld;
    slopeWorld.setChunk({0, 0}, std::move(slopeChunk));
    mc::gameplay::WorldSimulation slopeSimulation;
    slopeWorld.setBlock(8, 1, 8, mc::world::Block::Water);
    slopeSimulation.notifyPlaced({8, 1, 8}, mc::world::Block::Water);
    for (int tick = 0; tick < 5; ++tick) {
        static_cast<void>(slopeSimulation.tick(slopeWorld));
    }
    // FlowableFluid#getSpread searches four blocks for the nearest drop and
    // initially chooses only the shortest (+X) route.
    assert(slopeWorld.block(9, 1, 8) == mc::world::Block::Water);
    assert(slopeWorld.block(7, 1, 8) == mc::world::Block::Air);
    assert(slopeWorld.block(8, 1, 7) == mc::world::Block::Air);
    assert(slopeWorld.block(8, 1, 9) == mc::world::Block::Air);

    // Water replaces non-colliding vegetation instead of treating it like a
    // wall. This is FlowableFluid#beforeBreakingBlock + canFill in Java.
    mc::world::Chunk plantChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            plantChunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    plantChunk.setBlock(9, 1, 8, mc::world::Block::GrassPlant);
    plantChunk.setBlock(7, 1, 8, mc::world::Block::Dandelion);
    plantChunk.setBlock(8, 1, 9, mc::world::Block::OakSapling);
    plantChunk.setBlock(8, 1, 7, mc::world::Block::GrassPlant);
    mc::world::World plantWorld;
    plantWorld.setChunk({0, 0}, std::move(plantChunk));
    mc::gameplay::WorldSimulation plantSimulation;
    plantWorld.setBlock(8, 1, 8, mc::world::Block::Water);
    plantWorld.setFluidLevel(8, 1, 8, 0U);
    plantSimulation.notifyPlaced({8, 1, 8}, mc::world::Block::Water);
    for (int tick = 0; tick < 5; ++tick) {
        static_cast<void>(plantSimulation.tick(plantWorld));
    }
    assert(plantWorld.block(9, 1, 8) == mc::world::Block::Water);
    assert(plantWorld.block(7, 1, 8) == mc::world::Block::Water);
    assert(plantWorld.block(8, 1, 9) == mc::world::Block::Water);
    assert(plantWorld.block(8, 1, 7) == mc::world::Block::Water);

    // Mining a seabed block wakes the source immediately above it, which must
    // create a falling-water column in the opened cell.
    mc::world::Chunk seabedChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            seabedChunk.setBlock(x, 0, z, mc::world::Block::Stone);
            seabedChunk.setBlock(x, 1, z, mc::world::Block::Stone);
        }
    }
    for (int z = 7; z <= 9; ++z) {
        for (int x = 7; x <= 9; ++x) {
            seabedChunk.setBlock(x, 2, z, mc::world::Block::Water);
            seabedChunk.setFluidLevel(x, 2, z, 0U);
        }
    }
    mc::world::World seabedWorld;
    seabedWorld.setChunk({0, 0}, std::move(seabedChunk));
    mc::gameplay::WorldSimulation seabedSimulation;
    seabedWorld.setBlock(8, 1, 8, mc::world::Block::Air);
    seabedSimulation.notifyNeighborChanged(seabedWorld, {8, 1, 8});
    for (int tick = 0; tick < 5; ++tick) {
        static_cast<void>(seabedSimulation.tick(seabedWorld));
    }
    assert(seabedWorld.block(8, 1, 8) == mc::world::Block::Water);
    assert(seabedWorld.fluidLevel(8, 1, 8) ==
           mc::gameplay::kFallingWaterLevel);

    // A gravity block vacates its voxel before its entity lands. Water must be
    // notified of that exact transition or the old sand position remains a
    // permanent dry hole until an unrelated neighbor update occurs.
    mc::world::Chunk fallingSeabedChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            fallingSeabedChunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    fallingSeabedChunk.setBlock(8, 3, 8, mc::world::Block::Sand);
    fallingSeabedChunk.setBlock(8, 4, 8, mc::world::Block::Water);
    fallingSeabedChunk.setFluidLevel(8, 4, 8, 0U);
    mc::world::World fallingSeabedWorld;
    fallingSeabedWorld.setChunk({0, 0}, std::move(fallingSeabedChunk));
    mc::gameplay::WorldSimulation fallingSeabedSimulation;
    fallingSeabedSimulation.notifyPlaced({8, 3, 8}, mc::world::Block::Sand);
    for (int tick = 0; tick < 10; ++tick) {
        static_cast<void>(fallingSeabedSimulation.tick(fallingSeabedWorld));
    }
    assert(fallingSeabedWorld.block(8, 3, 8) == mc::world::Block::Water);
    assert(fallingSeabedWorld.fluidLevel(8, 3, 8) ==
           mc::gameplay::kFallingWaterLevel);

    mc::world::Chunk verticalChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            verticalChunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    mc::world::World verticalWorld;
    verticalWorld.setChunk({0, 0}, std::move(verticalChunk));
    mc::gameplay::WorldSimulation verticalSimulation;
    verticalWorld.setBlock(8, 6, 8, mc::world::Block::Water);
    verticalSimulation.notifyPlaced({8, 6, 8}, mc::world::Block::Water);
    for (int tick = 0; tick < 64; ++tick) {
        static_cast<void>(verticalSimulation.tick(verticalWorld));
    }
    for (int y = 1; y < 6; ++y) {
        assert(verticalWorld.block(8, y, 8) == mc::world::Block::Water);
        assert(verticalWorld.fluidLevel(8, y, 8) == mc::gameplay::kFallingWaterLevel);
    }
    assert(verticalWorld.block(9, 1, 8) == mc::world::Block::Water);
    assert(verticalWorld.fluidLevel(9, 1, 8) ==
           mc::gameplay::kFallingWaterLevel);

    mc::world::Chunk infiniteChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            infiniteChunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    mc::world::World infiniteWorld;
    infiniteWorld.setChunk({0, 0}, std::move(infiniteChunk));
    mc::gameplay::WorldSimulation infiniteSimulation;
    infiniteWorld.setBlock(5, 1, 5, mc::world::Block::Water);
    infiniteWorld.setFluidLevel(5, 1, 5, 0U);
    infiniteSimulation.notifyPlaced({5, 1, 5}, mc::world::Block::Water);
    for (int tick = 0; tick < 32; ++tick) {
        static_cast<void>(infiniteSimulation.tick(infiniteWorld));
    }

    // Match actual bucket use: the first source has finished flowing before
    // the second source replaces an existing diagonal flowing-water cell.
    infiniteWorld.setBlock(6, 1, 6, mc::world::Block::Water);
    infiniteWorld.setFluidLevel(6, 1, 6, 0U);
    infiniteSimulation.notifyPlaced({6, 1, 6}, mc::world::Block::Water);
    for (int tick = 0; tick < 32; ++tick) {
        static_cast<void>(infiniteSimulation.tick(infiniteWorld));
    }
    assert(infiniteWorld.block(6, 1, 5) == mc::world::Block::Water);
    assert(infiniteWorld.fluidLevel(6, 1, 5) == 0U);
    assert(infiniteWorld.block(5, 1, 6) == mc::world::Block::Water);
    assert(infiniteWorld.fluidLevel(5, 1, 6) == 0U);

    // Simulate collecting one source with a bucket. Two neighboring sources
    // must recreate it as a level-0 source instead of a flowing level-1 cell.
    infiniteWorld.setBlock(5, 1, 5, mc::world::Block::Air);
    infiniteSimulation.notifyNeighborChanged(infiniteWorld, {5, 1, 5});
    for (int tick = 0; tick < 32; ++tick) {
        static_cast<void>(infiniteSimulation.tick(infiniteWorld));
    }
    assert(infiniteWorld.block(5, 1, 5) == mc::world::Block::Water);
    assert(infiniteWorld.fluidLevel(5, 1, 5) == 0U);

    // A large set of water deadlines drains over consecutive render phases
    // instead of being released as one global five-tick batch.
    mc::world::Chunk scheduledChunk;
    mc::gameplay::WorldSimulation scheduledSimulation;
    for (int z = 0; z < 2; ++z) {
        for (int x = 0; x < 16; ++x) {
            scheduledChunk.setBlock(x, 0, z, mc::world::Block::Stone);
            scheduledChunk.setBlock(x, 1, z, mc::world::Block::Water);
            scheduledChunk.setFluidLevel(x, 1, z, 0U);
        }
    }
    mc::world::World scheduledWorld;
    scheduledWorld.setChunk({0, 0}, std::move(scheduledChunk));
    for (int z = 0; z < 2; ++z) {
        for (int x = 0; x < 16; ++x) {
            scheduledSimulation.notifyPlaced({x, 1, z}, mc::world::Block::Water);
        }
    }
    for (int tick = 0; tick < 4; ++tick) {
        static_cast<void>(scheduledSimulation.tick(scheduledWorld));
        assert(scheduledSimulation.lastWaterUpdatesProcessed() == 0U);
    }
    static_cast<void>(scheduledSimulation.tick(scheduledWorld));
    assert(scheduledSimulation.lastWaterUpdatesProcessed() ==
           mc::gameplay::WorldSimulation::kMaximumWaterUpdatesPerPhase);
    assert(scheduledSimulation.pendingWaterUpdateCount() >= 16U);
    static_cast<void>(scheduledSimulation.tick(scheduledWorld));
    assert(scheduledSimulation.lastWaterUpdatesProcessed() ==
           mc::gameplay::WorldSimulation::kMaximumWaterUpdatesPerPhase);

    // Attached blocks pop off with a drop as soon as their support is gone, and
    // one break cascades into the next.
    mc::world::Chunk supportChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            supportChunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    mc::world::World supportWorld;
    supportWorld.setChunk({0, 0}, std::move(supportChunk));
    mc::gameplay::WorldSimulation supportSimulation;
    supportWorld.setBlock(3, 1, 3, mc::world::Block::Stone);
    supportWorld.setBlock(3, 2, 3, mc::world::Block::Torch);
    supportWorld.setBlock(4, 2, 3, mc::world::Block::WallTorch);
    supportWorld.setOrientation(4, 2, 3, mc::world::BlockOrientation::East);
    supportWorld.setBlock(3, 1, 5, mc::world::Block::Grass);
    supportWorld.setBlock(3, 2, 5, mc::world::Block::Dandelion);

    supportWorld.setBlock(3, 1, 3, mc::world::Block::Air);
    supportSimulation.notifyNeighborChanged(supportWorld, {3, 1, 3});
    auto supportChanges = supportSimulation.tick(supportWorld);
    assert(supportWorld.block(3, 2, 3) == mc::world::Block::Air);
    assert(supportWorld.block(4, 2, 3) == mc::world::Block::Air);
    std::size_t torchDrops = 0U;
    for (const auto& change : supportChanges) {
        if (change.dropped.block() == mc::world::Block::Torch ||
            change.dropped.block() == mc::world::Block::WallTorch) {
            ++torchDrops;
        }
    }
    assert(torchDrops == 2U);

    // Digging the soil out from under a flower drops the flower too.
    supportWorld.setBlock(3, 1, 5, mc::world::Block::Stone);
    supportSimulation.notifyNeighborChanged(supportWorld, {3, 1, 5});
    const auto flowerChanges = supportSimulation.tick(supportWorld);
    assert(supportWorld.block(3, 2, 5) == mc::world::Block::Air);
    assert(flowerChanges.size() == 1U);
    assert(flowerChanges.front().dropped.block() == mc::world::Block::Dandelion);

    // A block with no support requirement is never disturbed.
    supportWorld.setBlock(8, 4, 8, mc::world::Block::Stone);
    supportSimulation.notifyNeighborChanged(supportWorld, {8, 3, 8});
    static_cast<void>(supportSimulation.tick(supportWorld));
    assert(supportWorld.block(8, 4, 8) == mc::world::Block::Stone);

    // A small oak: four trunk blocks with a blob of leaves around the top.
    mc::world::Chunk treeChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            treeChunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    mc::world::World treeWorld;
    treeWorld.setChunk({0, 0}, std::move(treeChunk));
    mc::gameplay::WorldSimulation treeSimulation;
    std::size_t leafCount = 0U;
    for (int y = 1; y <= 4; ++y) {
        treeWorld.setBlock(8, y, 8, mc::world::Block::OakLog);
    }
    for (int y = 4; y <= 5; ++y) {
        for (int z = -2; z <= 2; ++z) {
            for (int x = -2; x <= 2; ++x) {
                if (x == 0 && z == 0 && y == 4) continue;
                treeWorld.setBlock(8 + x, y, 8 + z, mc::world::Block::OakLeaves);
                ++leafCount;
            }
        }
    }

    // With the trunk standing, nothing decays however long the world runs.
    treeSimulation.notifyNeighborChanged(treeWorld, {8, 4, 8});
    for (int tick = 0; tick < 400; ++tick) {
        assert(treeSimulation.tick(treeWorld).empty());
    }
    assert(treeWorld.block(6, 5, 6) == mc::world::Block::OakLeaves);

    // One leaf a player placed out of reach of any log keeps its PERSISTENT
    // flag and survives alongside the canopy that is about to go.
    treeWorld.setState(2, 3, 2,
                       mc::world::BlockState{mc::world::Block::OakLeaves}.withPersistent(true));
    treeSimulation.notifyNeighborChanged(treeWorld, {2, 3, 2});

    // Cut the trunk out and the canopy dissolves, dropping its loot as it goes.
    for (int y = 1; y <= 4; ++y) {
        treeWorld.setBlock(8, y, 8, mc::world::Block::Air);
        treeSimulation.notifyNeighborChanged(treeWorld, {8, y, 8});
    }
    std::size_t decayed = 0U;
    std::size_t decayDrops = 0U;
    for (int tick = 0; tick < 20000 && decayed < leafCount; ++tick) {
        for (const auto& change : treeSimulation.tick(treeWorld)) {
            assert(change.state.block() == mc::world::Block::Air);
            ++decayed;
            if (change.dropped.block() == mc::world::Block::OakLeaves) {
                ++decayDrops;
            }
        }
    }
    assert(decayed == leafCount);
    assert(decayDrops == leafCount);
    assert(treeWorld.block(6, 5, 6) == mc::world::Block::Air);
    assert(treeWorld.block(8, 5, 8) == mc::world::Block::Air);
    assert(treeWorld.block(2, 3, 2) == mc::world::Block::OakLeaves);

    // A grass block covered by water reverts to dirt: water has opacity 3 in
    // the spreadable-block material check, so canSpread fails on the random
    // tick. The water cell sits untouched above the grass (a grass block is not
    // replaceable), so this is purely the random-tick conversion.
    mc::world::Chunk waterGrassChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            waterGrassChunk.setBlock(x, 0, z, mc::world::Block::Stone);
            waterGrassChunk.setBlock(x, 1, z, mc::world::Block::Dirt);
        }
    }
    waterGrassChunk.setBlock(8, 2, 8, mc::world::Block::Grass);
    waterGrassChunk.setBlock(8, 3, 8, mc::world::Block::Water);
    waterGrassChunk.setFluidLevel(8, 3, 8, 0U);
    mc::world::World waterGrassWorld;
    waterGrassWorld.setChunk({0, 0}, std::move(waterGrassChunk));
    mc::gameplay::WorldSimulation waterGrassSimulation;
    waterGrassSimulation.setRandomTickSpeed(1000);
    for (int tick = 0; tick < 200; ++tick) {
        static_cast<void>(waterGrassSimulation.tick(waterGrassWorld));
    }
    assert(waterGrassWorld.block(8, 2, 8) == mc::world::Block::Dirt);
    assert(waterGrassWorld.block(8, 3, 8) == mc::world::Block::Water);

    // 遮挡判据是**形状**不是渲染层（Java 的 BlockBehaviour#getLightDampening：
    // 只有 isSolidRender 才报 15，也就是遮挡形状是满立方体）。
    // 这里曾按 isOpaque() 判，于是所有「Opaque 渲染层但不填满格子」的方块
    // ——26 种台阶、附魔台、三种铁砧——统统压死了下面的草。
    // 四种覆盖各取一例，正反都钉住：
    mc::world::Chunk coverChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            coverChunk.setBlock(x, 0, z, mc::world::Block::Stone);
            coverChunk.setBlock(x, 1, z, mc::world::Block::Dirt);
        }
    }
    mc::world::World coverWorld;
    coverWorld.setChunk({0, 0}, std::move(coverChunk));
    // 每根柱子隔 3 格，免得彼此的扩散探测（±1 格水平）互相干扰
    constexpr int kAnvilX = 2;
    constexpr int kSlabX = 5;
    constexpr int kTableX = 8;
    constexpr int kStoneX = 11;
    constexpr int kDoubleSlabX = 14;
    for (const int x : {kAnvilX, kSlabX, kTableX, kStoneX, kDoubleSlabX}) {
        coverWorld.setBlock(x, 2, 8, mc::world::Block::Grass);
    }
    coverWorld.setBlock(kAnvilX, 3, 8, mc::world::Block::Anvil);
    coverWorld.setBlock(kSlabX, 3, 8, mc::world::Block::OakSlab);
    coverWorld.setBlock(kTableX, 3, 8, mc::world::Block::EnchantingTable);
    coverWorld.setBlock(kStoneX, 3, 8, mc::world::Block::Stone);
    coverWorld.setState(kDoubleSlabX, 3, 8,
                        mc::world::BlockState{mc::world::Block::OakSlab}.withSlabPortion(
                            mc::world::SlabPortion::Double));
    mc::gameplay::WorldSimulation coverSimulation;
    coverSimulation.setRandomTickSpeed(1000);
    for (int tick = 0; tick < 200; ++tick) {
        static_cast<void>(coverSimulation.tick(coverWorld));
    }
    // 铁砧的遮挡形状是四个盒子（底座只有 12x12x4），附魔台只有 0.75 高，
    // 下半砖只占半格 —— vanilla 里这三种下面的草都活着
    assert(coverWorld.block(kAnvilX, 2, 8) == mc::world::Block::Grass);
    assert(coverWorld.block(kSlabX, 2, 8) == mc::world::Block::Grass);
    assert(coverWorld.block(kTableX, 2, 8) == mc::world::Block::Grass);
    // 反向：真正填满格子的仍然要压死草。双层台阶是逐**状态**才能答对的那一例
    // ——只问 Block 的话它和下半砖没有区别
    assert(coverWorld.block(kStoneX, 2, 8) == mc::world::Block::Dirt);
    assert(coverWorld.block(kDoubleSlabX, 2, 8) == mc::world::Block::Dirt);

    // A lit grass block spreads onto plain dirt in light: the random tick
    // probes a 3x3x5 volume around the grass and converts dirt cells that could
    // hold a grass block (soil below).
    mc::world::Chunk spreadChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            spreadChunk.setBlock(x, 0, z, mc::world::Block::Stone);
            spreadChunk.setBlock(x, 1, z, mc::world::Block::Dirt);
        }
    }
    spreadChunk.setBlock(8, 2, 8, mc::world::Block::Grass);
    spreadChunk.setBlock(9, 2, 8, mc::world::Block::Dirt);
    mc::world::World spreadWorld;
    spreadWorld.setChunk({0, 0}, std::move(spreadChunk));
    mc::gameplay::WorldSimulation spreadSimulation;
    spreadSimulation.setRandomTickSpeed(1000);
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            spreadWorld.setSkyLight(x, 3, z, 15U);
        }
    }
    for (int tick = 0; tick < 600; ++tick) {
        static_cast<void>(spreadSimulation.tick(spreadWorld));
    }
    assert(spreadWorld.block(9, 2, 8) == mc::world::Block::Grass);
    // The source grass survives the whole run (open air, full light above).
    assert(spreadWorld.block(8, 2, 8) == mc::world::Block::Grass);

    // The same field at night. getMaxLocalRawBrightness subtracts the ambient
    // darkness, so the open surface reads 4: enough for the grass to stay alive,
    // one short of the 9 it needs to spread. Before the environment snapshot the
    // simulation read the stored full-sun value at every hour and a field spread
    // through the night exactly as fast as through noon.
    mc::world::Chunk nightChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            nightChunk.setBlock(x, 0, z, mc::world::Block::Stone);
            nightChunk.setBlock(x, 1, z, mc::world::Block::Dirt);
        }
    }
    nightChunk.setBlock(8, 2, 8, mc::world::Block::Grass);
    nightChunk.setBlock(9, 2, 8, mc::world::Block::Dirt);
    nightChunk.setBlock(4, 2, 4, mc::world::Block::OakSapling);
    mc::world::World nightWorld;
    nightWorld.setChunk({0, 0}, std::move(nightChunk));
    mc::gameplay::WorldSimulation nightSimulation;
    nightSimulation.setRandomTickSpeed(1000);
    nightSimulation.setEnvironment(
        mc::gameplay::EnvironmentSnapshot::resolve(18000.0, 0.0F, 0.0F));
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            nightWorld.setSkyLight(x, 3, z, 15U);
        }
    }
    for (int tick = 0; tick < 600; ++tick) {
        static_cast<void>(nightSimulation.tick(nightWorld));
    }
    assert(nightWorld.block(9, 2, 8) == mc::world::Block::Dirt);
    assert(nightWorld.block(8, 2, 8) == mc::world::Block::Grass);
    // SaplingBlock#randomTick reads the same darkened value, so a sapling waits
    // for morning instead of sprouting in the dark.
    assert(nightWorld.block(4, 2, 4) == mc::world::Block::OakSapling);

    // A leaves-filtered surface is one level dimmer than open sky. At midnight
    // its stored sky 14 minus ambient darkness 11 reads 3: below the spreading
    // threshold, but not a reason for existing grass to die. The old survival
    // check incorrectly used that local brightness and spent a conversion on
    // grass -> dirt at night; morning then spent another growing it back.
    mc::world::Chunk canopyChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            canopyChunk.setBlock(x, 0, z, mc::world::Block::Stone);
            canopyChunk.setBlock(x, 1, z, mc::world::Block::Dirt);
            canopyChunk.setState(
                x, 4, z,
                mc::world::BlockState{mc::world::Block::OakLeaves}.withPersistent(true));
        }
    }
    canopyChunk.setBlock(8, 2, 8, mc::world::Block::Grass);
    canopyChunk.setBlock(9, 2, 8, mc::world::Block::Dirt);
    mc::world::World canopyWorld;
    canopyWorld.setChunk({0, 0}, std::move(canopyChunk));
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            canopyWorld.setSkyLight(x, 3, z, 14U);
        }
    }
    mc::gameplay::WorldSimulation canopySimulation;
    canopySimulation.setRandomTickSpeed(1000);
    canopySimulation.setEnvironment(
        mc::gameplay::EnvironmentSnapshot::resolve(18000.0, 0.0F, 0.0F));
    std::size_t nightCanopyConversions = 0U;
    for (int tick = 0; tick < 600; ++tick) {
        static_cast<void>(canopySimulation.tick(canopyWorld));
        nightCanopyConversions += canopySimulation.lastRandomTickConversions();
    }
    assert(canopyWorld.block(8, 2, 8) == mc::world::Block::Grass);
    assert(canopyWorld.block(9, 2, 8) == mc::world::Block::Dirt);
    assert(nightCanopyConversions == 0U);

    // The same stored sky is bright enough by day, so the distinction is
    // survival versus propagation: the source remains unchanged while nearby
    // surface dirt may now green over.
    canopySimulation.setEnvironment(
        mc::gameplay::EnvironmentSnapshot::resolve(6000.0, 0.0F, 0.0F));
    for (int tick = 0; tick < 600; ++tick) {
        static_cast<void>(canopySimulation.tick(canopyWorld));
    }
    assert(canopyWorld.block(8, 2, 8) == mc::world::Block::Grass);
    assert(canopyWorld.block(9, 2, 8) == mc::world::Block::Grass);

    // Grass must not spread into the dirt layer under itself: the probes reach
    // one below the surface, and without a light check on the target that dirt
    // converts to grass, dies for being covered, and repeats forever — a churn
    // of changes that drowns real spread and eats the per-tick budget. Buried
    // dirt (an opaque block above) stays dirt.
    mc::world::Chunk buriedChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            buriedChunk.setBlock(x, 0, z, mc::world::Block::Stone);
            buriedChunk.setBlock(x, 1, z, mc::world::Block::Dirt);
            buriedChunk.setBlock(x, 2, z, mc::world::Block::Dirt);
        }
    }
    buriedChunk.setBlock(8, 3, 8, mc::world::Block::Grass);
    buriedChunk.setBlock(9, 3, 8, mc::world::Block::Dirt);
    mc::world::World buriedWorld;
    buriedWorld.setChunk({0, 0}, std::move(buriedChunk));
    mc::gameplay::WorldSimulation buriedSimulation;
    buriedSimulation.setRandomTickSpeed(1000);
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            buriedWorld.setSkyLight(x, 4, z, 15U);
        }
    }
    for (int tick = 0; tick < 400; ++tick) {
        static_cast<void>(buriedSimulation.tick(buriedWorld));
    }
    assert(buriedWorld.block(8, 3, 8) == mc::world::Block::Grass);
    // Surface dirt next to the grass greened over...
    assert(buriedWorld.block(9, 3, 8) == mc::world::Block::Grass);
    // ...but the buried layers below it never did.
    assert(buriedWorld.block(8, 2, 8) == mc::world::Block::Dirt);
    assert(buriedWorld.block(8, 1, 8) == mc::world::Block::Dirt);

    // A sapling under open sky grows into a tree: the trunk takes the sapling's
    // own cell, and the canopy sits on top.
    mc::world::Chunk saplingChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            saplingChunk.setBlock(x, 0, z, mc::world::Block::Stone);
            saplingChunk.setBlock(x, 1, z, mc::world::Block::Dirt);
        }
    }
    saplingChunk.setBlock(8, 2, 8, mc::world::Block::OakSapling);
    mc::world::World saplingWorld;
    saplingWorld.setChunk({0, 0}, std::move(saplingChunk));
    mc::gameplay::WorldSimulation saplingSimulation;
    saplingSimulation.setRandomTickSpeed(1000);
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            saplingWorld.setSkyLight(x, 3, z, 15U);
        }
    }
    bool grew = false;
    std::vector<mc::gameplay::BlockChange> saplingGrowthChanges;
    for (int tick = 0; tick < 1000 && !grew; ++tick) {
        auto changes = saplingSimulation.tick(saplingWorld);
        grew = saplingWorld.block(8, 2, 8) == mc::world::Block::OakLog;
        if (grew) {
            saplingGrowthChanges = std::move(changes);
        }
    }
    assert(grew);
    // Oak trunks are 4-7 blocks tall, so the cell above the base is always log.
    assert(saplingWorld.block(8, 3, 8) == mc::world::Block::OakLog);
    assert(saplingWorld.orientation(8, 2, 8) == mc::world::BlockOrientation::Up);
    assert(saplingWorld.orientation(8, 3, 8) == mc::world::BlockOrientation::Up);
    bool publishedVerticalTrunk = false;
    for (const auto& change : saplingGrowthChanges) {
        if (change.state.block() != mc::world::Block::OakLog) {
            continue;
        }
        // Runtime growth must publish the same complete state that it stored.
        // Previously the worker received a horizontal default and never saw
        // the separate orientation patch made only in the simulation world.
        assert(change.state.orientation() == mc::world::BlockOrientation::Up);
        publishedVerticalTrunk = true;
    }
    assert(publishedVerticalTrunk);
    std::size_t saplingLeaves = 0U;
    for (int z = 6; z <= 10; ++z) {
        for (int x = 6; x <= 10; ++x) {
            for (int y = 4; y <= 8; ++y) {
                if (saplingWorld.block(x, y, z) == mc::world::Block::OakLeaves) {
                    ++saplingLeaves;
                }
            }
        }
    }
    assert(saplingLeaves > 0U);
    assert(saplingSimulation.pendingTreeGrowthCount() == 0U);

    // A whole batch of saplings maturing together drains through the per-tick
    // growth cap instead of growing a forest in one frame, and every one still
    // ends up as a tree.
    mc::world::Chunk batchChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            batchChunk.setBlock(x, 0, z, mc::world::Block::Stone);
            batchChunk.setBlock(x, 1, z, mc::world::Block::Dirt);
        }
    }
    for (int z = 1; z <= 5; ++z) {
        for (int x = 1; x <= 10; ++x) {
            batchChunk.setBlock(x, 2, z, mc::world::Block::OakSapling);
        }
    }
    mc::world::World batchWorld;
    batchWorld.setChunk({0, 0}, std::move(batchChunk));
    mc::gameplay::WorldSimulation batchSimulation;
    batchSimulation.setRandomTickSpeed(1000);
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            batchWorld.setSkyLight(x, 3, z, 15U);
        }
    }
    for (int tick = 0; tick < 300; ++tick) {
        static_cast<void>(batchSimulation.tick(batchWorld));
        assert(batchSimulation.lastTreeGrowthsProcessed() <=
               mc::gameplay::WorldSimulation::kMaximumTreeGrowthsPerTick);
    }
    std::size_t batchGrown = 0U;
    for (int z = 1; z <= 5; ++z) {
        for (int x = 1; x <= 10; ++x) {
            if (batchWorld.block(x, 2, z) == mc::world::Block::OakLog) {
                ++batchGrown;
            }
        }
    }
    assert(batchGrown == 50U);
    assert(batchSimulation.pendingTreeGrowthCount() == 0U);

    // --- Crop farming ---

    // Crops grow when their own cell is lit enough — CropBlock#randomTick reads
    // getRawBrightness(pos, 0), not the cell above — advancing the age stored in
    // their orientation state until it reaches 7. randomTickSpeed 0 freezes
    // them, exactly like every other random-tick behaviour.
    {
        mc::world::Chunk farmChunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                farmChunk.setBlock(x, 0, z, mc::world::Block::Stone);
            }
        }
        farmChunk.setBlock(4, 1, 4, mc::world::Block::Farmland);
        farmChunk.setBlock(5, 1, 5, mc::world::Block::Farmland);
        farmChunk.setBlock(4, 2, 4, mc::world::Block::WheatCrops);
        farmChunk.setBlock(5, 2, 5, mc::world::Block::Carrots);
        mc::world::World farmWorld;
        farmWorld.setChunk({0, 0}, std::move(farmChunk));
        mc::gameplay::WorldSimulation farmSimulation;
        farmSimulation.setRandomTickSpeed(1000);
        farmWorld.setSkyLight(4, 2, 4, 15U);
        farmWorld.setSkyLight(5, 2, 5, 15U);
        for (int tick = 0; tick < 5000; ++tick) {
            static_cast<void>(farmSimulation.tick(farmWorld));
        }
        assert(farmWorld.state(4, 2, 4).age() == 7);
        assert(farmWorld.state(5, 2, 5).age() == 7);
        // A crop in the dark never grows.
        mc::world::Chunk darkChunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                darkChunk.setBlock(x, 0, z, mc::world::Block::Stone);
            }
        }
        darkChunk.setBlock(4, 1, 4, mc::world::Block::Farmland);
        darkChunk.setBlock(4, 2, 4, mc::world::Block::WheatCrops);
        mc::world::World darkWorld;
        darkWorld.setChunk({0, 0}, std::move(darkChunk));
        mc::gameplay::WorldSimulation darkSimulation;
        darkSimulation.setRandomTickSpeed(1000);
        for (int tick = 0; tick < 2000; ++tick) {
            static_cast<void>(darkSimulation.tick(darkWorld));
        }
        assert(darkWorld.state(4, 2, 4).age() == 0);
    }

    // Farmland moisture: water within four blocks hydrates the soil (jumping
    // straight to 7, as FarmlandBlock#randomTick does); dry farmland with
    // nothing on top reverts to dirt, but one holding a crop stays put.
    {
        mc::world::Chunk farmChunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                farmChunk.setBlock(x, 0, z, mc::world::Block::Stone);
            }
        }
        farmChunk.setBlock(4, 1, 4, mc::world::Block::Farmland);   // near the water
        farmChunk.setBlock(12, 1, 12, mc::world::Block::Farmland); // far: reverts
        farmChunk.setBlock(2, 1, 12, mc::world::Block::Farmland);  // far but cropped
        farmChunk.setBlock(2, 2, 12, mc::world::Block::Potatoes);
        farmChunk.setBlock(7, 1, 8, mc::world::Block::Water);
        mc::world::World farmWorld;
        farmWorld.setChunk({0, 0}, std::move(farmChunk));
        mc::gameplay::WorldSimulation farmSimulation;
        farmSimulation.setRandomTickSpeed(1000);
        for (int tick = 0; tick < 400; ++tick) {
            static_cast<void>(farmSimulation.tick(farmWorld));
        }
        assert(mc::world::isFarmland(farmWorld.block(4, 1, 4)));
        assert(farmWorld.state(4, 1, 4).moisture() == 7);
        assert(farmWorld.block(12, 1, 12) == mc::world::Block::Dirt);
        // A crop on top keeps the dry farmland from reverting.
        assert(mc::world::isFarmland(farmWorld.block(2, 1, 12)));
        assert(farmWorld.state(2, 1, 12).moisture() == 0);
    }

    // A crop pops off the moment its farmland is removed, and the dropped state
    // keeps the age the crop had grown to, so its loot rolls against the right
    // stage.
    {
        mc::world::Chunk cropChunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                cropChunk.setBlock(x, 0, z, mc::world::Block::Stone);
            }
        }
        cropChunk.setBlock(4, 1, 4, mc::world::Block::Farmland);
        cropChunk.setBlock(4, 2, 4, mc::world::Block::WheatCrops);
        mc::world::World cropWorld;
        cropWorld.setChunk({0, 0}, std::move(cropChunk));
        mc::gameplay::WorldSimulation cropSimulation;
        // Grow the crop to maturity first.
        cropSimulation.setRandomTickSpeed(1000);
        cropWorld.setSkyLight(4, 3, 4, 15U);
        for (int tick = 0; tick < 5000; ++tick) {
            static_cast<void>(cropSimulation.tick(cropWorld));
        }
        const int maturedAge = cropWorld.state(4, 2, 4).age();
        cropWorld.setBlock(4, 1, 4, mc::world::Block::Air);
        cropSimulation.notifyNeighborChanged(cropWorld, {4, 1, 4});
        bool poppedWithAge = false;
        for (int tick = 0; tick < 5; ++tick) {
            for (const auto& change : cropSimulation.tick(cropWorld)) {
                if (change.dropped.block() == mc::world::Block::WheatCrops) {
                    assert(change.dropped.age() == maturedAge);
                    poppedWithAge = true;
                }
            }
        }
        assert(cropWorld.block(4, 2, 4) == mc::world::Block::Air);
        assert(poppedWithAge);
    }

    // --- AR-CX2: sugar cane ---

    // Placement rule (SugarCaneBlock#canSurvive): a cane survives on soil/sand
    // beside water, or on another cane, and nowhere else.
    {
        using mc::world::Block;
        using mc::world::BlockOrientation;
        mc::world::Chunk caneChunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                caneChunk.setBlock(x, 0, z, Block::Stone);
            }
        }
        // Sand at (4,1,4) with water beside it: valid footing.
        caneChunk.setBlock(4, 1, 4, Block::Sand);
        caneChunk.setBlock(5, 1, 4, Block::Water);
        // Dirt at (8,1,8) with no water nearby: invalid.
        caneChunk.setBlock(8, 1, 8, Block::Dirt);
        // Stone at (12,1,12): never valid.
        caneChunk.setBlock(12, 1, 12, Block::Stone);
        mc::world::World caneWorld;
        caneWorld.setChunk({0, 0}, std::move(caneChunk));
        assert(mc::world::canBlockSurvive(caneWorld, {4, 2, 4}, Block::SugarCane,
                                          BlockOrientation::North));
        assert(!mc::world::canBlockSurvive(caneWorld, {8, 2, 8}, Block::SugarCane,
                                           BlockOrientation::North));
        assert(!mc::world::canBlockSurvive(caneWorld, {12, 2, 12}, Block::SugarCane,
                                           BlockOrientation::North));
        // A cane placed on the valid cell supports another cane on top of it.
        caneWorld.setBlock(4, 2, 4, Block::SugarCane);
        assert(mc::world::canBlockSurvive(caneWorld, {4, 3, 4}, Block::SugarCane,
                                          BlockOrientation::North));
    }

    // Growth: a single cane on watered sand grows up to a stack of three and no
    // further, driven purely by random ticks (deterministic mc::rng,墙钟-free).
    {
        using mc::world::Block;
        mc::world::Chunk caneChunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                caneChunk.setBlock(x, 0, z, Block::Stone);
            }
        }
        caneChunk.setBlock(4, 1, 4, Block::Sand);
        caneChunk.setBlock(5, 1, 4, Block::Water);
        caneChunk.setBlock(4, 2, 4, Block::SugarCane);
        mc::world::World caneWorld;
        caneWorld.setChunk({0, 0}, std::move(caneChunk));
        mc::gameplay::WorldSimulation caneSimulation;
        caneSimulation.setRandomTickSpeed(1000);
        for (int tick = 0; tick < 20000; ++tick) {
            static_cast<void>(caneSimulation.tick(caneWorld));
        }
        assert(caneWorld.block(4, 2, 4) == Block::SugarCane);
        assert(caneWorld.block(4, 3, 4) == Block::SugarCane);
        assert(caneWorld.block(4, 4, 4) == Block::SugarCane);
        // Never a fourth: the stack caps at three.
        assert(caneWorld.block(4, 5, 4) == Block::Air);
    }

    // A cane breaks and drops itself when its footing is removed (support pass +
    // dropsItem). Removing the base cane strands the stack above it.
    {
        using mc::world::Block;
        mc::world::Chunk caneChunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                caneChunk.setBlock(x, 0, z, Block::Stone);
            }
        }
        caneChunk.setBlock(4, 1, 4, Block::Sand);
        caneChunk.setBlock(5, 1, 4, Block::Water);
        caneChunk.setBlock(4, 2, 4, Block::SugarCane);
        caneChunk.setBlock(4, 3, 4, Block::SugarCane);
        mc::world::World caneWorld;
        caneWorld.setChunk({0, 0}, std::move(caneChunk));
        mc::gameplay::WorldSimulation caneSimulation;
        caneSimulation.setRandomTickSpeed(0); // isolate the support pass
        // Remove the sand: the whole cane column loses its footing.
        caneWorld.setBlock(4, 1, 4, Block::Air);
        caneSimulation.notifyNeighborChanged(caneWorld, {4, 1, 4});
        bool droppedCane = false;
        for (int tick = 0; tick < 8; ++tick) {
            for (const auto& change : caneSimulation.tick(caneWorld)) {
                if (change.dropped.block() == Block::SugarCane) {
                    droppedCane = true;
                }
            }
        }
        assert(caneWorld.block(4, 2, 4) == Block::Air);
        assert(caneWorld.block(4, 3, 4) == Block::Air);
        assert(droppedCane);
    }

    // --- AR-CX4-b: fire ---

    // Support rule (FireBlock#canSurvive): fire survives on a sturdy floor, or
    // beside a flammable neighbour, and nowhere with neither.
    {
        using mc::world::Block;
        using mc::world::BlockOrientation;
        mc::world::Chunk fireChunk;
        // A solid stone floor at y=1 across the chunk.
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                fireChunk.setBlock(x, 1, z, Block::Stone);
            }
        }
        // A wooden plank pillar with air on top: fire at (8,3,8) has no floor
        // below but a flammable plank beside it.
        fireChunk.setBlock(9, 3, 8, Block::OakPlanks);
        mc::world::World fireWorld;
        fireWorld.setChunk({0, 0}, std::move(fireChunk));
        // On top of stone: sturdy floor below.
        assert(mc::world::canBlockSurvive(fireWorld, {4, 2, 4}, Block::Fire,
                                          BlockOrientation::Up));
        // Beside a plank, no floor: a flammable neighbour keeps it.
        assert(mc::world::canBlockSurvive(fireWorld, {8, 3, 8}, Block::Fire,
                                          BlockOrientation::Up));
        // Mid-air over nothing flammable: cannot survive.
        assert(!mc::world::canBlockSurvive(fireWorld, {4, 6, 4}, Block::Fire,
                                           BlockOrientation::Up));
    }

    // Fire placed on a bare stone floor ages out to air on its own, driven only
    // by random ticks (deterministic, 墙钟-free), with nothing flammable to eat.
    {
        using mc::world::Block;
        mc::world::Chunk fireChunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                fireChunk.setBlock(x, 1, z, Block::Stone);
            }
        }
        fireChunk.setBlock(4, 2, 4, Block::Fire);
        mc::world::World fireWorld;
        fireWorld.setChunk({0, 0}, std::move(fireChunk));
        mc::gameplay::WorldSimulation fireSimulation;
        fireSimulation.setRandomTickSpeed(1000);
        assert(fireWorld.block(4, 2, 4) == Block::Fire);
        bool burnedOut = false;
        for (int tick = 0; tick < 20000 && !burnedOut; ++tick) {
            static_cast<void>(fireSimulation.tick(fireWorld));
            burnedOut = fireWorld.block(4, 2, 4) == Block::Air;
        }
        assert(burnedOut);
    }

    // Fire spreads to an adjacent flammable block: a plank next to the flame is
    // eventually replaced by a fresh fire.
    {
        using mc::world::Block;
        mc::world::Chunk fireChunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                fireChunk.setBlock(x, 1, z, Block::Stone);
            }
        }
        // A row of planks the fire can walk along, so at least one neighbour is
        // always flammable while the source burns.
        fireChunk.setBlock(5, 2, 4, Block::OakPlanks);
        fireChunk.setBlock(6, 2, 4, Block::OakPlanks);
        fireChunk.setBlock(4, 2, 4, Block::Fire);
        mc::world::World fireWorld;
        fireWorld.setChunk({0, 0}, std::move(fireChunk));
        mc::gameplay::WorldSimulation fireSimulation;
        fireSimulation.setRandomTickSpeed(1000);
        bool plankIgnited = false;
        for (int tick = 0; tick < 20000 && !plankIgnited; ++tick) {
            static_cast<void>(fireSimulation.tick(fireWorld));
            plankIgnited = fireWorld.block(5, 2, 4) == Block::Fire;
        }
        assert(plankIgnited);
    }

    // --- B1': the random-tick switch is now a table, and the draw loop rejects
    // a block with isRandomlyTicking before entering any call. The table and
    // the pre-filter are the same data, so they cannot drift — but the *set*
    // still can, and a block silently dropping out of it stops grass spreading
    // or crops growing with no other symptom. ---
    {
        using mc::world::Block;
        const Block ticking[] = {
            Block::Grass,         Block::Farmland,      Block::WheatCrops,
            Block::Carrots,       Block::Potatoes,      Block::OakSapling,
            Block::SpruceSapling, Block::BirchSapling,  Block::JungleSapling,
            Block::AcaciaSapling, Block::DarkOakSapling, Block::SugarCane,
            Block::Fire,
        };
        for (const auto block : ticking) {
            assert(mc::gameplay::WorldSimulation::isRandomlyTicking(block));
        }
        // The overwhelming majority must be rejected: this is the pre-filter's
        // whole purpose, and a table that answered true for stone would put the
        // simulation back to a call per draw.
        const Block inert[] = {
            Block::Air,   Block::Stone,     Block::Dirt,   Block::Cobblestone,
            Block::Sand,  Block::OakLog,    Block::Water,  Block::OakLeaves,
            Block::Chest, Block::Furnace,
        };
        for (const auto block : inert) {
            assert(!mc::gameplay::WorldSimulation::isRandomlyTicking(block));
        }
        // Every entry the table holds must also pass the pre-filter, and vice
        // versa — they are one array, and this pins that they stay one.
        std::size_t tickingCount = 0U;
        for (std::size_t index = 0; index < static_cast<std::size_t>(Block::Count); ++index) {
            if (mc::gameplay::WorldSimulation::isRandomlyTicking(static_cast<Block>(index))) {
                ++tickingCount;
            }
        }
        assert(tickingCount == std::size(ticking));
    }

    return 0;
}

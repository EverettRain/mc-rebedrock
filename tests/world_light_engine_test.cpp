#include "world/WorldLightEngine.hpp"
#include "world/WorldLighting.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <span>

int main() {
    mc::world::World world;
    world.setChunk({0, 0}, mc::world::Chunk{});
    world.setChunk({1, 0}, mc::world::Chunk{});
    mc::world::WorldLightEngine engine;
    const std::array chunks{mc::world::ChunkPosition{0, 0},
                            mc::world::ChunkPosition{1, 0}};
    engine.initializeChunks(world, std::span<const mc::world::ChunkPosition>{chunks});
    assert(world.skyLight(4, 100, 4) == 15U);
    assert(world.blockLight(4, 100, 4) == 0U);

    const auto expectMatchesReference = [&world, &chunks](int minimumX, int maximumX,
                                                          int minimumY, int maximumY,
                                                          int minimumZ, int maximumZ) {
        const mc::world::ChunkLightSampler reference{
            world, std::span<const mc::world::ChunkPosition>{chunks}};
        for (int z = minimumZ; z <= maximumZ; ++z) {
            for (int y = minimumY; y <= maximumY; ++y) {
                for (int x = minimumX; x <= maximumX; ++x) {
                    const auto expected = reference.level(x, y, z);
                    assert(world.skyLight(x, y, z) == expected.sky);
                    assert(world.blockLight(x, y, z) == expected.block);
                }
            }
        }
    };

    world.setBlock(15, 40, 8, mc::world::Block::Torch);
    engine.updateBlock(world, 15, 40, 8);
    assert(world.blockLight(15, 40, 8) == 14U);
    assert(world.blockLight(16, 40, 8) == 13U);
    assert(world.blockLight(20, 40, 8) == 9U);
    assert(engine.lastPropagationVisitCount() < 50'000U);
    assert(!engine.takeDirtySections().empty());
    expectMatchesReference(1, 29, 30, 50, 1, 15);

    world.setBlock(15, 40, 8, mc::world::Block::Air);
    engine.updateBlock(world, 15, 40, 8);
    assert(world.blockLight(15, 40, 8) == 0U);
    assert(world.blockLight(16, 40, 8) == 0U);
    assert(world.blockLight(20, 40, 8) == 0U);
    expectMatchesReference(1, 29, 30, 50, 1, 15);

    // A roof blocks direct skylight; removing it restores the whole column.
    world.setBlock(4, 120, 4, mc::world::Block::Stone);
    engine.updateBlock(world, 4, 120, 4);
    assert(world.skyLight(4, 120, 4) == 0U);
    assert(world.skyLight(4, 119, 4) == 14U); // lateral smooth-light ingress
    expectMatchesReference(1, 8, 115, 124, 1, 8);
    world.setBlock(4, 120, 4, mc::world::Block::Air);
    engine.updateBlock(world, 4, 120, 4);
    assert(world.skyLight(4, 119, 4) == 15U);

    // Java 26.1 does not attenuate unobstructed vertical skylight by depth. A
    // sealed stone shaft is dark, but opening its roof to the sky makes every
    // cell in the column level 15, even far below the surface. This guards
    // against adding an artificial depth fog/light rule to address reports
    // made while digging a shaft that is still open above.
    mc::world::World shaftWorld;
    mc::world::Chunk shaftChunk;
    for (int y = 20; y <= 30; ++y) {
        for (int z = 8; z <= 12; ++z) {
            for (int x = 8; x <= 12; ++x) {
                shaftChunk.setBlock(x, y, z, mc::world::Block::Stone);
            }
        }
    }
    for (int y = 21; y < 30; ++y) {
        shaftChunk.setBlock(10, y, 10, mc::world::Block::Air);
    }
    shaftWorld.setChunk({0, 0}, std::move(shaftChunk));
    mc::world::WorldLightEngine shaftEngine;
    const std::array shaftPosition{mc::world::ChunkPosition{0, 0}};
    shaftEngine.initializeChunks(
        shaftWorld, std::span<const mc::world::ChunkPosition>{shaftPosition});
    assert(shaftWorld.skyLight(10, 21, 10) == 0U);
    shaftWorld.setBlock(10, 30, 10, mc::world::Block::Air);
    shaftEngine.updateBlock(shaftWorld, 10, 30, 10);
    assert(shaftWorld.directSkyLight(10, 21, 10) == 15U);
    assert(shaftWorld.skyLight(10, 21, 10) == 15U);

    // Leaves attenuate direct sky by one level but remain light-propagating.
    world.setBlock(6, 120, 6, mc::world::Block::OakLeaves);
    engine.updateBlock(world, 6, 120, 6);
    assert(world.directSkyLight(6, 120, 6) == 14U);
    assert(world.skyLight(6, 120, 6) == 14U);

    // Regression for the former multi-second leaves path: a light update in
    // a dense 12x8x12 canopy must remain a bounded local propagation.
    mc::world::World canopyWorld;
    mc::world::Chunk canopyChunk;
    for (int y = 80; y < 88; ++y) {
        for (int z = 2; z < 14; ++z) {
            for (int x = 2; x < 14; ++x) {
                canopyChunk.setBlock(x, y, z, mc::world::Block::OakLeaves);
            }
        }
    }
    canopyWorld.setChunk({0, 0}, std::move(canopyChunk));
    mc::world::WorldLightEngine canopyEngine;
    const std::array canopyPosition{mc::world::ChunkPosition{0, 0}};
    canopyEngine.initializeChunks(
        canopyWorld, std::span<const mc::world::ChunkPosition>{canopyPosition});
    const auto canopyEditStarted = std::chrono::steady_clock::now();
    canopyWorld.setBlock(8, 84, 8, mc::world::Block::Air);
    canopyEngine.updateBlock(canopyWorld, 8, 84, 8);
    const auto canopyEditElapsed = std::chrono::steady_clock::now() - canopyEditStarted;
    assert(canopyEngine.lastPropagationVisitCount() < 100'000U);
    assert(canopyEditElapsed < std::chrono::milliseconds{250});
    return 0;
}

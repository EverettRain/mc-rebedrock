// Decisive diagnostic for the "grass stopped spreading" regression: does the
// real lighting pipeline give a surface grass block the light its random tick
// needs? Builds a flat grass plain, lights it with WorldLightEngine (exactly
// what the streaming worker does before it publishes chunks), carves a dirt
// strip, then runs the random-tick pass at speed 100 and reports spread/die.
// Prints the light the grass sees above it and the outcome.

#include "gameplay/WorldSimulation.hpp"

#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/WorldLightEngine.hpp"

#include <algorithm>
#include <iostream>
#include <utility>
#include <vector>

int main() {
    mc::world::World world;
    std::vector<mc::world::ChunkPosition> positions;
    for (int chunkZ = 0; chunkZ < 3; ++chunkZ) {
        for (int chunkX = 0; chunkX < 3; ++chunkX) {
            mc::world::Chunk chunk;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    for (int y = 0; y <= 60; ++y) {
                        chunk.setBlock(x, y, z, mc::world::Block::Stone);
                    }
                    chunk.setBlock(x, 61, z, mc::world::Block::Dirt);
                    chunk.setBlock(x, 62, z, mc::world::Block::Dirt);
                    chunk.setBlock(x, 63, z, mc::world::Block::Grass);
                }
            }
            world.setChunk({chunkX, chunkZ}, std::move(chunk));
            positions.push_back({chunkX, chunkZ});
        }
    }
    mc::world::WorldLightEngine lighting;
    lighting.initializeChunks(world, positions);

    // The light a surface grass block sees above it, straight from the engine.
    int brightAbove = 0;
    int total = 0;
    for (int x = 24; x <= 28; ++x) {
        for (int z = 24; z <= 28; ++z) {
            const int above = std::max(static_cast<int>(world.skyLight(x, 64, z)),
                                       static_cast<int>(world.blockLight(x, 64, z)));
            ++total;
            if (above >= 9) {
                ++brightAbove;
            }
        }
    }
    std::cout << "grass light probe: " << brightAbove << "/" << total
              << " surface air cells with light>=9\n";
    if (brightAbove == 0) {
        std::cout << "LIGHT FAILED: grass cannot see any light above it\n";
        return 3;
    }

    // Carve a dirt strip down the middle column (world x=24) so the grass on
    // either side has a clear spread target, then run the random-tick pass.
    for (int z = 0; z < 16; ++z) {
        world.setBlock(24, 63, 16 + z, mc::world::Block::Dirt);
    }
    mc::gameplay::WorldSimulation simulation;
    simulation.setRandomTickSpeed(100);
    std::size_t spread = 0U;
    std::size_t died = 0U;
    std::size_t peak = 0U;
    for (int tick = 0; tick < 2000; ++tick) {
        for (const auto& change : simulation.tick(world)) {
            if (change.state.block() == mc::world::Block::Grass) {
                ++spread;
            } else if (change.state.block() == mc::world::Block::Dirt) {
                ++died;
            }
        }
        peak = std::max(peak, simulation.lastRandomTickConversions());
    }
    std::cout << "after 2000 ticks at speed 100: " << spread << " dirt->grass, " << died
              << " grass->dirt, peak conversions/tick " << peak << '\n';
    // The dirt strip (16 cells) should end up grass; die should be ~0 on an open
    // sunlit plain.
    std::cout << (spread > 0U ? "SPREAD WORKS\n" : "SPREAD FAILED\n");
    return spread > 0U && died == 0U ? 0 : 2;
}

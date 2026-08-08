// Measures the render-thread cost of the random-tick pass over a large loaded
// area, the way it hits the real game: a flat plain of grass over dirt over
// stone with a raw-dirt strip, so grass random ticks both probe for spread and
// convert cells. Prints millisecond-per-tick at the vanilla speed and at a
// player-set speed of 100, where the per-pick cost used to eat a frame, plus a
// huge-field scenario that would otherwise flood the worker/GPU pipeline with
// conversions (and the per-tick conversion budget that caps it).
//
// This is a benchmark, not a correctness test: it asserts nothing beyond
// returning 0, and its wall-clock numbers are informational.

#include "gameplay/WorldSimulation.hpp"

#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace {

[[nodiscard]] mc::world::World makePlainWorld(int chunkRadius) {
    mc::world::World world;
    for (int chunkZ = -chunkRadius; chunkZ <= chunkRadius; ++chunkZ) {
        for (int chunkX = -chunkRadius; chunkX <= chunkRadius; ++chunkX) {
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
        }
    }
    // A ring of bare dirt around the centre chunk: a boundary grass keeps
    // probing against, so the pass produces steady conversions instead of a
    // dead scan.
    for (int z = -2; z <= 2; ++z) {
        for (int x = -2; x <= 2; ++x) {
            if (x >= -1 && x <= 1 && z >= -1 && z <= 1) {
                continue;
            }
            world.setBlock(x, 63, z, mc::world::Block::Dirt);
        }
    }
    return world;
}

// A world with a huge grass/dirt frontier down the middle: every grass random
// tick has dirt within its four probes, so without the per-tick conversion
// budget this is exactly the flood that used to stall the pipeline at speed 100.
[[nodiscard]] mc::world::World makeFrontierWorld(int chunkRadius) {
    mc::world::World world;
    for (int chunkZ = -chunkRadius; chunkZ <= chunkRadius; ++chunkZ) {
        for (int chunkX = -chunkRadius; chunkX <= chunkRadius; ++chunkX) {
            mc::world::Chunk chunk;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    for (int y = 0; y <= 60; ++y) {
                        chunk.setBlock(x, y, z, mc::world::Block::Stone);
                    }
                    chunk.setBlock(x, 61, z, mc::world::Block::Dirt);
                    chunk.setBlock(x, 62, z, mc::world::Block::Dirt);
                }
            }
            world.setChunk({chunkX, chunkZ}, std::move(chunk));
        }
    }
    // Everything west of the centre column is bare dirt, east is grass.
    for (int z = -16 * chunkRadius; z <= 16 * chunkRadius; ++z) {
        for (int x = -16 * chunkRadius; x <= 16 * chunkRadius; ++x) {
            world.setBlock(x, 63, z, x < 0 ? mc::world::Block::Dirt : mc::world::Block::Grass);
        }
    }
    return world;
}

void run(mc::world::World& world, int speed, int ticks, const char* label) {
    mc::gameplay::WorldSimulation simulation;
    simulation.setRandomTickSpeed(speed);
    std::size_t totalChanges = 0U;
    std::size_t maxConversionsPerTick = 0U;
    const auto start = std::chrono::steady_clock::now();
    for (int tick = 0; tick < ticks; ++tick) {
        totalChanges += simulation.tick(world).size();
        maxConversionsPerTick =
            std::max(maxConversionsPerTick, simulation.lastRandomTickConversions());
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double perTick =
        std::chrono::duration<double, std::milli>(elapsed).count() / static_cast<double>(ticks);
    std::cout << label << ": " << perTick << " ms/tick over " << ticks << " ticks, "
              << totalChanges << " total changes, peak "
              << maxConversionsPerTick << " grass conversions in one tick\n";
}

} // namespace

int main() {
    constexpr int chunkRadius = 3;  // 7x7 chunks, like a mid view distance.
    constexpr int kTicks = 2000;
    auto plain = makePlainWorld(chunkRadius);
    run(plain, 3, kTicks, "plain  speed   3");
    run(plain, 100, kTicks, "plain  speed 100");
    auto frontier = makeFrontierWorld(chunkRadius);
    run(frontier, 100, kTicks, "frontier speed 100");
    return 0;
}

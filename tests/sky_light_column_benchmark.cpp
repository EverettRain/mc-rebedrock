// Informational benchmark for the two light-engine paths RN-19a rewrites:
//
//   * WorldLightEngine::updateBlock — every gameplay block edit runs it, and its
//     first act is the sky column recompute. That recompute used to write a
//     nibble into all 384 cells of the column through World::setDirectSkyLight,
//     which is a World::chunk() lookup (an unordered_map::find plus a shared_ptr
//     use_count read) *per cell*; it is now one hoisted chunk pointer, one scan
//     that stops at the first occluded edge, and one integer store.
//   * WorldLightEngine::initializeChunks — the per-column pass that runs for
//     every streamed chunk. It lost a whole nibble array's worth of writes.
//
// This is a benchmark, not a correctness test: it asserts nothing and its
// wall-clock numbers are informational — compare relative to a prior run on the
// same machine (and only in a Release build), never against a hard target.

#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"
#include "world/WorldLightEngine.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {

using mc::world::Block;
using mc::world::Chunk;
using mc::world::ChunkPosition;
using mc::world::World;
using mc::world::WorldLightEngine;

constexpr int kChunkRadius = 2; // 5x5 chunks
constexpr int kSurfaceY = 70;

// A surface world: solid ground up to kSurfaceY, open sky above, a scattering of
// leaves over it so the column scan meets an occluded edge at a realistic depth
// rather than running the full world height every time.
[[nodiscard]] World makeSurfaceWorld() {
    World world;
    for (int chunkZ = -kChunkRadius; chunkZ <= kChunkRadius; ++chunkZ) {
        for (int chunkX = -kChunkRadius; chunkX <= kChunkRadius; ++chunkX) {
            Chunk chunk;
            for (int y = mc::world::kMinY; y <= kSurfaceY; ++y) {
                for (int z = 0; z < mc::world::kChunkDepth; ++z) {
                    for (int x = 0; x < mc::world::kChunkWidth; ++x) {
                        chunk.setBlock(x, y, z, Block::Stone);
                    }
                }
            }
            for (int z = 0; z < mc::world::kChunkDepth; z += 4) {
                for (int x = 0; x < mc::world::kChunkWidth; x += 4) {
                    chunk.setBlock(x, kSurfaceY + 5, z, Block::OakLeaves);
                }
            }
            world.setChunk({chunkX, chunkZ}, std::move(chunk));
        }
    }
    return world;
}

[[nodiscard]] std::vector<ChunkPosition> allPositions() {
    std::vector<ChunkPosition> positions;
    for (int chunkZ = -kChunkRadius; chunkZ <= kChunkRadius; ++chunkZ) {
        for (int chunkX = -kChunkRadius; chunkX <= kChunkRadius; ++chunkX) {
            positions.push_back({chunkX, chunkZ});
        }
    }
    return positions;
}

constexpr int kRepeats = 15;

void report(const std::string& label, double best, std::size_t count, const char* unit) {
    std::cout << label << ": " << best << " us/" << unit << " (best of " << kRepeats << " x "
              << count << ")\n";
}

// The hot path: a block edit that does not change the column at all (the random
// tick's bread and butter — a crop stage, grass spreading) still pays for the
// recompute, so this is the case the rewrite has to be cheap in.
void benchmarkEditNoColumnChange() {
    double best = std::numeric_limits<double>::max();
    constexpr std::size_t kEdits = 4096U;
    for (int repeat = 0; repeat < kRepeats; ++repeat) {
        World world = makeSurfaceWorld();
        WorldLightEngine engine;
        const auto positions = allPositions();
        engine.initializeChunks(world, std::span<const ChunkPosition>{positions});
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t edit = 0U; edit < kEdits; ++edit) {
            const int x = static_cast<int>(edit % 64U);
            const int z = static_cast<int>((edit / 64U) % 64U);
            world.setBlock(x, kSurfaceY, z, Block::Dirt);
            engine.updateBlock(world, x, kSurfaceY, z);
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const double perEdit = std::chrono::duration<double, std::micro>(elapsed).count() /
                               static_cast<double>(kEdits);
        best = perEdit < best ? perEdit : best;
    }
    report("edit, column unchanged ", best, kEdits, "edit");
}

// The other half: an edit that does move the column, so the seeds and the settle
// pass are paid for as well.
void benchmarkEditMovesColumn() {
    double best = std::numeric_limits<double>::max();
    constexpr std::size_t kEdits = 1024U;
    for (int repeat = 0; repeat < kRepeats; ++repeat) {
        World world = makeSurfaceWorld();
        WorldLightEngine engine;
        const auto positions = allPositions();
        engine.initializeChunks(world, std::span<const ChunkPosition>{positions});
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t edit = 0U; edit < kEdits; ++edit) {
            const int x = static_cast<int>(edit % 32U);
            const int z = static_cast<int>((edit / 32U) % 32U);
            world.setBlock(x, kSurfaceY + 1, z, Block::OakLeaves);
            engine.updateBlock(world, x, kSurfaceY + 1, z);
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const double perEdit = std::chrono::duration<double, std::micro>(elapsed).count() /
                               static_cast<double>(kEdits);
        best = perEdit < best ? perEdit : best;
    }
    report("edit, column moves     ", best, kEdits, "edit");
}

void benchmarkInitialize() {
    double best = std::numeric_limits<double>::max();
    const auto positions = allPositions();
    for (int repeat = 0; repeat < kRepeats; ++repeat) {
        World world = makeSurfaceWorld();
        WorldLightEngine engine;
        const auto start = std::chrono::steady_clock::now();
        engine.initializeChunks(world, std::span<const ChunkPosition>{positions});
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const double perChunk = std::chrono::duration<double, std::micro>(elapsed).count() /
                                static_cast<double>(positions.size());
        best = perChunk < best ? perChunk : best;
    }
    report("initializeChunks       ", best, positions.size(), "chunk");
}

} // namespace

int main() {
    benchmarkEditNoColumnChange();
    benchmarkEditMovesColumn();
    benchmarkInitialize();
    return 0;
}

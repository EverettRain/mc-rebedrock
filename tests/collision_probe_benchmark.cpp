// Informational benchmark for the two collision-probe loops AR-B4-0 touches:
// PlayerController::collidesAtHeight (reached through canStandUp, which is the
// same query with the standing height) and EntitySystem::boxIntersectsWorld
// (reached through canOccupy). Every player move step and every creature move
// step runs one of these, so the row-below scan B4-0 adds is on the hottest
// path the world geometry has.
//
// block_shape_benchmark measures the shape dispatch by replicating the loop;
// this one measures the loops themselves, which is where the prefilter
// (`hasTallCollision` per cell in one extra x*z row) actually lands.
//
// This is a benchmark, not a correctness test: it asserts nothing and its
// wall-clock numbers are informational — compare relative to a prior run on the
// same machine, never against a hard target.

#include "gameplay/EntitySystem.hpp"
#include "gameplay/PlayerController.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

namespace {

using mc::world::Block;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::World;

constexpr int kChunkRadius = 4; // 9x9 chunks, a generous loaded area.
constexpr int kFloorY = 4;

[[nodiscard]] World makeFlatWorld(Block floorBlock) {
    World world;
    for (int chunkZ = -kChunkRadius; chunkZ <= kChunkRadius; ++chunkZ) {
        for (int chunkX = -kChunkRadius; chunkX <= kChunkRadius; ++chunkX) {
            Chunk chunk;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    chunk.setBlock(x, kFloorY, z, floorBlock);
                }
            }
            world.setChunk({chunkX, chunkZ}, std::move(chunk));
        }
    }
    return world;
}

// A flat stone floor with a fence-gate lattice standing on it — the tall block
// the row-below scan exists for, so the "hit the prefilter and then pay for the
// shape" branch is measured, not only the "miss the prefilter" one.
[[nodiscard]] World makeGateWorld() {
    World world = makeFlatWorld(Block::Stone);
    const BlockState gate{Block::OakFenceGate};
    for (int z = -32; z < 32; ++z) {
        for (int x = -32; x < 32; ++x) {
            if (((x + z) & 3) == 0) {
                world.setState(x, kFloorY + 1, z, gate);
            }
        }
    }
    return world;
}

// The player probe: sweep a 64x64 grid of feet positions, one collidesAtHeight
// per position, standing on the floor so the query box straddles the cells that
// matter.
[[nodiscard]] std::size_t sweepPlayerProbe(const World& world, int steps) {
    mc::gameplay::PlayerController player({0.5F, static_cast<float>(kFloorY) + 1.0F, 0.5F});
    std::size_t fits = 0U;
    for (int step = 0; step < steps; ++step) {
        const float x = static_cast<float>(step % 64) - 32.0F + 0.5F;
        const float z = static_cast<float>((step / 64) % 64) - 32.0F + 0.5F;
        player.setPosition({x, static_cast<float>(kFloorY) + 1.0F, z});
        if (player.canStandUp(world)) {
            ++fits;
        }
    }
    return fits;
}

// The creature probe: the same sweep through EntitySystem::canOccupy, i.e.
// boxIntersectsWorld, at a zombie's 0.6 x 1.95 box.
[[nodiscard]] std::size_t sweepEntityProbe(const World& world, int steps) {
    constexpr mc::gameplay::entities::EntityDimensions kZombie{0.6F, 1.95F};
    std::size_t fits = 0U;
    for (int step = 0; step < steps; ++step) {
        const float x = static_cast<float>(step % 64) - 32.0F + 0.5F;
        const float z = static_cast<float>((step / 64) % 64) - 32.0F + 0.5F;
        if (mc::gameplay::EntitySystem::canOccupy(
                world, {x, static_cast<float>(kFloorY) + 1.0F, z}, kZombie)) {
            ++fits;
        }
    }
    return fits;
}

// Best-of-N rather than a single timing: at ~25 ns/probe the run-to-run spread
// from scheduling and frequency scaling is several tens of percent, which is an
// order of magnitude wider than the regression this is meant to resolve. The
// minimum is the least contaminated sample, and it is stable to well under a
// percent across repeats — the whole point of an A/B that has to answer "under
// 1%".
constexpr int kRepeats = 25;

template <typename Sweep>
void run(Sweep sweep, const World& world, int steps, const std::string& label) {
    double best = std::numeric_limits<double>::max();
    std::size_t fits = 0U;
    for (int repeat = 0; repeat < kRepeats; ++repeat) {
        const auto start = std::chrono::steady_clock::now();
        fits = sweep(world, steps);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const double perStep = std::chrono::duration<double, std::micro>(elapsed).count() /
                               static_cast<double>(steps);
        best = perStep < best ? perStep : best;
    }
    std::cout << label << ": " << best << " us/probe (best of " << kRepeats << " x " << steps
              << " probes), " << fits << " free\n";
}

} // namespace

int main() {
    constexpr int kSteps = 4096 * 16; // 64x64 grid, x16 per timed repeat.

    const auto airWorld = makeFlatWorld(Block::Air);
    const auto stoneWorld = makeFlatWorld(Block::Stone);
    const auto gateWorld = makeGateWorld();

    run(sweepPlayerProbe, airWorld, kSteps, "player probe / air        ");
    run(sweepPlayerProbe, stoneWorld, kSteps, "player probe / stone floor");
    run(sweepPlayerProbe, gateWorld, kSteps, "player probe / gate lattice");
    run(sweepEntityProbe, airWorld, kSteps, "entity probe / air        ");
    run(sweepEntityProbe, stoneWorld, kSteps, "entity probe / stone floor");
    run(sweepEntityProbe, gateWorld, kSteps, "entity probe / gate lattice");

    return 0;
}

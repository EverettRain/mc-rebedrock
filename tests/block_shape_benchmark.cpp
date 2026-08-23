// Informational benchmark for the BlockShape single-source hot path (see
// docs/content-dev/AR-content-realization/AR-B1-shaped-blocks-finalize.md,
// Slice D). BlockShape.hpp replaced five independent per-consumer switches
// (collisionSpan, the VoxelRaycast pick-ray switch, the selection-outline
// switch, appendSlab in the mesher) with one `blockShape(state)` behind a
// per-model function-pointer table (`kShapeByModel`, indexed by BlockModel
// ordinal — the same DOD move `kRandomTickTable` makes for random ticks).
//
// This measures whether that unification cost the walk anything: the exact
// access pattern PlayerController::collidesAtHeight and EntitySystem's
// collision walk use per physics tick — a 3D cell range, `collisionShape` (or
// `collisionSpan`) per cell, `shapeOverlaps` against the query box — over
// worlds dominated by each ShapeKind in turn (Empty air, Column stone/slabs,
// Boxes chests). A regression here is a regression in every entity's every
// tick, not a one-off cost.
//
// This is a benchmark, not a correctness test: it asserts nothing beyond
// returning 0 (block_shape_test.cpp owns correctness), and its wall-clock
// numbers are informational — compare relative to a prior run on the same
// machine, never against a hard FPS/ms target.

#include "world/Block.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

using mc::world::Block;
using mc::world::BlockShape;
using mc::world::Chunk;
using mc::world::ShapeKind;
using mc::world::World;

constexpr int kChunkRadius = 4; // 9x9 chunks, a generous loaded area.
constexpr int kFloorY = 4;
constexpr int kSpanY = 3; // scan 3 cells tall, like a player's collision box.

// A flat floor of `floorBlock` at kFloorY, everything else air. Column-heavy
// (a full cube) or Empty-heavy (air) depending on the floor block's shape.
[[nodiscard]] World makeFlatWorld(Block floorBlock, int chunkRadius) {
    World world;
    for (int chunkZ = -chunkRadius; chunkZ <= chunkRadius; ++chunkZ) {
        for (int chunkX = -chunkRadius; chunkX <= chunkRadius; ++chunkX) {
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

// A checkerboard of chests and air at kFloorY: the Boxes-with-collision case
// (chest's 14/16 box), alternated so every other cell forces the box-list
// path in shapeOverlaps rather than the Column fast path.
[[nodiscard]] World makeBoxesWorld(int chunkRadius) {
    World world;
    for (int chunkZ = -chunkRadius; chunkZ <= chunkRadius; ++chunkZ) {
        for (int chunkX = -chunkRadius; chunkX <= chunkRadius; ++chunkX) {
            Chunk chunk;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    const int worldX = chunkX * 16 + x;
                    const int worldZ = chunkZ * 16 + z;
                    if (((worldX + worldZ) & 1) == 0) {
                        chunk.setBlock(x, kFloorY, z, Block::Chest);
                    }
                }
            }
            world.setChunk({chunkX, chunkZ}, std::move(chunk));
        }
    }
    return world;
}

// Replicates PlayerController::collidesAtHeight's inner loop: for each of
// `steps` query boxes swept across the floor, walk the 3D cell range and test
// shapeOverlaps, exactly the per-tick collision-walk access pattern
// (blockShape dispatch -> collisionShape filter -> shapeOverlaps test).
[[nodiscard]] std::size_t sweepCollisionWalk(const World& world, int steps) {
    std::size_t hits = 0U;
    for (int step = 0; step < steps; ++step) {
        const float qMinX = static_cast<float>(step % 64) + 0.3F;
        const float qMinZ = static_cast<float>((step / 64) % 64) + 0.3F;
        const float qMaxX = qMinX + 0.6F; // ~player half-width*2
        const float qMaxZ = qMinZ + 0.6F;
        const float qMinY = static_cast<float>(kFloorY) - 1.0F;
        const float qMaxY = qMinY + static_cast<float>(kSpanY);

        const int minX = static_cast<int>(qMinX);
        const int maxX = static_cast<int>(qMaxX);
        const int minZ = static_cast<int>(qMinZ);
        const int maxZ = static_cast<int>(qMaxZ);
        const int minY = static_cast<int>(qMinY);
        const int maxY = static_cast<int>(qMaxY);

        for (int y = minY; y <= maxY; ++y) {
            for (int z = minZ; z <= maxZ; ++z) {
                for (int x = minX; x <= maxX; ++x) {
                    const BlockShape shape =
                        mc::world::collisionShape(world.state(x, y, z));
                    if (mc::world::shapeOverlaps(
                            shape, static_cast<float>(x), static_cast<float>(y),
                            static_cast<float>(z), qMinX, qMinY, qMinZ, qMaxX, qMaxY, qMaxZ)) {
                        ++hits;
                    }
                }
            }
        }
    }
    return hits;
}

void run(const World& world, int steps, const std::string& label) {
    const auto start = std::chrono::steady_clock::now();
    const std::size_t hits = sweepCollisionWalk(world, steps);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double perStep =
        std::chrono::duration<double, std::micro>(elapsed).count() / static_cast<double>(steps);
    std::cout << label << ": " << perStep << " us/step over " << steps << " steps, " << hits
              << " overlap hits\n";
}

} // namespace

int main() {
    constexpr int kSteps = 4096; // 64x64 swept grid, one query box per step.

    // ShapeKind::Empty dominant: every cell above/around the thin floor is air.
    auto emptyWorld = makeFlatWorld(Block::Air, kChunkRadius);
    run(emptyWorld, kSteps, "empty (air)          ");

    // ShapeKind::Column dominant: the ordinary full-cube fast path.
    auto columnWorld = makeFlatWorld(Block::Stone, kChunkRadius);
    run(columnWorld, kSteps, "column (stone)       ");

    // ShapeKind::Column, non-trivial span: a bottom slab, exercising the
    // Column branch with a fractional top rather than the {0,1} common case.
    {
        World slabWorld;
        for (int chunkZ = -kChunkRadius; chunkZ <= kChunkRadius; ++chunkZ) {
            for (int chunkX = -kChunkRadius; chunkX <= kChunkRadius; ++chunkX) {
                Chunk chunk;
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        chunk.setBlock(x, kFloorY, z, Block::OakSlab);
                    }
                }
                slabWorld.setChunk({chunkX, chunkZ}, std::move(chunk));
            }
        }
        run(slabWorld, kSteps, "column (slab, 0.5 top)");
    }

    // ShapeKind::Boxes dominant: chests, the box-list path in shapeOverlaps.
    auto boxesWorld = makeBoxesWorld(kChunkRadius);
    run(boxesWorld, kSteps, "boxes (chest)        ");

    // AR-B2: ShapeKind::Boxes, the widest entry the table now dispatches — an
    // inner-corner stair's 3-box list (vs. the chest's fixed 1), the new
    // content that grew kShapeByModel from 6 to 9 rows. Same access pattern,
    // worst-case box count, so a regression from the table's growth or the
    // stair handler's own cost (an interned-table subscript, not a per-call
    // rotation) would show here relative to the chest row above.
    {
        World stairWorld;
        const auto innerStair =
            mc::world::BlockState{Block::OakStairs}.withStairShape(mc::world::StairShape::InnerLeft);
        for (int chunkZ = -kChunkRadius; chunkZ <= kChunkRadius; ++chunkZ) {
            for (int chunkX = -kChunkRadius; chunkX <= kChunkRadius; ++chunkX) {
                Chunk chunk;
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        chunk.setState(x, kFloorY, z, innerStair);
                    }
                }
                stairWorld.setChunk({chunkX, chunkZ}, std::move(chunk));
            }
        }
        run(stairWorld, kSteps, "boxes (stair, 3-box) ");
    }

    return 0;
}

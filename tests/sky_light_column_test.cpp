// RN-19a: 26.1's sky light is a binary *source column* plus ordinary
// propagation, not a per-cell "remaining direct value" that stops decaying once
// it has passed the last dampening block. These assertions pin the difference,
// which is the whole of the reported bug: light leaking, undimmed, to the bottom
// of the world through anything that is not a full opaque cube.
//
// Every column test runs inside a sealed 1x1 shaft. Without the stone walls the
// assertions would be meaningless: an isolated leaf block in an otherwise empty
// world has four open-sky neighbour columns at 15, and horizontal propagation
// would light the "dark" cell to 14 no matter what the vertical model does.

#include "world/Chunk.hpp"
#include "world/NibbleArray.hpp"
#include "world/SkyColumn.hpp"
#include "world/World.hpp"
#include "world/WorldLightEngine.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <span>

namespace {

using mc::world::Block;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::ChunkPosition;
using mc::world::SlabPortion;
using mc::world::SubmergedFluid;
using mc::world::World;
using mc::world::WorldLightEngine;

constexpr int kShaftX = 8;
constexpr int kShaftZ = 8;
constexpr int kShaftTopY = 79; // solid up to here, open sky above
constexpr int kTestY = 70;     // where each test puts its one block

// A single chunk of solid stone from the world bottom to kShaftTopY with one
// 1x1 column carved out of it, open to the sky above kShaftTopY.
[[nodiscard]] World makeShaftWorld() {
    World world;
    Chunk chunk;
    for (int y = mc::world::kMinY; y <= kShaftTopY; ++y) {
        for (int z = 0; z < mc::world::kChunkDepth; ++z) {
            for (int x = 0; x < mc::world::kChunkWidth; ++x) {
                chunk.setBlock(x, y, z, Block::Stone);
            }
        }
    }
    for (int y = mc::world::kMinY; y <= kShaftTopY; ++y) {
        chunk.setBlock(kShaftX, y, kShaftZ, Block::Air);
    }
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

void light(World& world, WorldLightEngine& engine) {
    const std::array position{ChunkPosition{0, 0}};
    engine.initializeChunks(world, std::span<const ChunkPosition>{position});
}

[[nodiscard]] std::uint8_t shaftSky(const World& world, int y) {
    return world.skyLight(kShaftX, y, kShaftZ);
}

// A stair, not a slab, carries both shape tests below. In this project a slab
// renders in the Opaque bucket and `skyLightOpacity` is derived from that bucket,
// so every slab already answers 15 — dampening, not shape, ends its column, and
// the shape criterion could not be observed through one. A stair is Cutout, so
// its opacity is 0 and its *shape* is the only thing that can end a column,
// which is exactly the case this node exists to fix. (That slabs and farmland
// block sky light outright is a separate defect of the one-field
// renderLayer/opacity conflation, out of RN-19a's scope; see the landing note.)
[[nodiscard]] BlockState stair(SlabPortion half) {
    return BlockState{Block::OakStairs, mc::world::defaultOrientation(Block::OakStairs), 0U}
        .withStairHalf(half);
}

// 1. Leaves dampen, so they end the source column: the cells below them start at
// 14 and lose one level per cell, reaching 0 fourteen cells down. The old model
// carried 14 all the way to the world bottom — this cell read 14, not 0.
void testDampeningEndsTheColumn() {
    World world = makeShaftWorld();
    world.setBlock(kShaftX, kTestY, kShaftZ, Block::OakLeaves);
    WorldLightEngine engine;
    light(world, engine);

    assert(world.lowestSourceY(kShaftX, kShaftZ) == kTestY + 1);
    assert(shaftSky(world, kTestY + 1) == 15U);           // last source cell
    assert(shaftSky(world, kTestY) == 14U);               // the leaves themselves: 15 - max(1, 1)
    assert(shaftSky(world, kTestY - 1) == 13U);
    assert(shaftSky(world, kTestY - 13) == 1U);
    assert(shaftSky(world, kTestY - 14) == 0U);           // fourteen cells and it is gone
    assert(shaftSky(world, 55) == 0U);
    assert(shaftSky(world, mc::world::kMinY) == 0U);
}

// 2. Shape ends the column too — a stair dampens nothing at all, so only its
// geometry can — and *where* it ends differs by exactly one cell between the two
// halves. A bottom stair seals only the edge beneath itself, so it is its own
// column's lowest source and reads 15; a top stair seals the edge above itself,
// so the run stops one cell higher and the stair's own cell is merely propagated
// into and reads 14. Asserting only "deep down it is dark" would hold for both
// and pin neither — and would also hold for a criterion built on RN-8a's face
// mask, which is gated by canOcclude and therefore identically zero for a Cutout
// stair. The 15/14 pair is what separates a working criterion from that one.
void testShapeEndsTheColumnOneCellApart() {
    World bottomWorld = makeShaftWorld();
    bottomWorld.setState(kShaftX, kTestY, kShaftZ, stair(SlabPortion::Bottom));
    WorldLightEngine bottomEngine;
    light(bottomWorld, bottomEngine);

    assert(bottomWorld.lowestSourceY(kShaftX, kShaftZ) == kTestY);
    assert(bottomWorld.skyLight(kShaftX, kTestY, kShaftZ) == 15U);     // a source itself
    assert(bottomWorld.skyLight(kShaftX, kTestY - 1, kShaftZ) == 14U);
    assert(bottomWorld.skyLight(kShaftX, kTestY - 14, kShaftZ) == 1U);
    assert(bottomWorld.skyLight(kShaftX, kTestY - 15, kShaftZ) == 0U);

    World topWorld = makeShaftWorld();
    topWorld.setState(kShaftX, kTestY, kShaftZ, stair(SlabPortion::Top));
    WorldLightEngine topEngine;
    light(topWorld, topEngine);

    assert(topWorld.lowestSourceY(kShaftX, kShaftZ) == kTestY + 1);    // one cell higher
    assert(topWorld.skyLight(kShaftX, kTestY + 1, kShaftZ) == 15U);
    assert(topWorld.skyLight(kShaftX, kTestY, kShaftZ) == 14U);        // not a source
    assert(topWorld.skyLight(kShaftX, kTestY - 1, kShaftZ) == 13U);
    assert(topWorld.skyLight(kShaftX, kTestY - 14, kShaftZ) == 0U);
}

// 3. An unobstructed column is lit to the bottom of the world. This is not the
// bug — 26.1 does exactly this — and the fix must not "improve" it into a depth
// fade.
void testOpenColumnReachesTheWorldBottom() {
    World world;
    world.setChunk({0, 0}, Chunk{});
    WorldLightEngine engine;
    light(world, engine);

    assert(world.lowestSourceY(4, 4) == mc::world::kMinY);
    assert(world.skyLight(4, mc::world::kMinY, 4) == 15U);
    assert(world.skyLight(4, 100, 4) == 15U);
    assert(world.canSeeSky(4, mc::world::kMinY, 4));

    // The same holds through a shaft with nothing in it: this is the case the
    // dampening and shape tests above are measured against.
    World shaft = makeShaftWorld();
    WorldLightEngine shaftEngine;
    light(shaft, shaftEngine);
    assert(shaft.lowestSourceY(kShaftX, kShaftZ) == mc::world::kMinY);
    assert(shaft.skyLight(kShaftX, mc::world::kMinY, kShaftZ) == 15U);

    // ... and through glass, which is the case that pins the shape criterion's
    // canOcclude gate. Glass is a full cube, so its bare geometry seals both
    // vertical faces — but it neither dampens (26.1's TransparentBlock
    // propagatesSkylightDown, so lightDampening is 0) nor occludes
    // (`noOcclusion()`), and vanilla therefore does not end the column at it.
    // Reading the shape without the gate would fade every cell under a glass
    // roof, which is the failure mode this assertion exists to catch.
    World glassWorld = makeShaftWorld();
    glassWorld.setBlock(kShaftX, kTestY, kShaftZ, Block::Glass);
    WorldLightEngine glassEngine;
    light(glassWorld, glassEngine);
    assert(glassWorld.lowestSourceY(kShaftX, kShaftZ) == mc::world::kMinY);
    assert(glassWorld.skyLight(kShaftX, kTestY - 1, kShaftZ) == 15U);
    assert(glassWorld.skyLight(kShaftX, mc::world::kMinY, kShaftZ) == 15U);
}

// 4. Submerging a stair changes its dampening without changing its block, its
// emission or its shape. Both halves of that have to agree: the column criterion
// reads the state (a submerged stair ends the column one cell above where a dry
// one does), and ChunkStreamer's edit gate has to see the same change or it
// skips the light update altogether and leaves the column stale.
void testSubmergedStairUpdatesTheColumn() {
    World world = makeShaftWorld();
    const BlockState dry = stair(SlabPortion::Bottom);
    const BlockState wet = dry.withSubmergedFluid(SubmergedFluid::Water);
    world.setState(kShaftX, kTestY, kShaftZ, dry);
    WorldLightEngine engine;
    light(world, engine);
    assert(world.lowestSourceY(kShaftX, kShaftZ) == kTestY);
    assert(world.skyLight(kShaftX, kTestY, kShaftZ) == 15U);

    world.setState(kShaftX, kTestY, kShaftZ, wet);
    engine.updateBlock(world, kShaftX, kTestY, kShaftZ);
    // Water's own filter now ends the column one cell higher, and the slab's own
    // cell drops to 15 - max(1, 1).
    assert(world.lowestSourceY(kShaftX, kShaftZ) == kTestY + 1);
    assert(world.skyLight(kShaftX, kTestY, kShaftZ) == 14U);
    assert(world.skyLight(kShaftX, kTestY - 1, kShaftZ) == 13U);

    // The gate ChunkStreamer applies before it calls updateBlock at all. Reading
    // opacity off the Block instead of the BlockState makes these two equal and
    // the edit above would never reach the light engine.
    assert(mc::world::skyColumnSignature(dry) != mc::world::skyColumnSignature(wet));
    // ... and the shape half of the same gate: air and a dry bottom stair are
    // both undampening and unlit, and only one of them ends a column.
    assert(mc::world::skyColumnSignature(BlockState{}) !=
           mc::world::skyColumnSignature(dry));

    // Draining it puts the column back exactly where it was.
    world.setState(kShaftX, kTestY, kShaftZ, dry);
    engine.updateBlock(world, kShaftX, kTestY, kShaftZ);
    assert(world.lowestSourceY(kShaftX, kShaftZ) == kTestY);
    assert(world.skyLight(kShaftX, kTestY, kShaftZ) == 15U);
}

// 5. The memory contract the heightmap replaced: a section carries two light
// nibble arrays, not three, and the column data is one int16 per column for the
// whole chunk rather than a nibble per cell.
void testLightMemoryContract() {
    World world = makeShaftWorld();
    WorldLightEngine engine;
    light(world, engine);

    const Chunk* chunk = world.chunk({0, 0});
    assert(chunk != nullptr);
    // The shaft's sections hold both a non-uniform sky array (the carved column
    // differs from the stone around it) and a uniform block-light one.
    const std::size_t sectionBytes =
        chunk->section(mc::world::sectionIndexFromWorldY(kTestY)).lightHeapBytes();
    assert(sectionBytes <= 2U * mc::world::NibbleArray::kByteCount);
    assert(sectionBytes >= mc::world::NibbleArray::kByteCount);

    // 256 independently addressable columns, one int16 each: 512 B per chunk
    // against the 2 KB per section (48 KB per chunk) the third nibble array cost.
    static_assert(sizeof(std::int16_t) * mc::world::kChunkWidth * mc::world::kChunkDepth ==
                  512U);
    World scratch;
    scratch.setChunk({0, 0}, Chunk{});
    Chunk* writable = scratch.chunk({0, 0});
    assert(writable != nullptr);
    for (int z = 0; z < mc::world::kChunkDepth; ++z) {
        for (int x = 0; x < mc::world::kChunkWidth; ++x) {
            writable->setLowestSourceY(x, z, z * mc::world::kChunkWidth + x);
        }
    }
    for (int z = 0; z < mc::world::kChunkDepth; ++z) {
        for (int x = 0; x < mc::world::kChunkWidth; ++x) {
            assert(writable->lowestSourceY(x, z) == z * mc::world::kChunkWidth + x);
        }
    }
}

} // namespace

int main() {
    testDampeningEndsTheColumn();
    testShapeEndsTheColumnOneCellApart();
    testOpenColumnReachesTheWorldBottom();
    testSubmergedStairUpdatesTheColumn();
    testLightMemoryContract();
    return 0;
}

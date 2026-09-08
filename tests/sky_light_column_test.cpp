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

// 形状那两条判据下面用**楼梯和台阶各跑一遍**。
//
// 这段注释原本写的是「只能用楼梯，不能用台阶」——因为当时台阶渲染在 Opaque 桶里、
// 而 `skyLightOpacity` 是从那个桶推出来的，于是每一块台阶都答 15，是**衰减**而不是
// 形状结束了它的源柱，形状判据在台阶身上根本观测不到。那条绕行如今不再需要：
// RN-8f 把光的不透明度从渲染分桶拆成了独立的一轴（26.1 `getLightDampening` 问的是
// 遮挡形状是不是满方块），台阶因此和楼梯给出**同一组**答案——vanilla 里它们本来就
// 走同一条默认规则。两个都跑，正是这条修复的端到端判据。
[[nodiscard]] BlockState stair(SlabPortion half) {
    return BlockState{Block::OakStairs, mc::world::defaultOrientation(Block::OakStairs), 0U}
        .withStairHalf(half);
}

[[nodiscard]] BlockState slab(SlabPortion half) {
    return BlockState{Block::OakSlab, mc::world::defaultOrientation(Block::OakSlab), 0U}
        .withSlabPortion(half);
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
void shapeEndsTheColumnFor(BlockState (*shaped)(SlabPortion)) {
    World bottomWorld = makeShaftWorld();
    bottomWorld.setState(kShaftX, kTestY, kShaftZ, shaped(SlabPortion::Bottom));
    WorldLightEngine bottomEngine;
    light(bottomWorld, bottomEngine);

    assert(bottomWorld.lowestSourceY(kShaftX, kShaftZ) == kTestY);
    assert(bottomWorld.skyLight(kShaftX, kTestY, kShaftZ) == 15U);     // a source itself
    // ★ 这三条原本是 14 / 1 / 0，也就是「光从下半砖穿下去、每格 −1」。那是**缺一道门**
    // 的样子：26.1 的传播带逐面形状门（`SkyLightEngine:158` → `LightEngine.shapeOccludes`），
    // 下半砖朝下那面是整块正方形，光进不到它下面那一格。用台阶封顶的屋子在原版里是黑的。
    // RN-8f 只修了衰减值，那道门是补完它的另一半。
    assert(bottomWorld.skyLight(kShaftX, kTestY - 1, kShaftZ) == 0U);
    assert(bottomWorld.skyLight(kShaftX, kTestY - 14, kShaftZ) == 0U);

    World topWorld = makeShaftWorld();
    topWorld.setState(kShaftX, kTestY, kShaftZ, shaped(SlabPortion::Top));
    WorldLightEngine topEngine;
    light(topWorld, topEngine);

    assert(topWorld.lowestSourceY(kShaftX, kShaftZ) == kTestY + 1);    // one cell higher
    assert(topWorld.skyLight(kShaftX, kTestY + 1, kShaftZ) == 15U);
    // 上半砖朝上那面是整块正方形，所以光连它**自己那一格**都进不去（那一格的空气在
    // 砖的下面）。原本这里写 14，同样是缺门的样子。两半的判别因此比从前更锐利：
    // 同一格，下半砖 15、上半砖 0——「源柱在哪结束」这件事仍然被钉住，而且钉得更死。
    assert(topWorld.skyLight(kShaftX, kTestY, kShaftZ) == 0U);
    assert(topWorld.skyLight(kShaftX, kTestY - 1, kShaftZ) == 0U);
}

void testShapeEndsTheColumnOneCellApart() {
    shapeEndsTheColumnFor(stair);
    // RN-8f：台阶如今走同一条规则、给同一组数字。这不是「顺手多测一种方块」——
    // 在 RN-8f 之前台阶答 15，上面每一条断言都会红（源柱会停在它上面一格，
    // 它自己是 0 而不是 15，下面十四格全黑）。
    shapeEndsTheColumnFor(slab);
}

// 2b. 同一组数字，但方块是**放下去**的，不是初始填充出来的。
//
// 这一段是补出来的。上面那条走 `initializeChunks`，而初始填充与编辑是引擎里两条
// 不同的路：填充走 BFS 的 `propagateIncreases`，编辑走 `desiredLevel` 的重算。
// 两条各有一处「这一格不透光」的短路，于是把其中**一处**改回问渲染分桶时，
// 另一处仍然给出正确答案，测试是绿的——sabotage 就这样溜过去了一次。
//
// 而玩家的动作恰恰是「放下一块台阶」，也就是编辑那条路。两条都要有夹具。
void testShapeEndsTheColumnAfterPlacement() {
    for (const auto shaped : {stair, slab}) {
        World world = makeShaftWorld();
        WorldLightEngine engine;
        light(world, engine);
        // 放之前：整条竖井通到底都是源。
        assert(shaftSky(world, kTestY) == 15U);
        assert(shaftSky(world, kTestY - 1) == 15U);

        world.setState(kShaftX, kTestY, kShaftZ, shaped(SlabPortion::Bottom));
        engine.updateBlock(world, kShaftX, kTestY, kShaftZ);

        // 放之后：下半砖自己仍是源（15），但它封住了自己的底面，下面那一格从 15
        // **掉到 0**。这一路是**递减**，走的正是 `desiredLevel` 的重算，与上面那条
        // 初始填充的 BFS 是引擎里两条不同的路。
        assert(world.lowestSourceY(kShaftX, kShaftZ) == kTestY);
        assert(shaftSky(world, kTestY) == 15U);
        assert(shaftSky(world, kTestY - 1) == 0U);
        assert(shaftSky(world, kTestY - 5) == 0U);
    }

    // 2c. 门问的是**形状**，不是衰减。
    //
    // 玻璃的衰减同样是 0，但它 `noOcclusion`，没有任何一面挡光——光照穿而过。
    // 少了这条，「衰减为 0 就不挡光」这种看似合理的实现也能让上面两段全绿。
    {
        World world = makeShaftWorld();
        world.setBlock(kShaftX, kTestY, kShaftZ, Block::Glass);
        WorldLightEngine engine;
        light(world, engine);
        assert(mc::world::skyLightOpacity(Block::Glass) == 0U);   // 与台阶同为 0
        assert(world.lowestSourceY(kShaftX, kShaftZ) <= kTestY);
        assert(shaftSky(world, kTestY) == 15U);
        assert(shaftSky(world, kTestY - 1) == 15U && "玻璃不挡光，源柱直通到底");
    }
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
    // Water's own filter now ends the column one cell higher, and the stair's own
    // cell drops to 15 - max(1, 1).
    assert(world.lowestSourceY(kShaftX, kShaftZ) == kTestY + 1);
    assert(world.skyLight(kShaftX, kTestY, kShaftZ) == 14U);
    // 下面那一格是 0 而不是 13：注水改的是**衰减**，不是形状——楼梯的底面照样是
    // 整块正方形，逐面形状门照样挡住向下那一步。这条原本写 13，是缺门时的样子。
    assert(world.skyLight(kShaftX, kTestY - 1, kShaftZ) == 0U);

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
    testShapeEndsTheColumnAfterPlacement();
    testOpenColumnReachesTheWorldBottom();
    testSubmergedStairUpdatesTheColumn();
    testLightMemoryContract();
    return 0;
}

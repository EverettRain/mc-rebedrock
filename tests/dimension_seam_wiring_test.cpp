// WG-4: DimensionGenerator seam wiring + per-dimension world height.
//
// WG-4 fills the DIM-3 seam — it binds the WG-2/3 generators behind
// DimensionChunkGenerator, flips hasTerrainGenerator true for the Nether/End, and
// makes the chunk streamer dimension-aware. This test is the headless acceptance:
//   1. the binding produces *real* terrain per dimension (netherrack / end_stone),
//      never fabricated air — the "实流送" unlock;
//   2. each dimension generates to its DimensionType height (the Nether capped at
//      its 128 logical roof, the End over its 256, the Overworld unchanged);
//   3. the DimensionChunkGenerator dispatch matches the standalone WG-1/2/3
//      generators byte-for-byte (it is a binding, not a second algorithm);
//   4. determinism: same world seed ⇒ identical dimension chunks.

#include "world/DimensionChunkGenerator.hpp"
#include "world/DimensionGenerator.hpp"
#include "world/EndGenerator.hpp"
#include "world/NetherGenerator.hpp"
#include "world/SurfaceGenerator.hpp"
#include "world/WorldConstants.hpp"

#include <cassert>
#include <cstdint>

namespace {

using mc::world::Block;
using mc::world::Chunk;
using mc::world::DimensionChunkGenerator;
using mc::world::DimensionId;
using mc::world::kChunkDepth;
using mc::world::kChunkWidth;
using mc::world::kMaxY;
using mc::world::kMinY;

[[nodiscard]] long count(const Chunk& chunk, Block block) {
    long n = 0;
    for (int x = 0; x < kChunkWidth; ++x) {
        for (int z = 0; z < kChunkDepth; ++z) {
            for (int y = kMinY; y < kMaxY; ++y) {
                if (chunk.block(x, y, z) == block) {
                    ++n;
                }
            }
        }
    }
    return n;
}

[[nodiscard]] bool chunksEqual(const Chunk& a, const Chunk& b) {
    for (int x = 0; x < kChunkWidth; ++x) {
        for (int z = 0; z < kChunkDepth; ++z) {
            for (int y = kMinY; y < kMaxY; ++y) {
                if (a.block(x, y, z) != b.block(x, y, z)) {
                    return false;
                }
            }
        }
    }
    return true;
}

// 1. The binding produces real terrain per dimension — the streamer, handed a
// DimensionChunkGenerator, gets netherrack/end_stone, not a fabricated empty
// chunk. This is what the flipped hasTerrainGenerator promises exists.
void testRealTerrainPerDimension() {
    constexpr std::uint64_t kSeed = 0xC0FFEEULL;
    const DimensionChunkGenerator overworld{DimensionId::Overworld, kSeed};
    const DimensionChunkGenerator nether{DimensionId::Nether, kSeed};
    const DimensionChunkGenerator end{DimensionId::End, kSeed};

    // Overworld: real stone/grass terrain (its generator unchanged).
    const Chunk over = overworld.generate(0, 0);
    assert(count(over, Block::Stone) > 0);

    // Nether: real netherrack, never fabricated air, never overworld stone.
    const Chunk neth = nether.generate(0, 0);
    assert(count(neth, Block::Netherrack) > 0);
    assert(count(neth, Block::Bedrock) > 0);  // its cap
    assert(count(neth, Block::Stone) == 0);   // not the overworld generator

    // End: a real end_stone central island, never fabricated air.
    const Chunk endChunk = end.generate(0, 0);
    assert(count(endChunk, Block::EndStone) > 0);
    assert(count(endChunk, Block::Stone) == 0);
    assert(count(endChunk, Block::Netherrack) == 0);
}

// 2. Each dimension generates to its DimensionType height. The Nether caps at its
// 128 logical roof (nothing above y=127, a bedrock roof there); the End spans its
// taller column; the Overworld is unchanged.
void testPerDimensionHeight() {
    constexpr std::uint64_t kSeed = 0xC0FFEEULL;
    const DimensionChunkGenerator nether{DimensionId::Nether, kSeed};
    const Chunk neth = nether.generate(0, 0);
    for (int x = 0; x < kChunkWidth; ++x) {
        for (int z = 0; z < kChunkDepth; ++z) {
            // The Nether roof is at y=127; nothing generates above it, and the top
            // row is bedrock (the ceiling cap).
            assert(neth.block(x, 127, z) == Block::Bedrock);
            for (int y = 128; y < kMaxY; ++y) {
                assert(neth.block(x, y, z) == Block::Air);
            }
            // No sub-zero fill either (the Nether floor is the lattice at y=0).
            for (int y = kMinY; y < 0; ++y) {
                assert(neth.block(x, y, z) == Block::Air);
            }
        }
    }
    // The config carries the DimensionType heights the generators honour.
    assert(mc::world::dimensionGeneratorConfig(DimensionId::Nether).hasCeiling);
    assert(mc::world::dimensionGeneratorConfig(DimensionId::Nether).height ==
           mc::world::dimensionType(DimensionId::Nether).height);
    assert(mc::world::dimensionGeneratorConfig(DimensionId::End).height ==
           mc::world::dimensionType(DimensionId::End).height);
}

// 3. The dispatch is a binding, not a second algorithm: the DimensionChunkGenerator
// reproduces the standalone WG-1/2/3 generators byte-for-byte.
void testDispatchMatchesStandalone() {
    constexpr std::uint64_t kSeed = 0x1234ULL;
    const DimensionChunkGenerator owDispatch{DimensionId::Overworld, kSeed};
    const DimensionChunkGenerator netherDispatch{DimensionId::Nether, kSeed};
    const DimensionChunkGenerator endDispatch{DimensionId::End, kSeed};

    const mc::world::SurfaceGenerator overworld{kSeed};
    const mc::world::NetherGenerator nether{kSeed};
    const mc::world::EndGenerator end{kSeed};

    assert(chunksEqual(owDispatch.generate(2, -3), overworld.generate(2, -3)));
    assert(chunksEqual(netherDispatch.generate(2, -3), nether.generate(2, -3)));
    assert(chunksEqual(endDispatch.generate(0, 0), end.generate(0, 0)));
}

// 4. Determinism: same world seed ⇒ identical dimension chunks.
void testDeterminism() {
    constexpr std::uint64_t kSeed = 42ULL;
    const DimensionChunkGenerator nether{DimensionId::Nether, kSeed};
    assert(chunksEqual(nether.generate(1, 1), nether.generate(1, 1)));
    const DimensionChunkGenerator end{DimensionId::End, kSeed};
    assert(chunksEqual(end.generate(0, 0), end.generate(0, 0)));
}

} // namespace

int main() {
    testRealTerrainPerDimension();
    testPerDimensionHeight();
    testDispatchMatchesStandalone();
    testDeterminism();
    return 0;
}

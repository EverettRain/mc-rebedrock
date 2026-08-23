// WG-3: end terrain generation.
//
// Verifies the end generator the acceptance calls for, headless:
//   1. terrain — a solid end_stone central island platform, a void ring around
//      it, discrete outer floating islands, no fluid anywhere (the end has no
//      sea — a non-solid cell is the void);
//   2. biomes — TheEnd at the centre, the outer biomes (highlands/midlands/
//      barrens/small islands) by distance to the origin, never an overworld one;
//   3. determinism — same seed ⇒ identical chunks, the end differs from the
//      overworld and the nether, and its biome map uses the derived end seed.

#include "world/EndGenerator.hpp"
#include "world/NetherGenerator.hpp"
#include "world/SurfaceGenerator.hpp"
#include "world/DimensionGenerator.hpp"
#include "world/WorldConstants.hpp"
#include "world/gen/Biome.hpp"

#include <cassert>
#include <cstdint>
#include <set>

namespace {

using mc::world::Block;
using mc::world::Chunk;
using mc::world::EndGenerator;
using mc::world::kChunkDepth;
using mc::world::kChunkWidth;
using mc::world::kMaxY;
using mc::world::kMinY;
using mc::world::gen::Biome;

[[nodiscard]] long countBlock(const Chunk& chunk, Block block) {
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

// A cell that is neither end_stone nor air: a fluid leak, which the void end must
// never have.
[[nodiscard]] long countNonVoidNonStone(const Chunk& chunk) {
    long n = 0;
    for (int x = 0; x < kChunkWidth; ++x) {
        for (int z = 0; z < kChunkDepth; ++z) {
            for (int y = kMinY; y < kMaxY; ++y) {
                const Block here = chunk.block(x, y, z);
                if (here != Block::Air && here != Block::EndStone) {
                    ++n;
                }
            }
        }
    }
    return n;
}

// 1. Terrain: central island solid, void ring, outer islands, no fluid.
void testTerrain() {
    const EndGenerator generator{0xC0FFEEULL};

    // The central island is a substantial solid end_stone platform, no fluid.
    const Chunk centre = generator.generate(0, 0);
    assert(countBlock(centre, Block::EndStone) > 1000);
    assert(countNonVoidNonStone(centre) == 0);          // no fluid, no leak
    assert(countBlock(centre, Block::Water) == 0);
    assert(countBlock(centre, Block::Stone) == 0);      // overworld did not leak
    assert(countBlock(centre, Block::Netherrack) == 0); // nether did not leak

    // The void ring: past the central island's falloff (which tapers a few chunks
    // beyond the 64-chunk disc) there is a deep void band before the outer islands
    // begin. The whole 68..80 band along the +x axis is empty.
    long voidRingChunks = 0;
    for (int cx = 68; cx < 80; ++cx) {
        const Chunk chunk = generator.generate(cx, 0);
        if (countBlock(chunk, Block::EndStone) == 0) {
            ++voidRingChunks;
        }
    }
    assert(voidRingChunks == 12);  // the whole sampled ring is void

    // Outer islands: over a wide far-out survey there is a mix of solid island
    // chunks and void chunks — discrete floating islands, not a continuous plate
    // and not all void.
    long outerIsland = 0;
    long outerVoid = 0;
    for (int cx = -140; cx < 140; cx += 3) {
        for (int cz = -140; cz < 140; cz += 11) {
            const long d2 = static_cast<long>(cx) * cx + static_cast<long>(cz) * cz;
            if (d2 <= 4096) {
                continue;  // skip the central disc
            }
            if (countBlock(generator.generate(cx, cz), Block::EndStone) > 0) {
                ++outerIsland;
            } else {
                ++outerVoid;
            }
        }
    }
    assert(outerIsland > 0);  // real outer islands exist
    assert(outerVoid > 0);    // separated by void (not a continuous plate)
    // No fluid anywhere in the far survey either.
    assert(countBlock(generator.generate(120, 37), Block::Water) == 0);
    assert(countBlock(generator.generate(120, 37), Block::Lava) == 0);
}

// 2. Biomes: TheEnd at the centre, the outer four by distance, never overworld.
void testBiomes() {
    const EndGenerator generator{0x1234ULL};

    // Dead centre is TheEnd.
    assert(generator.biomeAt(0, 0) == Biome::TheEnd);
    // A column well inside the 64-chunk disc is still TheEnd.
    assert(generator.biomeAt(16 * 30, 0) == Biome::TheEnd);

    // Survey the whole end from the centre out past the central island's edge and
    // into the far void: the biome bands come from the island-height field, so the
    // transition ring around the central island carries the highlands→midlands→
    // barrens gradient and the far void is the small-islands zone.
    std::set<Biome> seen;
    for (int cx = -120; cx < 120; ++cx) {
        for (int cz = -120; cz < 120; ++cz) {
            seen.insert(generator.biomeAt(cx * 16, cz * 16));
        }
    }
    // Every biome the source emits is an end biome (never an overworld/nether one).
    for (const Biome biome : seen) {
        assert(biome == Biome::TheEnd || biome == Biome::EndHighlands ||
               biome == Biome::EndMidlands || biome == Biome::EndBarrens ||
               biome == Biome::SmallEndIslands);
    }
    // The centre disc is TheEnd, and all four outer biomes appear across the map:
    // the distance/height source resolves the full nether-free end biome set.
    assert(seen.count(Biome::TheEnd) == 1);
    assert(seen.count(Biome::EndHighlands) != 0);
    assert(seen.count(Biome::EndMidlands) != 0);
    assert(seen.count(Biome::EndBarrens) != 0);
    assert(seen.count(Biome::SmallEndIslands) != 0);
}

// 3. Determinism and the derived seed.
void testDeterminismAndDerivedSeed() {
    const EndGenerator generator{42ULL};
    const Chunk a = generator.generate(0, 0);
    const Chunk b = generator.generate(0, 0);
    for (int x = 0; x < kChunkWidth; ++x) {
        for (int z = 0; z < kChunkDepth; ++z) {
            for (int y = kMinY; y < kMaxY; ++y) {
                assert(a.block(x, y, z) == b.block(x, y, z));
            }
        }
    }

    // The end seed is derived, not the world seed, and differs from the nether's.
    assert(mc::world::dimensionSeed(42ULL, mc::world::DimensionId::End) != 42ULL);
    assert(mc::world::dimensionSeed(42ULL, mc::world::DimensionId::End) !=
           mc::world::dimensionSeed(42ULL, mc::world::DimensionId::Nether));

    // The end differs from the overworld and the nether at the same chunk. The
    // central end island is solid end_stone; the overworld/nether are not.
    const mc::world::SurfaceGenerator overworld{42ULL};
    const mc::world::NetherGenerator nether{42ULL};
    const Chunk end = generator.generate(0, 0);
    const Chunk over = overworld.generate(0, 0);
    const Chunk neth = nether.generate(0, 0);
    bool differsFromOver = false;
    bool differsFromNether = false;
    for (int x = 0; x < kChunkWidth && !(differsFromOver && differsFromNether); ++x) {
        for (int z = 0; z < kChunkDepth; ++z) {
            for (int y = kMinY; y < kMaxY; ++y) {
                if (end.block(x, y, z) != over.block(x, y, z)) {
                    differsFromOver = true;
                }
                if (end.block(x, y, z) != neth.block(x, y, z)) {
                    differsFromNether = true;
                }
            }
        }
    }
    assert(differsFromOver);
    assert(differsFromNether);

    // The generator's biome map uses the *derived* end seed: rebuilding an end
    // source directly from the derived seed matches, from the raw world seed
    // diverges. (buildEndIslandNoise salts the seed, so a raw vs derived seed
    // produces a different island field somewhere.)
    const auto derived = mc::world::dimensionSeed(42ULL, mc::world::DimensionId::End);
    const mc::world::gen::BiomeSource derivedMap = mc::world::gen::BiomeSource::end(derived);
    const mc::world::gen::BiomeSource rawSeedMap = mc::world::gen::BiomeSource::end(42ULL);
    // Sample well outside the central disc (which is TheEnd for every seed) so the
    // island-noise-driven outer biomes can reveal whether the two seeds differ.
    // Quart 400 == 1600 blocks == 100 chunks, past the 64-chunk disc.
    bool matchesDerived = true;
    bool divergesFromRaw = false;
    for (int qx = -400; qx < 400; qx += 7) {
        for (int qz = -400; qz < 400; qz += 7) {
            if (generator.biomeAt(qx * 4, qz * 4) != derivedMap.biomeForNoiseGeneration(qx, qz)) {
                matchesDerived = false;
            }
            if (rawSeedMap.biomeForNoiseGeneration(qx, qz) !=
                derivedMap.biomeForNoiseGeneration(qx, qz)) {
                divergesFromRaw = true;
            }
        }
    }
    assert(matchesDerived);
    assert(divergesFromRaw);
}

} // namespace

int main() {
    testTerrain();
    testBiomes();
    testDeterminismAndDerivedSeed();
    return 0;
}

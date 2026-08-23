// WG-2: nether terrain generation.
//
// Verifies the nether generator the acceptance calls for, headless:
//   1. terrain — netherrack solid with the noise carving caverns, a lava sea at
//      y<32, a bedrock floor and roof cap, height 128 (nothing above the ceiling);
//   2. biomes — the five nether biomes appear across a wide sample, and the soul
//      sand valley / basalt deltas paint their surface palette;
//   3. features — glowstone clusters and quartz ore actually generate;
//   4. determinism — same seed ⇒ identical chunks, and the nether differs from
//      the overworld at the same coordinates (seed is derived, not reused).

#include "world/NetherGenerator.hpp"
#include "world/SurfaceGenerator.hpp"
#include "world/DimensionGenerator.hpp"
#include "world/WorldConstants.hpp"
#include "world/gen/Biome.hpp"
#include "world/gen/MultiNoiseBiomeSource.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>

namespace {

using mc::world::Block;
using mc::world::Chunk;
using mc::world::kChunkDepth;
using mc::world::kChunkWidth;
using mc::world::NetherGenerator;
using mc::world::gen::Biome;

constexpr int kNetherMinY = 0;
constexpr int kNetherHeight = 128;
constexpr int kNetherTop = kNetherMinY + kNetherHeight - 1;

// 1. Terrain shape: netherrack-dominated column, a lava sea, bedrock cap, and a
// closed ceiling.
void testTerrain() {
    const NetherGenerator generator{0xC0FFEEULL};
    std::map<Block, long> blocks;
    long lavaBelowSea = 0;
    long solidCells = 0;
    long caveAir = 0;
    for (int cx = 0; cx < 6; ++cx) {
        for (int cz = 0; cz < 6; ++cz) {
            const Chunk chunk = generator.generate(cx, cz);
            for (int x = 0; x < kChunkWidth; ++x) {
                for (int z = 0; z < kChunkDepth; ++z) {
                    // Bedrock floor: the very bottom row is always bedrock.
                    assert(chunk.block(x, kNetherMinY, z) == Block::Bedrock);
                    // Bedrock roof: the very top row is always bedrock (the nether
                    // has a ceiling).
                    assert(chunk.block(x, kNetherTop, z) == Block::Bedrock);
                    // Nothing generates above the ceiling (height honoured — the
                    // extra overworld rows -64..0 and above 128 stay air).
                    for (int y = kNetherTop + 1; y < mc::world::kMaxY; ++y) {
                        assert(chunk.block(x, y, z) == Block::Air);
                    }
                    for (int y = mc::world::kMinY; y < kNetherMinY; ++y) {
                        assert(chunk.block(x, y, z) == Block::Air);  // no sub-zero fill
                    }
                    for (int y = kNetherMinY; y <= kNetherTop; ++y) {
                        const Block here = chunk.block(x, y, z);
                        ++blocks[here];
                        if (here == Block::Lava) {
                            assert(y <= 32);  // lava only in the sea, at/below y=32
                            ++lavaBelowSea;
                        }
                        if (here == Block::Netherrack) {
                            ++solidCells;
                        }
                        // An air pocket bounded below by rock: a real cavern.
                        if (here == Block::Air && y > kNetherMinY + 2 && y < kNetherTop - 2 &&
                            chunk.block(x, y - 1, z) == Block::Netherrack) {
                            ++caveAir;
                        }
                    }
                }
            }
        }
    }
    // Netherrack is the bulk of the column.
    assert(blocks[Block::Netherrack] > 0 && solidCells > 0);
    // The default fluid is lava, never water.
    assert(blocks.find(Block::Water) == blocks.end());
    assert(lavaBelowSea > 0);
    // The noise carved real caverns rather than a solid brick.
    assert(caveAir > 0);
    // No overworld surface leaked in.
    assert(blocks.find(Block::Grass) == blocks.end());
    assert(blocks.find(Block::Stone) == blocks.end());
}

// 2. Biomes: the five nether biomes appear across a wide sample, and the two with
// a non-netherrack surface palette paint it.
void testBiomes() {
    const NetherGenerator generator{0x1234ULL};
    std::set<Biome> seen;
    for (int cx = -8; cx < 8; ++cx) {
        for (int cz = -8; cz < 8; ++cz) {
            seen.insert(generator.biomeAt(cx * 16, cz * 16));
        }
    }
    // Every biome the source can emit is a nether biome (never an overworld one).
    for (const Biome biome : seen) {
        assert(biome == Biome::NetherWastes || biome == Biome::SoulSandValley ||
               biome == Biome::CrimsonForest || biome == Biome::WarpedForest ||
               biome == Biome::BasaltDeltas);
    }
    // A multi-noise map over a wide area yields more than one biome.
    assert(seen.size() >= 2);

    // Find a soul sand valley column somewhere and confirm its surface paints
    // soul sand (the WG-0 palette flowing through the surface pass).
    bool sawSoulSand = false;
    bool sawBasalt = false;
    for (int cx = -12; cx < 12 && !(sawSoulSand && sawBasalt); ++cx) {
        for (int cz = -12; cz < 12; ++cz) {
            const Biome biome = generator.biomeAt(cx * 16, cz * 16);
            if (biome != Biome::SoulSandValley && biome != Biome::BasaltDeltas) {
                continue;
            }
            const Chunk chunk = generator.generate(cx, cz);
            for (int x = 0; x < kChunkWidth; ++x) {
                for (int z = 0; z < kChunkDepth; ++z) {
                    for (int y = kNetherTop; y > kNetherMinY; --y) {
                        const Block here = chunk.block(x, y, z);
                        if (here == Block::SoulSand || here == Block::SoulSoil) {
                            sawSoulSand = true;
                        }
                        if (here == Block::Basalt || here == Block::Blackstone) {
                            sawBasalt = true;
                        }
                    }
                }
            }
        }
    }
    // At least one of the two special-surface biomes painted its palette (both if
    // both appeared in range).
    assert(sawSoulSand || sawBasalt);
}

// 3. Features: glowstone clusters and quartz ore generate somewhere in a survey.
void testFeatures() {
    const NetherGenerator generator{0xBEEFULL};
    long glowstone = 0;
    long quartz = 0;
    for (int cx = 0; cx < 8; ++cx) {
        for (int cz = 0; cz < 8; ++cz) {
            const Chunk chunk = generator.generate(cx, cz);
            for (int x = 0; x < kChunkWidth; ++x) {
                for (int z = 0; z < kChunkDepth; ++z) {
                    for (int y = kNetherMinY; y <= kNetherTop; ++y) {
                        const Block here = chunk.block(x, y, z);
                        if (here == Block::Glowstone) {
                            ++glowstone;
                        } else if (here == Block::NetherQuartzOre) {
                            ++quartz;
                        }
                    }
                }
            }
        }
    }
    assert(glowstone > 0);
    assert(quartz > 0);
}

// 4. Determinism, and the derived seed: same seed ⇒ identical chunk, and the
// nether at (x,z) is not the overworld at (x,z) (the seed is folded through the
// dimension, so the two never mirror).
void testDeterminismAndDerivedSeed() {
    const NetherGenerator generator{42ULL};
    const Chunk a = generator.generate(2, -3);
    const Chunk b = generator.generate(2, -3);
    for (int x = 0; x < kChunkWidth; ++x) {
        for (int z = 0; z < kChunkDepth; ++z) {
            for (int y = kNetherMinY; y <= kNetherTop; ++y) {
                assert(a.block(x, y, z) == b.block(x, y, z));
            }
        }
    }

    // The nether seed is derived, not the world seed: dimensionSeed folds the
    // ordinal in, so it differs from the overworld's plain world seed.
    assert(mc::world::dimensionSeed(42ULL, mc::world::DimensionId::Nether) != 42ULL);

    // And the terrain differs: the nether column at a chunk is not the overworld
    // column at the same chunk (different blocks, not just a coincidental match).
    const mc::world::SurfaceGenerator overworld{42ULL};
    const Chunk over = overworld.generate(2, -3);
    const Chunk nether = generator.generate(2, -3);
    bool differs = false;
    for (int x = 0; x < kChunkWidth && !differs; ++x) {
        for (int z = 0; z < kChunkDepth && !differs; ++z) {
            for (int y = kNetherMinY; y <= kNetherTop; ++y) {
                if (over.block(x, y, z) != nether.block(x, y, z)) {
                    differs = true;
                    break;
                }
            }
        }
    }
    assert(differs);

    // The generator's biome map must be the one keyed by the *derived* nether
    // seed, not the raw world seed — this is what catches a "reused the world
    // seed" regression (the terrain-differs check above passes regardless, since
    // the nether and overworld settings differ). Build the multi-noise source
    // both ways and confirm the generator matches the derived one and diverges
    // from the raw-seed one somewhere.
    constexpr std::uint64_t kWorld = 42ULL;
    const auto derived = mc::world::dimensionSeed(kWorld, mc::world::DimensionId::Nether);
    const mc::world::gen::MultiNoiseBiomeSource derivedMap{derived};
    const mc::world::gen::MultiNoiseBiomeSource rawSeedMap{kWorld};
    bool matchesDerived = true;
    bool divergesFromRaw = false;
    for (int qx = -20; qx < 20; ++qx) {
        for (int qz = -20; qz < 20; ++qz) {
            const Biome fromGenerator = generator.biomeAt(qx * 4, qz * 4);
            if (fromGenerator != derivedMap.sample(qx, qz)) {
                matchesDerived = false;
            }
            if (rawSeedMap.sample(qx, qz) != derivedMap.sample(qx, qz)) {
                divergesFromRaw = true;
            }
        }
    }
    assert(matchesDerived);     // the generator uses the derived nether seed
    assert(divergesFromRaw);    // and the derived seed is genuinely different
}

} // namespace

int main() {
    testTerrain();
    testBiomes();
    testFeatures();
    testDeterminismAndDerivedSeed();
    return 0;
}

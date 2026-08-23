// WG-1: NoiseChunkGenerator generalisation — one algorithm, per-dimension
// NoiseGeneratorSettings values.
//
// The acceptance has three parts:
//   1. Overworld bit-equivalence (the core regression): the generalised
//      generator, run with NoiseGeneratorSettings::overworld(), produces the
//      exact same terrain the pre-WG-1 hardcoded generator did. Proven against a
//      golden hash captured from the pre-WG-1 build (see the harness in the
//      commit message / landing note); a single flipped cell changes the hash.
//   2. The settings actually flow: swapping the default block, the fluid, the
//      sea level and the floor fill changes the output the way the value says.
//   3. Determinism: same seed + same settings ⇒ identical chunks.

#include "world/SurfaceGenerator.hpp"
#include "world/gen/BiomeSource.hpp"
#include "world/gen/JavaRandom.hpp"
#include "world/gen/NoiseChunkGenerator.hpp"
#include "world/gen/NoiseGeneratorSettings.hpp"
#include "world/gen/NoiseSampler.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using mc::world::Block;
using mc::world::Chunk;
using mc::world::kChunkDepth;
using mc::world::kChunkWidth;
using mc::world::kMaxY;
using mc::world::kMinY;
namespace gen = mc::world::gen;

// The same vanilla-ordered sampler draw SurfaceGenerator uses, so a standalone
// NoiseChunkGenerator here rides the exact stream the runtime one does.
struct Samplers {
    std::vector<gen::PerlinNoiseSampler> lower;
    std::vector<gen::PerlinNoiseSampler> upper;
    std::vector<gen::PerlinNoiseSampler> interpolation;
    gen::OctavePerlinNoiseSampler densityOffset;
};

[[nodiscard]] Samplers drawSamplers(std::uint64_t seed) {
    gen::JavaRandom random{seed};
    Samplers result;
    result.lower = gen::buildOctaves(random, gen::NoiseChunkGenerator::kOctaveCount);
    result.upper = gen::buildOctaves(random, gen::NoiseChunkGenerator::kOctaveCount);
    result.interpolation =
        gen::buildOctaves(random, gen::NoiseChunkGenerator::kInterpolationOctaveCount);
    (void)gen::OctaveSimplexNoiseSampler{random, 4};
    random.consume(2620);
    result.densityOffset =
        gen::OctavePerlinNoiseSampler{random, gen::NoiseChunkGenerator::kOctaveCount};
    return result;
}

// A stable FNV-1a over the base terrain a settings value produces for one chunk.
// Hashes the raw base-terrain column (noise + fluid + floor fill), before the
// carve/surface/feature passes, so it isolates exactly what WG-1 touched.
[[nodiscard]] std::uint64_t hashBaseTerrain(const gen::NoiseGeneratorSettings& settings,
                                            std::uint64_t seed, int chunkX, int chunkZ) {
    gen::BiomeSource biomeSource{seed};
    const Samplers samplers = drawSamplers(seed);
    // The generator copies the sampler vectors it is handed; give it copies so
    // the caller can reuse them.
    gen::NoiseChunkGenerator generator{biomeSource, settings, samplers.lower, samplers.upper,
                                       samplers.interpolation, samplers.densityOffset};
    Chunk chunk;
    generator.buildBaseTerrain(chunk, chunkX, chunkZ);

    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    for (int x = 0; x < kChunkWidth; ++x) {
        for (int z = 0; z < kChunkDepth; ++z) {
            for (int y = kMinY; y < kMaxY; ++y) {
                mix(static_cast<std::uint64_t>(chunk.block(x, y, z)));
            }
        }
    }
    return hash;
}

// 1. Overworld bit-equivalence. The golden hashes were captured from the
// pre-WG-1 generator (the hardcoded overworld path) for these chunks; the
// generalised generator with overworld() settings must reproduce them exactly.
// A regression that drops or reorders a settings constant changes a cell and
// trips one of these.
void testOverworldEquivalence() {
    constexpr std::uint64_t kSeed = 0xC0FFEEULL;
    const auto overworld = gen::NoiseGeneratorSettings::overworld();
    struct Golden {
        int chunkX;
        int chunkZ;
        std::uint64_t hash;
    };
    // GOLDEN: captured from the pre-WG-1 build (the hardcoded-overworld
    // NoiseChunkGenerator at commit 91cd562), via an FNV-1a over the base-terrain
    // column for seed 0xC0FFEE. Regenerate with the golden_dump harness (landing
    // note) if the overworld value set ever legitimately changes.
    constexpr Golden kGolden[] = {
        {0, 0, 0x5f1bbd024fd893a1ULL},
        {3, -5, 0x7482e74642f43a1bULL},
        {12, 7, 0xb0dd73048accec68ULL},
    };
    for (const Golden& g : kGolden) {
        const std::uint64_t got = hashBaseTerrain(overworld, kSeed, g.chunkX, g.chunkZ);
        assert(got == g.hash);
    }
}

// The overworld() value set carries the exact literals NoiseChunkGenerator used
// to hardcode. Pinning them here catches a typo in the extraction directly (not
// only through the terrain hash), which is the "settings lost a constant"
// sabotage's second net.
void testOverworldConstants() {
    const auto s = gen::NoiseGeneratorSettings::overworld();
    assert(s.seaLevel == mc::world::kSeaLevel);
    assert(s.minY == mc::world::kMinY);
    assert(s.height == mc::world::kWorldHeight);
    assert(s.defaultBlock == Block::Stone);
    assert(s.defaultFluid == Block::Water);
    assert(s.densityFactor == 1.0);
    assert(s.densityOffset == -0.46875);
    assert(s.topSlide.target == -10.0 && s.topSlide.size == 3 && s.topSlide.offset == 0);
    assert(s.bottomSlide.target == -30.0 && s.bottomSlide.size == 0);
    assert(s.fillBelowLatticeFloor);
    // The sampling scales are the fixed 684.412 base times the overworld config.
    assert(s.sampling.xzScale == 684.412 * 0.9999999814507745);
    assert(s.sampling.xzFactor == (684.412 * 0.9999999814507745) / 80.0);
}

// 2. The settings flow. A generator built with a nether-shaped value set (default
// block netherrack over a lava sea, floor at the lattice, height 128) must put
// those blocks in the column — never the overworld's stone/water. This is what
// the "default block hardcoded" and "height hardcoded" sabotages break.
void testSettingsFlow() {
    constexpr std::uint64_t kSeed = 0xBEEFULL;
    gen::BiomeSource biomeSource{kSeed};
    const Samplers samplers = drawSamplers(kSeed);

    // A stand-in nether value set (real values are WG-2's). Netherrack solid,
    // lava sea at y=31, floor is the lattice (no sub-zero fill), 0..128.
    gen::NoiseGeneratorSettings nether;
    nether.minY = 0;
    nether.height = 128;
    nether.seaLevel = 31;
    nether.defaultBlock = Block::Netherrack;
    nether.defaultFluid = Block::Lava;
    nether.fillBelowLatticeFloor = false;

    gen::NoiseChunkGenerator generator{biomeSource, nether, samplers.lower, samplers.upper,
                                       samplers.interpolation, samplers.densityOffset};
    Chunk chunk;
    generator.buildBaseTerrain(chunk, 0, 0);

    // The solid rows are the default block, and never the overworld's stone. The
    // fluid, where present below the sea level, is lava, never water.
    long solid = 0;
    long fluid = 0;
    for (int x = 0; x < kChunkWidth; ++x) {
        for (int z = 0; z < kChunkDepth; ++z) {
            for (int y = kMinY; y < kMaxY; ++y) {
                const Block here = chunk.block(x, y, z);
                assert(here != Block::Stone);   // default block is not hardcoded stone
                assert(here != Block::Water);    // fluid is not hardcoded water
                if (here == Block::Netherrack) {
                    ++solid;
                } else if (here == Block::Lava) {
                    ++fluid;
                    assert(y <= nether.seaLevel);
                }
                // fillBelowLatticeFloor == false: the sub-zero rows stay air.
                if (y < 0) {
                    assert(here == Block::Air);
                }
            }
        }
    }
    assert(solid > 0);  // the netherrack terrain actually generated

    // Height honoured: nothing is placed at or above the settings' build limit
    // (minY + height == 128), and the deep overworld floor is not filled.
    for (int x = 0; x < kChunkWidth; ++x) {
        for (int z = 0; z < kChunkDepth; ++z) {
            for (int y = nether.minY + nether.height; y < kMaxY; ++y) {
                assert(chunk.block(x, y, z) == Block::Air);
            }
        }
    }
}

// A default-fluid of Air (the end): below sea level a non-solid cell stays air,
// never a fluid block.
void testVoidFluid() {
    constexpr std::uint64_t kSeed = 0x5A5AULL;
    gen::BiomeSource biomeSource{kSeed};
    const Samplers samplers = drawSamplers(kSeed);
    gen::NoiseGeneratorSettings end;
    end.minY = 0;
    end.height = 256;
    end.seaLevel = 63;
    end.defaultBlock = Block::EndStone;
    end.defaultFluid = Block::Air;  // the end has no sea
    end.fillBelowLatticeFloor = false;
    gen::NoiseChunkGenerator generator{biomeSource, end, samplers.lower, samplers.upper,
                                       samplers.interpolation, samplers.densityOffset};
    Chunk chunk;
    generator.buildBaseTerrain(chunk, 0, 0);
    for (int x = 0; x < kChunkWidth; ++x) {
        for (int z = 0; z < kChunkDepth; ++z) {
            for (int y = kMinY; y < kMaxY; ++y) {
                const Block here = chunk.block(x, y, z);
                assert(here == Block::Air || here == Block::EndStone);
            }
        }
    }
}

// 3. Determinism: same seed + same settings ⇒ identical base terrain.
void testDeterminism() {
    const auto overworld = gen::NoiseGeneratorSettings::overworld();
    const std::uint64_t a = hashBaseTerrain(overworld, 777ULL, 2, 2);
    const std::uint64_t b = hashBaseTerrain(overworld, 777ULL, 2, 2);
    assert(a == b);
    // A different settings value (different default block) is a different world.
    gen::NoiseGeneratorSettings alt = overworld;
    alt.defaultBlock = Block::Netherrack;
    assert(hashBaseTerrain(alt, 777ULL, 2, 2) != a);
}

} // namespace

int main() {
    testOverworldEquivalence();
    testOverworldConstants();
    testSettingsFlow();
    testVoidFluid();
    testDeterminism();
    return 0;
}

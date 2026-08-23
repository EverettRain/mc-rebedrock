#pragma once

#include "world/gen/JavaRandom.hpp"
#include "world/gen/NoiseChunkGenerator.hpp"
#include "world/gen/NoiseSampler.hpp"

#include <cstdint>
#include <vector>

namespace mc::world::gen {

// The noise samplers a chunk generator's constructor draws from its single
// ChunkRandom stream, in vanilla's order: the three terrain stacks, then the
// simplex surface-depth sampler, then (after a 2620-step skip) the 16-octave
// density-offset stack. Sharing one stream is what makes a given seed lay the
// terrain, the surface and the cave floors coherently. Both the overworld
// (SurfaceGenerator) and the nether (NetherGenerator) draw the same bundle, so it
// lives here rather than inside either generator.
struct GenerationSamplers {
    std::vector<PerlinNoiseSampler> lower;
    std::vector<PerlinNoiseSampler> upper;
    std::vector<PerlinNoiseSampler> interpolation;
    OctaveSimplexNoiseSampler surfaceDepth;
    OctavePerlinNoiseSampler densityOffset;
};

// SurfaceChunkGenerator's constructor draws every sampler from one stream in this
// order, with a 2620-step skip before the density offset.
[[nodiscard]] inline GenerationSamplers buildGenerationSamplers(std::uint64_t seed) {
    JavaRandom random{seed};
    GenerationSamplers result;
    result.lower = buildOctaves(random, NoiseChunkGenerator::kOctaveCount);
    result.upper = buildOctaves(random, NoiseChunkGenerator::kOctaveCount);
    result.interpolation = buildOctaves(random, NoiseChunkGenerator::kInterpolationOctaveCount);
    result.surfaceDepth = OctaveSimplexNoiseSampler{random, 4};
    random.consume(2620);
    result.densityOffset = OctavePerlinNoiseSampler{random, NoiseChunkGenerator::kOctaveCount};
    return result;
}

} // namespace mc::world::gen

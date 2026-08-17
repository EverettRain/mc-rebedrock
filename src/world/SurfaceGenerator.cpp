#include "world/SurfaceGenerator.hpp"

namespace mc::world {
namespace {

// SurfaceChunkGenerator's constructor draws every sampler from one stream, in
// this order, with a 2620-step skip before the density offset. The lower and
// upper stacks carry the terrain detail, the interpolation stack blends them,
// the simplex surface-depth noise roughens the surface layer, and the density
// offset tilts whole regions up or down by a fraction of a block.
[[nodiscard]] GenerationSamplers buildGenerationSamplers(std::uint64_t seed) {
    gen::JavaRandom random{seed};
    GenerationSamplers result;
    result.lower = gen::buildOctaves(random, gen::NoiseChunkGenerator::kOctaveCount);
    result.upper = gen::buildOctaves(random, gen::NoiseChunkGenerator::kOctaveCount);
    result.interpolation =
        gen::buildOctaves(random, gen::NoiseChunkGenerator::kInterpolationOctaveCount);
    result.surfaceDepth = gen::OctaveSimplexNoiseSampler{random, 4};
    random.consume(2620);
    result.densityOffset =
        gen::OctavePerlinNoiseSampler{random, gen::NoiseChunkGenerator::kOctaveCount};
    return result;
}

} // namespace

SurfaceGenerator::SurfaceGenerator(std::uint64_t seed)
    : biomeSource_(seed),
      samplers_(buildGenerationSamplers(seed)),
      noiseGenerator_(biomeSource_, samplers_.lower, samplers_.upper, samplers_.interpolation,
                      samplers_.densityOffset),
      carver_(seed),
      features_(seed, biomeSource_, samplers_.surfaceDepth) {}

Chunk SurfaceGenerator::generate(
    int chunkX,
    int chunkZ,
    std::vector<gen::TreeBorderBlock>& borderBlocks) const {
    Chunk chunk;
    // ChunkStatus's order: NOISE, then CARVERS, then SURFACE, then FEATURES.
    // Carving before the surface pass is what leaves a cave mouth lined with
    // grass and dirt instead of raw stone.
    noiseGenerator_.buildBaseTerrain(chunk, chunkX, chunkZ);
    carver_.carveChunk(chunk, chunkX, chunkZ);
    features_.buildSurface(chunk, chunkX, chunkZ);
    features_.generateOres(chunk, chunkX, chunkZ);
    features_.generateVegetation(chunk, chunkX, chunkZ, borderBlocks);
    return chunk;
}

} // namespace mc::world

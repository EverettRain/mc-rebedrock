#include "world/SurfaceGenerator.hpp"

namespace mc::world {

SurfaceGenerator::SurfaceGenerator(std::uint64_t seed)
    : biomeSource_(seed),
      samplers_(gen::buildGenerationSamplers(seed)),
      noiseGenerator_(biomeSource_, gen::NoiseGeneratorSettings::overworld(), samplers_.lower,
                      samplers_.upper, samplers_.interpolation, samplers_.densityOffset),
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

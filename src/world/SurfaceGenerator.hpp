#pragma once

#include "world/Chunk.hpp"
#include "world/gen/Biome.hpp"
#include "world/gen/BiomeSource.hpp"
#include "world/gen/Carver.hpp"
#include "world/gen/Features.hpp"
#include "world/gen/GenerationSamplers.hpp"
#include "world/gen/NoiseChunkGenerator.hpp"
#include "world/gen/NoiseSampler.hpp"

#include <cstdint>
#include <vector>

namespace mc::world {

// The world generator, running Java 1.16.1's overworld pipeline in its order:
// the noise generator lays a stone/water column down, the carvers cut caves and
// ravines out of it, the surface builder paints the top few blocks per biome,
// and the feature passes add ores and vegetation.
//
// The algorithms and their constants are ported from 1.16.1; the biome map that
// decides which biome a column belongs to is a climate-noise stand-in for
// vanilla's GenLayer stack (see gen::BiomeSource), so a given seed does not
// reproduce the same world Java would — everything downstream of the biome map
// does behave the way vanilla's does.
class SurfaceGenerator final {
  public:
    explicit SurfaceGenerator(std::uint64_t seed);

    // Generates the chunk, appending any tree crown blocks that crossed the
    // chunk border to `borderBlocks` for the streamer to finish in neighbours.
    [[nodiscard]] Chunk generate(
        int chunkX,
        int chunkZ,
        std::vector<gen::TreeBorderBlock>& borderBlocks) const;

    // Convenience overload that discards the border crown (the standalone tests
    // and the runtime sapling path have no neighbour to finish it in).
    [[nodiscard]] Chunk generate(int chunkX, int chunkZ) const {
        std::vector<gen::TreeBorderBlock> ignored;
        return generate(chunkX, chunkZ, ignored);
    }

    [[nodiscard]] gen::Biome biomeAt(int worldX, int worldZ) const {
        return biomeSource_.biomeAtBlock(worldX, worldZ);
    }

    // The noise-only terrain surface height at a column (STRUCT structure Y).
    [[nodiscard]] int terrainHeightAt(int worldX, int worldZ) const {
        return noiseGenerator_.terrainHeightAt(worldX, worldZ);
    }

  private:
    gen::BiomeSource biomeSource_;
    // The samplers are drawn once in the constructor and handed down, so the
    // three terrain stacks, the surface depth and the density offset all share
    // one vanilla-ordered stream.
    gen::GenerationSamplers samplers_;
    gen::NoiseChunkGenerator noiseGenerator_;
    gen::Carver carver_;
    gen::Features features_;
};

} // namespace mc::world

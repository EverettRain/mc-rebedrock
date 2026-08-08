#pragma once

#include "world/Chunk.hpp"
#include "world/gen/Biome.hpp"
#include "world/gen/BiomeSource.hpp"
#include "world/gen/JavaRandom.hpp"
#include "world/gen/NoiseSampler.hpp"
#include "world/gen/TreeGrower.hpp"

#include <cstdint>

namespace mc::world::gen {

// Java 1.16.1's SurfaceBuilder.DEFAULT, plus the ore and vegetation features the
// overworld's GenerationStep runs after it.
class Features final {
  public:
    // `surfaceDepth` is the sampler SurfaceChunkGenerator draws from its shared
    // stream right after the interpolation stack, so a given seed lays the
    // surface the way Java would.
    Features(std::uint64_t seed, const BiomeSource& biomeSource,
             OctaveSimplexNoiseSampler surfaceDepth);

    // ChunkGenerator#buildSurface: replaces the top few stone blocks of every
    // column with the biome's surface materials, and lays the bedrock floor.
    void buildSurface(Chunk& chunk, int chunkX, int chunkZ) const;

    // GenerationStep.Feature.UNDERGROUND_ORES: the vanilla ore set at the
    // vanilla counts and heights. Every chunk in reach gets its features
    // replayed into this one, so a vein rolled next door still crosses the
    // border instead of stopping flat against it.
    void generateOres(Chunk& chunk, int chunkX, int chunkZ) const;

    // GenerationStep.Feature.VEGETAL_DECORATION: trees first, then grass and
    // flowers on whatever ground is left. Crown blocks that cross the chunk
    // border are appended to `borderBlocks` so the streamer can finish them in
    // the neighbouring chunk instead of clipping them.
    void generateVegetation(
        Chunk& chunk,
        int chunkX,
        int chunkZ,
        std::vector<TreeBorderBlock>& borderBlocks) const;

  private:
    // The ore features one chunk rolls, clipped to whichever chunk is being
    // written. Seeding from the origin chunk keeps the result independent of
    // generation order, exactly the way Carver::carveChunk replays the carvers
    // around it.
    void generateOresFrom(
        Chunk& chunk,
        int chunkX,
        int chunkZ,
        int originX,
        int originZ) const;

    // OreFeature#generate: one elongated blob of `size` cells along a random
    // axis, replacing stone only. The centre is in world coordinates; the blob
    // writes whichever of its cells land inside the chunk being generated and
    // silently skips the rest, so the same vein comes out identical no matter
    // which neighbour asked for it.
    static void placeOreBlob(
        Chunk& chunk,
        int chunkX,
        int chunkZ,
        JavaRandom& random,
        Block ore,
        int size,
        double centreX,
        int centreY,
        double centreZ);
    // The first non-air, non-water block in a column, or -1 for an empty one.
    [[nodiscard]] static int surfaceHeight(const Chunk& chunk, int localX, int localZ);
    // Thin wrapper over TreeGrower::growTree for the generation path: the tree
    // shapes live in the shared TreeGrower so a sapling can grow the same trees
    // at runtime. World coordinates are derived from the chunk origin; crown
    // blocks past the border land in `borderBlocks` instead of being dropped.
    [[nodiscard]] static bool placeTree(
        Chunk& chunk,
        int chunkX,
        int chunkZ,
        JavaRandom& random,
        const TreeChoice& choice,
        int localX,
        int groundY,
        int localZ,
        std::vector<TreeBorderBlock>& borderBlocks);

    std::uint64_t seed_ = 0U;
    const BiomeSource* biomeSource_ = nullptr;
    // SurfaceBuilder's `surfaceDepthNoise`, which decides how thick the dirt
    // layer is and roughens the sand/grass boundary.
    OctaveSimplexNoiseSampler surfaceDepthNoise_;
};

} // namespace mc::world::gen

#include "world/EndGenerator.hpp"

#include "world/DimensionGenerator.hpp"
#include "world/WorldConstants.hpp"

#include <cmath>

namespace mc::world {
namespace {

// The end's islands centre on this row: the main island's top surface, and where
// the outer islands hang. Vanilla's end terrain sits around here (the obsidian
// spawn platform is at y=49; the island tops run a bit above 60).
constexpr int kEndSurfaceY = 64;

// How fast an island thins away from its centre row: a column of island height h
// is solid roughly over [surfaceY - h*factor, surfaceY + h*factor], so a tall
// central-island column is a thick block and a low outer-island column is a thin
// shelf. Tuned so the central plateau (h up to 80) is a substantial island and
// the void (h <= 0) is empty.
constexpr double kThicknessPerHeight = 0.22;

// The 3D shell noise stretches/roughens each island's boundary so it is not a flat
// disc. Small amplitude relative to the thickness.
constexpr double kShellNoiseScale = 0.03;
constexpr double kShellNoiseAmplitude = 8.0;

} // namespace

EndGenerator::EndGenerator(std::uint64_t worldSeed)
    : endSeed_(dimensionSeed(worldSeed, DimensionId::End)),
      settings_(gen::NoiseGeneratorSettings::end()),
      biomeSource_(gen::BiomeSource::end(endSeed_)),
      samplers_(gen::buildGenerationSamplers(endSeed_)),
      islandNoise_(gen::buildEndIslandNoise(endSeed_)),
      shapeNoise_([this] {
          gen::JavaRandom random{endSeed_ ^ 0xEED5EED5EED5EED5ULL};
          return gen::OctavePerlinNoiseSampler{random, 4};
      }()) {}

float EndGenerator::islandHeightAt(int blockX, int blockZ) const {
    const int chunkX = blockX >> 4;
    const int chunkZ = blockZ >> 4;
    return gen::endIslandHeight(islandNoise_, chunkX, chunkZ, 1, 1);
}

Chunk EndGenerator::generate(int chunkX, int chunkZ) const {
    Chunk chunk;
    const int top = settings_.minY + settings_.height - 1;
    const int bottom = settings_.minY;

    for (int localX = 0; localX < kChunkWidth; ++localX) {
        for (int localZ = 0; localZ < kChunkDepth; ++localZ) {
            const int worldX = chunkX * kChunkWidth + localX;
            const int worldZ = chunkZ * kChunkDepth + localZ;
            chunk.setColumnBiome(localX, localZ, biomeSource_.biomeAtBlock(worldX, worldZ));

            // The island height for this column. Sampled per column at chunk
            // granularity (the field is chunk-scale) so a whole chunk shares one
            // island profile, matching the biome cell.
            const float height = gen::endIslandHeight(islandNoise_, chunkX, chunkZ, 1, 1);
            if (height <= 0.0F) {
                continue;  // the void: no island here, the column stays air
            }

            const double halfThickness = static_cast<double>(height) * kThicknessPerHeight;
            // A per-column shell perturbation, so island edges undulate instead of
            // forming a clean slab. Fixed in Y (a vertical shell), keyed to world
            // XZ so it is continuous across chunk borders.
            const double shell =
                shapeNoise_.sample(static_cast<double>(worldX) * kShellNoiseScale, 0.0,
                                   static_cast<double>(worldZ) * kShellNoiseScale) *
                kShellNoiseAmplitude;

            for (int y = bottom; y <= top; ++y) {
                // Solid where the column's island band, widened/narrowed by the
                // shell noise, contains y. A positive density is end_stone; the
                // rest of the column is void (air) — never a fluid.
                const double distance = std::abs(static_cast<double>(y - kEndSurfaceY));
                const double density = halfThickness + shell - distance;
                if (density > 0.0) {
                    chunk.setBlock(localX, y, localZ, Block::EndStone);
                }
            }
        }
    }
    return chunk;
}

} // namespace mc::world

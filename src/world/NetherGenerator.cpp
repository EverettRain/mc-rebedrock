#include "world/NetherGenerator.hpp"

#include "world/DimensionGenerator.hpp"
#include "world/WorldConstants.hpp"
#include "world/gen/GenerationSamplers.hpp"

namespace mc::world {
namespace {

// A chunk-local random stream, the way ChunkRandom#setDecoratorSeed keys a
// decoration pass off the world seed and the chunk origin so the result is
// independent of visit order.
[[nodiscard]] gen::JavaRandom chunkRandom(std::uint64_t seed, int chunkX, int chunkZ,
                                          std::uint64_t salt) {
    const auto x = static_cast<std::uint64_t>(static_cast<std::int64_t>(chunkX));
    const auto z = static_cast<std::uint64_t>(static_cast<std::int64_t>(chunkZ));
    return gen::JavaRandom{seed ^ (x * 0x9E3779B97F4A7C15ULL) ^ (z * 0xC2B2AE3D27D4EB4FULL) ^
                           (salt * 0x165667B19E3779F9ULL)};
}

} // namespace

NetherGenerator::NetherGenerator(std::uint64_t worldSeed)
    : netherSeed_(dimensionSeed(worldSeed, DimensionId::Nether)),
      settings_(gen::NoiseGeneratorSettings::nether()),
      biomeSource_(gen::BiomeSource::nether(netherSeed_)),
      samplers_(gen::buildGenerationSamplers(netherSeed_)),
      noiseGenerator_(biomeSource_, settings_, samplers_.lower, samplers_.upper,
                      samplers_.interpolation, samplers_.densityOffset) {}

void NetherGenerator::buildBedrockCap(Chunk& chunk, gen::JavaRandom& random) const {
    const int floorBase = settings_.minY;
    const int roofTop = settings_.minY + settings_.height - 1;
    // A ragged bedrock floor: the bottom row always solid, the next few thinning
    // out, mirroring ChunkGenerator#buildBedrock. The roof does the same from the
    // top down where the dimension has a ceiling.
    for (int localZ = 0; localZ < kChunkWidth; ++localZ) {
        for (int localX = 0; localX < kChunkDepth; ++localX) {
            // ChunkGenerator#buildBedrock: `row <= random.nextInt(n)` — depth 0 is
            // always solid (0 <= anything), and each row above is progressively
            // rarer, so the band is ragged but the very floor/roof is sealed.
            for (int row = 0; row < settings_.bedrockFloorRows; ++row) {
                if (row <= random.nextInt(settings_.bedrockFloorRows)) {
                    chunk.setBlock(localX, floorBase + row, localZ, Block::Bedrock);
                }
            }
            for (int row = 0; row < settings_.bedrockRoofRows; ++row) {
                if (row <= random.nextInt(settings_.bedrockRoofRows)) {
                    chunk.setBlock(localX, roofTop - row, localZ, Block::Bedrock);
                }
            }
        }
    }
}

void NetherGenerator::buildSurface(Chunk& chunk, int chunkX, int chunkZ) const {
    const int top = settings_.minY + settings_.height - 1;
    const int bottom = settings_.minY;
    for (int localX = 0; localX < kChunkWidth; ++localX) {
        for (int localZ = 0; localZ < kChunkDepth; ++localZ) {
            const int worldX = chunkX * kChunkWidth + localX;
            const int worldZ = chunkZ * kChunkDepth + localZ;
            const gen::Biome biome = biomeSource_.biomeAtBlock(worldX, worldZ);
            // The biome's surface palette (from WG-0's BiomeDefinition): soul sand
            // over soul soil in the valley, basalt over blackstone in the deltas,
            // netherrack elsewhere. Applied to the top solid rows of each exposed
            // ledge (a cell whose block is netherrack with air/lava above), the way
            // SurfaceBuilder.NETHER paints the top three blocks.
            const auto& definition = biomeDefinition(biome);
            if (definition.surface == Block::Netherrack) {
                continue;  // netherrack already fills; no repaint needed
            }
            int painted = 0;
            for (int y = top; y > bottom && painted < 3; --y) {
                const Block here = chunk.block(localX, y, localZ);
                if (here != Block::Netherrack) {
                    painted = 0;  // reset the run at any gap so only exposed tops paint
                    continue;
                }
                const Block above = (y + 1 <= top) ? chunk.block(localX, y + 1, localZ) : Block::Air;
                if (painted == 0 && above == Block::Netherrack) {
                    continue;  // buried netherrack, not an exposed surface
                }
                chunk.setBlock(localX, y, localZ,
                               painted == 0 ? definition.surface : definition.filler);
                ++painted;
            }
        }
    }
}

void NetherGenerator::generateFeatures(Chunk& chunk, int chunkX, int chunkZ) const {
    const int top = settings_.minY + settings_.height - 1;
    const int bottom = settings_.minY;

    // Glowstone clusters (GlowstoneBlobFeature): a handful of blobs per chunk hung
    // from a netherrack ceiling near the roof, each a small clump of glowstone.
    gen::JavaRandom glowstone = chunkRandom(netherSeed_, chunkX, chunkZ, 1U);
    const int glowstoneBlobs = glowstone.nextInt(6);  // COUNT_MULTILAYER-ish spread
    for (int blob = 0; blob < glowstoneBlobs; ++blob) {
        const int cx = glowstone.nextInt(kChunkWidth);
        const int cz = glowstone.nextInt(kChunkDepth);
        // Find a ceiling: a netherrack cell with air directly below it, in the
        // upper half of the column.
        int ceilingY = -1;
        for (int y = top - 1; y > settings_.seaLevel; --y) {
            if (chunk.block(cx, y, cz) == Block::Netherrack &&
                chunk.block(cx, y - 1, cz) == Block::Air) {
                ceilingY = y;
                break;
            }
        }
        if (ceilingY < 0) {
            continue;
        }
        const int clump = 8 + glowstone.nextInt(8);
        for (int i = 0; i < clump; ++i) {
            const int gx = cx + glowstone.nextInt(3) - 1;
            const int gy = ceilingY - glowstone.nextInt(3);
            const int gz = cz + glowstone.nextInt(3) - 1;
            if (gx < 0 || gx >= kChunkWidth || gz < 0 || gz >= kChunkDepth || gy <= bottom) {
                continue;
            }
            if (chunk.block(gx, gy, gz) == Block::Air) {
                chunk.setBlock(gx, gy, gz, Block::Glowstone);
            }
        }
    }

    // Nether quartz ore (ReplaceBlobFeature over netherrack): scattered veins
    // throughout the rock, replacing netherrack only.
    gen::JavaRandom quartz = chunkRandom(netherSeed_, chunkX, chunkZ, 2U);
    const int veins = 12 + quartz.nextInt(6);  // ~16 veins/chunk, vanilla-ish
    for (int vein = 0; vein < veins; ++vein) {
        const int cx = quartz.nextInt(kChunkWidth);
        const int cy = bottom + 1 + quartz.nextInt(settings_.height - 2);
        const int cz = quartz.nextInt(kChunkDepth);
        const int size = 4 + quartz.nextInt(6);
        for (int i = 0; i < size; ++i) {
            const int gx = cx + quartz.nextInt(3) - 1;
            const int gy = cy + quartz.nextInt(3) - 1;
            const int gz = cz + quartz.nextInt(3) - 1;
            if (gx < 0 || gx >= kChunkWidth || gz < 0 || gz >= kChunkDepth || gy <= bottom ||
                gy >= top) {
                continue;
            }
            if (chunk.block(gx, gy, gz) == Block::Netherrack) {
                chunk.setBlock(gx, gy, gz, Block::NetherQuartzOre);
            }
        }
    }
}

Chunk NetherGenerator::generate(int chunkX, int chunkZ) const {
    Chunk chunk;
    // Record the biome of each column, so the surface pass and any downstream
    // reader (mesher tint, spawning) can query it.
    for (int localX = 0; localX < kChunkWidth; ++localX) {
        for (int localZ = 0; localZ < kChunkDepth; ++localZ) {
            const int worldX = chunkX * kChunkWidth + localX;
            const int worldZ = chunkZ * kChunkDepth + localZ;
            chunk.setColumnBiome(localX, localZ, biomeSource_.biomeAtBlock(worldX, worldZ));
        }
    }
    // NOISE: netherrack over a lava sea, the noise carving the caverns.
    noiseGenerator_.buildBaseTerrain(chunk, chunkX, chunkZ);
    // The bedrock floor/roof cap seals the column top and bottom.
    gen::JavaRandom bedrock = chunkRandom(netherSeed_, chunkX, chunkZ, 0U);
    buildBedrockCap(chunk, bedrock);
    // SURFACE then FEATURES.
    buildSurface(chunk, chunkX, chunkZ);
    generateFeatures(chunk, chunkX, chunkZ);
    return chunk;
}

} // namespace mc::world

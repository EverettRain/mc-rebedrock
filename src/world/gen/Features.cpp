#include "world/gen/Features.hpp"

#include "world/WorldConstants.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace mc::world::gen {
namespace {

constexpr float kPi = 3.14159265358979323846F;

// DefaultBiomeFeatures#addDefaultOres, one entry per configured ore feature:
// the block, the blob size, how many blobs a chunk gets and the height band the
// COUNT_RANGE decorator draws from.
struct OreConfiguration final {
    Block ore = Block::CoalOre;
    int size = 8;
    int count = 1;
    int minimumY = 0;
    int maximumY = 16;
};

constexpr std::array<OreConfiguration, 10> kOreConfigurations{{
    // The stone variants and the dirt/gravel pockets come first, so an ore blob
    // placed later can still overwrite them. The noise lattice spans 0..255, so
    // the ore bands keep their historical depths there; the filled depth below
    // is uniform stone.
    {Block::Dirt, 33, 10, 0, 256},
    {Block::Gravel, 33, 8, 0, 256},
    {Block::Granite, 33, 10, 0, 80},
    {Block::Diorite, 33, 10, 0, 80},
    {Block::Andesite, 33, 10, 0, 80},
    {Block::CoalOre, 17, 20, 0, 128},
    {Block::IronOre, 9, 20, 0, 64},
    {Block::GoldOre, 9, 2, 0, 32},
    {Block::RedstoneOre, 8, 8, 0, 16},
    {Block::DiamondOre, 8, 1, 0, 16},
}};

// Lapis uses DEPTH_AVERAGE rather than a flat range: it clusters around y=16.
constexpr int kLapisSize = 7;
constexpr int kLapisCount = 1;
constexpr int kLapisCentre = 16;
constexpr int kLapisSpread = 16;

// Emerald only generates in the mountain family, one to eleven single blocks
// between y=4 and y=32.
constexpr int kEmeraldMinimumY = 4;
constexpr int kEmeraldMaximumY = 32;

// How far an ore blob can reach out of the chunk it was rolled in: half the
// capsule (size/8) plus the widest sampling radius (size/16 + 0.5), which for
// the largest configured size of 33 comes to under seven blocks. One ring of
// neighbouring chunks therefore covers every blob that could touch this one.
constexpr int kOreOriginRadius = 1;

[[nodiscard]] bool oreReplaceable(Block block) {
    // OreFeature's `Rules.BASE_STONE_OVERWORLD` target predicate.
    return block == Block::Stone || block == Block::Granite || block == Block::Diorite ||
           block == Block::Andesite;
}

[[nodiscard]] int floorToInt(double value) { return static_cast<int>(std::floor(value)); }

// How close the two dominant surface materials must be before the seam is
// dithered into a soft blend band instead of one hard line. With bilinear
// weights this translates to a transition roughly 1-2 blocks wide.
constexpr float kDitherThreshold = 0.30F;

// A deterministic unit hash of a world position, for the boundary dither and
// anything else that needs a stable per-block random. Unlike the chunk-seeded
// surface random, the same world cell always rolls the same value.
[[nodiscard]] float positionHashUnit(int x, int z) {
    std::uint32_t state = static_cast<std::uint32_t>(x) * 0x9E3779B9U ^
                          static_cast<std::uint32_t>(z) * 0x85EBCA77U;
    state ^= state >> 16U;
    state *= 0x7FEB352DU;
    state ^= state >> 15U;
    state *= 0x846CA68BU;
    state ^= state >> 16U;
    return static_cast<float>(state >> 8U) / static_cast<float>(1U << 24);
}

// The biome map is a 1:4 grid, so a plain per-column lookup switches the
// surface material on hard four-block steps at biome boundaries; over a long
// sand/grass edge that reads as a staircase of right angles. Pick the surface
// material that dominates the column's four surrounding biome cells, weighted
// bilinearly by where the column sits inside its 1:4 cell — cells that share a
// surface block pool their weight (a grass biome next to another grass biome
// stays grass; only a true material edge like grass↔sand blends). The material
// is still one block, but the line it switches along follows the interpolated
// boundary instead of the 4x4 grid, so the seam runs smooth and organic.
[[nodiscard]] int smoothingWinner(
    const std::array<const BiomeDefinition*, 4>& cells,
    const std::array<float, 4>& weights,
    int worldX,
    int worldZ) {
    float bestWeight = -1.0F;
    int best = 0;
    float secondWeight = -1.0F;
    int second = 0;
    for (int index = 0; index < 4; ++index) {
        // Process each distinct surface block once: a group's later cells carry
        // the same aggregated weight, and the runner-up must be a *different*
        // material for the dither below to blend grass↔sand rather than two
        // cells of the same grass.
        bool alreadyCounted = false;
        for (int prior = 0; prior < index; ++prior) {
            if (cells[static_cast<std::size_t>(prior)]->surface ==
                cells[static_cast<std::size_t>(index)]->surface) {
                alreadyCounted = true;
                break;
            }
        }
        if (alreadyCounted) {
            continue;
        }
        float weight = 0.0F;
        for (int other = 0; other < 4; ++other) {
            if (cells[static_cast<std::size_t>(other)]->surface ==
                cells[static_cast<std::size_t>(index)]->surface) {
                weight += weights[static_cast<std::size_t>(other)];
            }
        }
        if (weight > bestWeight) {
            secondWeight = bestWeight;
            second = best;
            bestWeight = weight;
            best = index;
        } else if (weight > secondWeight) {
            secondWeight = weight;
            second = index;
        }
    }
    // Boundary dither: within the narrow band where the two dominant materials
    // trade dominance, pick stochastically so the seam is a soft 1-2 block
    // blend instead of a single hard line. The per-position hash makes the
    // choice stable across chunk re-meshes and world reloads.
    if (secondWeight > 0.0F && bestWeight - secondWeight < kDitherThreshold) {
        if (positionHashUnit(worldX, worldZ) >= bestWeight) {
            return second;
        }
    }
    return best;
}

} // namespace

Features::Features(std::uint64_t seed, const BiomeSource& biomeSource,
                   OctaveSimplexNoiseSampler surfaceDepth)
    : seed_(seed),
      biomeSource_(&biomeSource),
      surfaceDepthNoise_(std::move(surfaceDepth)) {}

int Features::surfaceHeight(const Chunk& chunk, int localX, int localZ) {
    for (int y = kWorldHeight - 1; y >= 0; --y) {
        const auto block = chunk.block(localX, y, localZ);
        if (block != Block::Air && block != Block::Water) {
            return y;
        }
    }
    // No solid surface. −1 is a legal world row now, so the sentinel lives
    // below the world.
    return kMinY - 1;
}

void Features::buildSurface(Chunk& chunk, int chunkX, int chunkZ) const {
    // ChunkGenerator#buildSurface seeds from ChunkRandom#setTerrainSeed, which
    // depends only on the chunk position, never on the world seed.
    JavaRandom random;
    random.setTerrainSeed(chunkX, chunkZ);

    // A column blends its own 1:4 biome cell with the east/north neighbours,
    // so the whole chunk touches a 5x5-cell window. Sample it once (fewer
    // biome queries than the old per-column lookup) and let smoothingWinner
    // anti-alias the surface material across biome boundaries.
    const int baseCellX = chunkX * 4;
    const int baseCellZ = chunkZ * 4;
    std::array<const BiomeDefinition*, 25> cellDefinitions{};
    for (int cellZ = 0; cellZ < 5; ++cellZ) {
        for (int cellX = 0; cellX < 5; ++cellX) {
            const Biome biome =
                biomeSource_->biomeForNoiseGeneration(baseCellX + cellX, baseCellZ + cellZ);
            cellDefinitions[static_cast<std::size_t>(cellZ * 5 + cellX)] =
                &biomeDefinition(biome);
        }
    }

    for (int localZ = 0; localZ < 16; ++localZ) {
        for (int localX = 0; localX < 16; ++localX) {
            const int worldX = chunkX * 16 + localX;
            const int worldZ = chunkZ * 16 + localZ;
            const int cellX = worldX >> 2;
            const int cellZ = worldZ >> 2;
            const int windowX = cellX - baseCellX;   // 0..3
            const int windowZ = cellZ - baseCellZ;   // 0..3
            // The raw 1:4 biome of the column's own cell, for the grass tint:
            // 1.16.1's grass colour follows the biome map (sharp), while the
            // surface material above is the smoothed vote.
            chunk.setColumnBiome(localX, localZ,
                                 cellDefinitions[static_cast<std::size_t>(windowZ * 5 + windowX)]
                                     ->biome);
            const float fractionalX = static_cast<float>(worldX - cellX * 4) * 0.25F;
            const float fractionalZ = static_cast<float>(worldZ - cellZ * 4) * 0.25F;
            const std::array<const BiomeDefinition*, 4> blended{{
                cellDefinitions[static_cast<std::size_t>(windowZ * 5 + windowX)],
                cellDefinitions[static_cast<std::size_t>(windowZ * 5 + windowX + 1)],
                cellDefinitions[static_cast<std::size_t>((windowZ + 1) * 5 + windowX)],
                cellDefinitions[static_cast<std::size_t>((windowZ + 1) * 5 + windowX + 1)],
            }};
            const std::array<float, 4> weights{{
                (1.0F - fractionalX) * (1.0F - fractionalZ),
                fractionalX * (1.0F - fractionalZ),
                (1.0F - fractionalX) * fractionalZ,
                fractionalX * fractionalZ,
            }};
            const auto& definition = *blended[static_cast<std::size_t>(
                smoothingWinner(blended, weights, worldX, worldZ))];
            // SurfaceChunkGenerator#buildSurface samples the simplex surface
            // noise at 1/16 per block and scales by 0.55 * 15; the 0.55 is the
            // NoiseSampler interface's attenuation for the 2D simplex.
            const double depthNoise =
                surfaceDepthNoise_.sample(static_cast<double>(worldX) * 0.0625,
                                          static_cast<double>(worldZ) * 0.0625) *
                0.55 * 15.0;
            // DefaultSurfaceBuilder#generate, verbatim: walk down the column,
            // put the top material on the first solid block, fill `depth` blocks
            // under it, and switch to the underwater material well below the sea.
            Block top = definition.surface;
            Block filler = definition.filler;
            int remaining = -1;
            const int depth =
                static_cast<int>(depthNoise / 3.0 + 3.0 + random.nextDouble() * 0.25);
            for (int y = surfaceHeight(chunk, localX, localZ); y >= 0; --y) {
                const auto block = chunk.block(localX, y, localZ);
                if (block == Block::Air) {
                    remaining = -1;
                    continue;
                }
                if (block != Block::Stone) {
                    continue;
                }
                if (remaining == -1) {
                    if (depth <= 0) {
                        top = Block::Air;
                        filler = Block::Stone;
                    } else if (y >= kSeaLevel - 4 && y <= kSeaLevel + 1) {
                        top = definition.surface;
                        filler = definition.filler;
                    }
                    remaining = depth;
                    if (y >= kSeaLevel - 1) {
                        // A column whose top solid block sits under water (the
                        // cell above is water — normally the y=62 shallow pond
                        // bed) gets the filler material, not grass: a grass
                        // block submerged by generation would otherwise churn
                        // through the random-tick grass-to-dirt conversion for
                        // no player-visible gain. The deep floor below uses the
                        // dedicated underwater material instead.
                        const bool submerged =
                            chunk.block(localX, y + 1, localZ) == Block::Water;
                        chunk.setBlock(localX, y, localZ, submerged ? filler : top);
                    } else if (y < kSeaLevel - 7 - depth) {
                        top = Block::Air;
                        filler = Block::Stone;
                        // The underwater material belongs to real sea floors,
                        // where the cell above is water. Because this pass runs
                        // after carving, an air gap would otherwise route a
                        // buried cave floor down this branch and pave it with a
                        // uniform sheet of gravel; leaving the stone bare keeps
                        // the cave natural. Vanilla never hits this because its
                        // surface pass runs before the carvers.
                        if (chunk.block(localX, y + 1, localZ) == Block::Water) {
                            chunk.setBlock(localX, y, localZ, definition.underwaterSurface);
                        }
                    } else {
                        chunk.setBlock(localX, y, localZ, filler);
                    }
                    continue;
                }
                if (remaining <= 0) {
                    continue;
                }
                --remaining;
                chunk.setBlock(localX, y, localZ, filler);
                if (remaining == 0 && filler == Block::Sand) {
                    // A sand column turns to sandstone underneath, so a desert
                    // does not sit on a bottomless pile of falling sand.
                    remaining = random.nextInt(4) + std::max(0, y - kSeaLevel);
                    filler = Block::Sandstone;
                }
            }
        }
    }

    // ChunkGenerator#buildBedrock: five ragged layers at the bottom of the world.
    for (int localZ = 0; localZ < 16; ++localZ) {
        for (int localX = 0; localX < 16; ++localX) {
            for (int y = kMinY + 4; y >= kMinY; --y) {
                if (y <= random.nextInt(5)) {
                    chunk.setBlock(localX, y, localZ, Block::Bedrock);
                }
            }
        }
    }
}

void Features::placeOreBlob(
    Chunk& chunk,
    int chunkX,
    int chunkZ,
    JavaRandom& random,
    Block ore,
    int size,
    double centreX,
    int centreY,
    double centreZ) {
    // OreFeature#generate: the blob is a capsule between two endpoints spun off
    // a random angle, sampled cell by cell against a squashed sphere.
    //
    // Every draw below happens whether or not the cell it produces lands inside
    // the chunk being written. That is what lets a neighbouring chunk replay
    // this blob and get the identical shape: the random stream must not depend
    // on who is asking.
    const int chunkOriginX = chunkX * 16;
    const int chunkOriginZ = chunkZ * 16;
    const float angle = random.nextFloat() * kPi;
    const double spread = static_cast<double>(size) / 8.0;
    const double offsetX = static_cast<double>(std::sin(angle)) * spread;
    const double offsetZ = static_cast<double>(std::cos(angle)) * spread;
    const double startX = centreX - offsetX;
    const double endX = centreX + offsetX;
    const double startZ = centreZ - offsetZ;
    const double endZ = centreZ + offsetZ;
    const double startY = static_cast<double>(centreY + random.nextInt(3) - 2);
    const double endY = static_cast<double>(centreY + random.nextInt(3) - 2);

    // Most of the blobs a neighbouring chunk rolls cannot touch this one at all.
    // Half the capsule plus the widest sampling radius bounds how far the shape
    // can travel, so a miss can be answered without walking it. The per-step
    // draws still have to happen: one nextDouble is two steps of the LCG, and
    // the stream has to land where it would have landed either way.
    const double reach = spread + static_cast<double>(size) / 16.0 + 2.0;
    if (centreX + reach < static_cast<double>(chunkOriginX) ||
        centreX - reach > static_cast<double>(chunkOriginX + 15) ||
        centreZ + reach < static_cast<double>(chunkOriginZ) ||
        centreZ - reach > static_cast<double>(chunkOriginZ + 15)) {
        random.consume(2 * size);
        return;
    }

    for (int step = 0; step < size; ++step) {
        const double progress = static_cast<double>(step) / static_cast<double>(size);
        const double x = startX + (endX - startX) * progress;
        const double y = startY + (endY - startY) * progress;
        const double z = startZ + (endZ - startZ) * progress;
        const double taper = random.nextDouble() * static_cast<double>(size) / 16.0;
        const double radius =
            (static_cast<double>(std::sin(kPi * static_cast<float>(progress))) + 1.0) * taper +
            1.0;
        const double radiusHalf = radius / 2.0;

        // Clip the sampling window to the chunk up front rather than testing
        // every cell against it: the eight neighbouring origins cost almost
        // nothing that way, because their blobs usually miss entirely.
        const int minX = std::max(floorToInt(x - radiusHalf), chunkOriginX);
        const int maxX = std::min(floorToInt(x + radiusHalf), chunkOriginX + 15);
        const int minY = std::max(floorToInt(y - radiusHalf), 1);
        const int maxY = std::min(floorToInt(y + radiusHalf), kWorldHeight - 1);
        const int minZ = std::max(floorToInt(z - radiusHalf), chunkOriginZ);
        const int maxZ = std::min(floorToInt(z + radiusHalf), chunkOriginZ + 15);
        for (int blockX = minX; blockX <= maxX; ++blockX) {
            const double normalizedX = (static_cast<double>(blockX) + 0.5 - x) / radiusHalf;
            if (normalizedX * normalizedX >= 1.0) continue;
            for (int blockY = minY; blockY <= maxY; ++blockY) {
                const double normalizedY = (static_cast<double>(blockY) + 0.5 - y) / radiusHalf;
                if (normalizedX * normalizedX + normalizedY * normalizedY >= 1.0) continue;
                for (int blockZ = minZ; blockZ <= maxZ; ++blockZ) {
                    const double normalizedZ =
                        (static_cast<double>(blockZ) + 0.5 - z) / radiusHalf;
                    if (normalizedX * normalizedX + normalizedY * normalizedY +
                            normalizedZ * normalizedZ >= 1.0) {
                        continue;
                    }
                    const int localX = blockX - chunkOriginX;
                    const int localZ = blockZ - chunkOriginZ;
                    if (!oreReplaceable(chunk.block(localX, blockY, localZ))) {
                        continue;
                    }
                    chunk.setBlock(localX, blockY, localZ, ore);
                }
            }
        }
    }
}

void Features::generateOres(Chunk& chunk, int chunkX, int chunkZ) const {
    // Every chunk whose blobs could reach this one gets its ore pass replayed,
    // seeded from its own origin so the result does not depend on generation
    // order. Without this a vein is clipped at the chunk it was rolled in and
    // nothing grows back in from next door: roughly a sixth of every blob was
    // being thrown away, and the leftovers ended flat against the border.
    for (int originZ = chunkZ - kOreOriginRadius; originZ <= chunkZ + kOreOriginRadius;
         ++originZ) {
        for (int originX = chunkX - kOreOriginRadius; originX <= chunkX + kOreOriginRadius;
             ++originX) {
            generateOresFrom(chunk, chunkX, chunkZ, originX, originZ);
        }
    }
}

void Features::generateOresFrom(
    Chunk& chunk,
    int chunkX,
    int chunkZ,
    int originX,
    int originZ) const {
    const int originBlockX = originX * 16;
    const int originBlockZ = originZ * 16;
    JavaRandom random;
    const std::int64_t populationSeed =
        random.setPopulationSeed(seed_, originBlockX, originBlockZ);
    int featureIndex = 0;
    for (const auto& configuration : kOreConfigurations) {
        random.setDecoratorSeed(populationSeed, featureIndex++, 6);
        for (int blob = 0; blob < configuration.count; ++blob) {
            // Decorator.COUNT_RANGE picks a uniform position inside the band.
            const int localX = random.nextInt(16);
            const int localZ = random.nextInt(16);
            const int y = random.nextInt(configuration.maximumY - configuration.minimumY) +
                          configuration.minimumY;
            placeOreBlob(chunk, chunkX, chunkZ, random, configuration.ore, configuration.size,
                         static_cast<double>(originBlockX + localX), y,
                         static_cast<double>(originBlockZ + localZ));
        }
    }

    // Lapis clusters around y=16 rather than spreading over a band.
    random.setDecoratorSeed(populationSeed, featureIndex++, 6);
    for (int blob = 0; blob < kLapisCount; ++blob) {
        const int localX = random.nextInt(16);
        const int localZ = random.nextInt(16);
        const int y = random.nextInt(kLapisSpread) + random.nextInt(kLapisSpread) +
                      kLapisCentre - kLapisSpread;
        placeOreBlob(chunk, chunkX, chunkZ, random, Block::LapisOre, kLapisSize,
                     static_cast<double>(originBlockX + localX), y,
                     static_cast<double>(originBlockZ + localZ));
    }

    // Emerald is the one biome-gated ore: single blocks, mountains only. Single
    // blocks never cross a border, so only the chunk's own pass ever writes one
    // — the neighbours still run it to keep the code one shape.
    random.setDecoratorSeed(populationSeed, featureIndex, 6);
    if (biomeSource_->biomeAtBlock(originBlockX + 8, originBlockZ + 8) == Biome::Mountains) {
        const int count = 3 + random.nextInt(6);
        for (int index = 0; index < count; ++index) {
            const int localX = originBlockX + random.nextInt(16) - chunkX * 16;
            const int localZ = originBlockZ + random.nextInt(16) - chunkZ * 16;
            const int y = random.nextInt(kEmeraldMaximumY - kEmeraldMinimumY) + kEmeraldMinimumY;
            if (localX < 0 || localX > 15 || localZ < 0 || localZ > 15) {
                continue;
            }
            // EmeraldOreFeature replaces plain stone only, not the stone variants.
            if (chunk.block(localX, y, localZ) == Block::Stone) {
                chunk.setBlock(localX, y, localZ, Block::EmeraldOre);
            }
        }
    }
}

bool Features::placeTree(
    Chunk& chunk,
    int chunkX,
    int chunkZ,
    JavaRandom& random,
    const TreeChoice& choice,
    int localX,
    int groundY,
    int localZ,
    std::vector<TreeBorderBlock>& borderBlocks) {
    ChunkTreeWriter writer{chunk, chunkX, chunkZ, borderBlocks};
    return growTree(
        writer, random, choice, chunkX * kChunkWidth + localX, groundY,
        chunkZ * kChunkDepth + localZ);
}

void Features::generateVegetation(
    Chunk& chunk,
    int chunkX,
    int chunkZ,
    std::vector<TreeBorderBlock>& borderBlocks) const {
    JavaRandom random;
    const std::int64_t populationSeed = random.setPopulationSeed(seed_, chunkX * 16, chunkZ * 16);
    const auto& definition =
        biomeDefinition(biomeSource_->biomeAtBlock(chunkX * 16 + 8, chunkZ * 16 + 8));

    // Decorator.COUNT_EXTRA: a fixed number of trees plus one more now and then.
    random.setDecoratorSeed(populationSeed, 0, 9);
    if (!definition.trees.empty()) {
        // The deepest water any of the biome's trees tolerates: the swamp's
        // maxWaterDepth(1) lets its oaks grow through a single standing-water
        // block, while every other biome accepts none.
        int maxWaterDepth = 0;
        for (const auto& choice : definition.trees) {
            maxWaterDepth = std::max(maxWaterDepth, choice.maxWaterDepth);
        }
        int treeCount = definition.treeCount;
        if (random.nextFloat() < definition.extraTreeChance) {
            treeCount += definition.extraTreeCount;
        }
        for (int tree = 0; tree < treeCount; ++tree) {
            // A random cell often lands in open water in the flooded swamp, so
            // retry a few times to find a column shallow enough to root in —
            // vanilla's swamp is nearly all plantable, this compensates for the
            // drowned patches without changing the per-chunk tree count.
            int localX = 0;
            int localZ = 0;
            int groundY = -1;
            for (int attempt = 0; attempt < 8; ++attempt) {
                const int candidateX = random.nextInt(16);
                const int candidateZ = random.nextInt(16);
                const int candidateY = surfaceHeight(chunk, candidateX, candidateZ);
                if (candidateY >= kSeaLevel - maxWaterDepth && candidateY >= 0 &&
                    isSoilForPlants(chunk.block(candidateX, candidateY, candidateZ))) {
                    localX = candidateX;
                    localZ = candidateZ;
                    groundY = candidateY;
                    break;
                }
            }
            if (groundY < kMinY) {
                continue;
            }
            // RandomFeature picks by weight; the list is already normalised.
            const float roll = random.nextFloat();
            float accumulated = 0.0F;
            const TreeChoice* chosen = &definition.trees.back();
            for (const auto& choice : definition.trees) {
                accumulated += choice.weight;
                if (roll < accumulated) {
                    chosen = &choice;
                    break;
                }
            }
            static_cast<void>(placeTree(chunk, chunkX, chunkZ, random, *chosen, localX, groundY,
                                        localZ, borderBlocks));
        }
    }

    // Grass and flowers, on whatever ground the trees left uncovered.
    random.setDecoratorSeed(populationSeed, 1, 9);
    for (int patch = 0; patch < definition.grassCount; ++patch) {
        const int localX = random.nextInt(16);
        const int localZ = random.nextInt(16);
        const int groundY = surfaceHeight(chunk, localX, localZ);
        if (!isWorldYInRange(groundY) || !isWorldYInRange(groundY + 1)) continue;
        // Feature.RANDOM_PATCH scatters up to 64 tries around the centre.
        for (int attempt = 0; attempt < 24; ++attempt) {
            const int x = localX + random.nextInt(8) - random.nextInt(8);
            const int z = localZ + random.nextInt(8) - random.nextInt(8);
            if (x < 0 || x > 15 || z < 0 || z > 15) continue;
            const int y = surfaceHeight(chunk, x, z);
            if (!isWorldYInRange(y) || !isWorldYInRange(y + 1)) continue;
            if (isSoilForPlants(chunk.block(x, y, z)) && chunk.block(x, y + 1, z) == Block::Air) {
                chunk.setBlock(x, y + 1, z, Block::GrassPlant);
            }
        }
    }
    random.setDecoratorSeed(populationSeed, 2, 9);
    for (int patch = 0; patch < definition.flowerCount; ++patch) {
        const int x = random.nextInt(16);
        const int z = random.nextInt(16);
        const int y = surfaceHeight(chunk, x, z);
        if (!isWorldYInRange(y) || !isWorldYInRange(y + 1)) continue;
        if (isSoilForPlants(chunk.block(x, y, z)) && chunk.block(x, y + 1, z) == Block::Air) {
            chunk.setBlock(x, y + 1, z, Block::Dandelion);
        }
    }
}

} // namespace mc::world::gen

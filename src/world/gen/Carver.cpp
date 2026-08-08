#include "world/gen/Carver.hpp"

#include "world/WorldConstants.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace mc::world::gen {
namespace {

constexpr float kPi = 3.14159265358979323846F;
// CaveCarver#getMaxCaveCount.
constexpr int kMaximumCaveCount = 15;
// The lowest layer a carver is allowed to break into, so bedrock stays sealed.
constexpr int kMinimumCarveY = 1;
// CaveCarver#carveAtPoint: carved cells become lava below this Y, cave air above.
constexpr int kLavaFloorY = 11;

[[nodiscard]] int floorToInt(double value) {
    return static_cast<int>(std::floor(value));
}

// CaveCarver#isPositionExcluded: the -0.7 bias flattens the ellipsoid's
// underside, which is what leaves cave floors walkable instead of round.
[[nodiscard]] bool cavePositionExcluded(
    double scaledX, double scaledY, double scaledZ, int) {
    return scaledY <= -0.7 ||
           scaledX * scaledX + scaledY * scaledY + scaledZ * scaledZ >= 1.0;
}

// Carver#canCarveBranch: gives up once the tunnel has walked further from the
// chunk being written than anything it could still carve there.
[[nodiscard]] bool canCarveBranch(
    int mainChunkX,
    int mainChunkZ,
    double x,
    double z,
    int branch,
    int branchCount,
    float width) {
    const double centreX = static_cast<double>(mainChunkX * 16 + 8);
    const double centreZ = static_cast<double>(mainChunkZ * 16 + 8);
    const double deltaX = x - centreX;
    const double deltaZ = z - centreZ;
    const double remaining = static_cast<double>(branchCount - branch);
    const double reach = static_cast<double>(width) + 2.0 + 16.0;
    return deltaX * deltaX + deltaZ * deltaZ - remaining * remaining <= reach * reach;
}

// Carver#canAlwaysCarveBlock plus the sand/gravel rule from #canCarveBlock:
// the block set a carver may break. Lava and bedrock are deliberately absent,
// so a lava pool laid down by one region is never re-carved by another.
[[nodiscard]] bool carvable(Block block) {
    return block == Block::Stone || block == Block::Dirt || block == Block::Grass ||
           block == Block::Sand || block == Block::Gravel || block == Block::Andesite ||
           block == Block::Granite || block == Block::Diorite || block == Block::CoarseDirt ||
           block == Block::Podzol || block == Block::RedSand || block == Block::SnowBlock ||
           block == Block::Sandstone;
}

// Whether a sand/gravel cell may be carved: never when the cell above is water,
// which is vanilla's `canCarveBlock` exception for the two falling blocks.
[[nodiscard]] bool canCarveUnderWater(const Chunk& chunk, int localX, int worldY, int localZ) {
    if (worldY + 1 >= kWorldHeight) {
        return true;
    }
    return chunk.block(localX, worldY + 1, localZ) != Block::Water;
}

// CanyonCarver.heightToHorizontalStretchFactor: the ravine's width profile, a
// noisy taper that gives it its ragged walls.
[[nodiscard]] std::array<float, 1024> ravineWidthProfile(JavaRandom& random) {
    std::array<float, 1024> profile{};
    float value = 1.0F;
    for (int index = 0; index < 1024; ++index) {
        if (index == 0 || random.nextInt(3) == 0) {
            value = 1.0F + random.nextFloat() * random.nextFloat();
        }
        profile[static_cast<std::size_t>(index)] = value * value;
    }
    return profile;
}

} // namespace

void Carver::carveRegion(
    Chunk& chunk,
    int mainChunkX,
    int mainChunkZ,
    double x,
    double y,
    double z,
    double horizontalRadius,
    double verticalRadius,
    const PositionExcluded& isExcluded) {
    const double chunkOriginX = static_cast<double>(mainChunkX * 16);
    const double chunkOriginZ = static_cast<double>(mainChunkZ * 16);
    int minX = floorToInt(x - horizontalRadius) - mainChunkX * 16 - 1;
    int maxX = floorToInt(x + horizontalRadius) - mainChunkX * 16 + 1;
    int minY = std::max(floorToInt(y - verticalRadius) - 1, kMinimumCarveY);
    int maxY = std::min(floorToInt(y + verticalRadius) + 1, kWorldHeight - 8);
    int minZ = floorToInt(z - horizontalRadius) - mainChunkZ * 16 - 1;
    int maxZ = floorToInt(z + horizontalRadius) - mainChunkZ * 16 + 1;
    minX = std::max(minX, 0);
    maxX = std::min(maxX, 15);
    minZ = std::max(minZ, 0);
    maxZ = std::min(maxZ, 15);
    if (minX > maxX || minZ > maxZ || minY > maxY) {
        return;
    }
    // Carver#isRegionUncarvable: never open a cave into standing water, so a
    // tunnel that would break into an ocean is skipped whole rather than
    // flooding the surrounding stone.
    if (isRegionUncarvable(chunk, minX, maxX, minY, maxY, minZ, maxZ)) {
        return;
    }

    for (int localX = minX; localX <= maxX; ++localX) {
        const double normalizedX =
            (chunkOriginX + static_cast<double>(localX) + 0.5 - x) / horizontalRadius;
        for (int localZ = minZ; localZ <= maxZ; ++localZ) {
            const double normalizedZ =
                (chunkOriginZ + static_cast<double>(localZ) + 0.5 - z) / horizontalRadius;
            if (normalizedX * normalizedX + normalizedZ * normalizedZ >= 1.0) {
                continue;
            }
            for (int worldY = maxY; worldY >= minY; --worldY) {
                const double normalizedY =
                    (static_cast<double>(worldY) + 0.5 - y) / verticalRadius;
                if (isExcluded(normalizedX, normalizedY, normalizedZ, worldY)) {
                    continue;
                }
                const auto block = chunk.block(localX, worldY, localZ);
                if (!carvable(block)) {
                    continue;
                }
                if ((block == Block::Sand || block == Block::Gravel) &&
                    !canCarveUnderWater(chunk, localX, worldY, localZ)) {
                    continue;
                }
                // CaveCarver#carveAtPoint: lava below the lava floor, cave air
                // above it. The surface pass runs afterwards, so a cave mouth
                // still gets its grass lining.
                if (worldY < kLavaFloorY) {
                    chunk.setBlock(localX, worldY, localZ, Block::Lava);
                } else {
                    chunk.setBlock(localX, worldY, localZ, Block::Air);
                }
            }
        }
    }
}

bool Carver::isRegionUncarvable(
    const Chunk& chunk, int minX, int maxX, int minY, int maxY, int minZ, int maxZ) {
    for (int localX = minX; localX <= maxX; ++localX) {
        for (int localZ = minZ; localZ <= maxZ; ++localZ) {
            for (int worldY = minY - 1; worldY <= maxY + 1; ++worldY) {
                if (worldY < 0 || worldY >= kWorldHeight) {
                    continue;
                }
                if (chunk.block(localX, worldY, localZ) == Block::Water) {
                    return true;
                }
            }
        }
    }
    return false;
}

void Carver::carveRoom(
    Chunk& chunk,
    std::int64_t roomSeed,
    int mainChunkX,
    int mainChunkZ,
    double x,
    double y,
    double z,
    float width) const {
    const double radius = 1.5 + static_cast<double>(std::sin(kPi * 0.5F) * width);
    // CaveCarver#carveCave: the room sits one block east of the roll, and its
    // vertical radius is half the horizontal one.
    carveRegion(chunk, mainChunkX, mainChunkZ, x + 1.0, y, z, radius, radius * 0.5,
                PositionExcluded{cavePositionExcluded});
}

void Carver::carveTunnel(
    Chunk& chunk,
    std::int64_t tunnelSeed,
    int mainChunkX,
    int mainChunkZ,
    double x,
    double y,
    double z,
    float width,
    float yaw,
    float pitch,
    int startBranch,
    int branchCount,
    double heightRatio) const {
    JavaRandom random{static_cast<std::uint64_t>(tunnelSeed)};
    const int forkBranch = random.nextInt(branchCount / 2) + branchCount / 4;
    const bool steep = random.nextInt(6) == 0;
    float yawDrift = 0.0F;
    float pitchDrift = 0.0F;

    for (int branch = startBranch; branch < branchCount; ++branch) {
        const double horizontalRadius =
            1.5 + static_cast<double>(std::sin(static_cast<float>(branch) * kPi /
                                               static_cast<float>(branchCount)) *
                                      width);
        const double verticalRadius = horizontalRadius * heightRatio;
        const float pitchCosine = std::cos(pitch);
        x += static_cast<double>(std::cos(yaw) * pitchCosine);
        y += static_cast<double>(std::sin(pitch));
        z += static_cast<double>(std::sin(yaw) * pitchCosine);
        pitch *= steep ? 0.92F : 0.7F;
        pitch += pitchDrift * 0.1F;
        yaw += yawDrift * 0.1F;
        pitchDrift *= 0.9F;
        yawDrift *= 0.75F;
        pitchDrift += (random.nextFloat() - random.nextFloat()) * random.nextFloat() * 2.0F;
        yawDrift += (random.nextFloat() - random.nextFloat()) * random.nextFloat() * 4.0F;

        if (branch == forkBranch && width > 1.0F) {
            // The tunnel splits into two thinner branches at right angles and
            // stops carrying on itself, which is where cave junctions come from.
            // Vanilla draws seed, width, seed, width from the parent's stream.
            const std::int64_t firstSeed = random.nextLong();
            const float firstWidth = random.nextFloat() * 0.5F + 0.5F;
            const std::int64_t secondSeed = random.nextLong();
            const float secondWidth = random.nextFloat() * 0.5F + 0.5F;
            carveTunnel(chunk, firstSeed, mainChunkX, mainChunkZ, x, y, z, firstWidth,
                        yaw - kPi * 0.5F, pitch / 3.0F, branch, branchCount, 1.0);
            carveTunnel(chunk, secondSeed, mainChunkX, mainChunkZ, x, y, z, secondWidth,
                        yaw + kPi * 0.5F, pitch / 3.0F, branch, branchCount, 1.0);
            return;
        }
        if (random.nextInt(4) == 0) {
            continue;
        }
        if (!canCarveBranch(mainChunkX, mainChunkZ, x, z, branch, branchCount, width)) {
            return;
        }
        carveRegion(chunk, mainChunkX, mainChunkZ, x, y, z, horizontalRadius, verticalRadius,
                    PositionExcluded{cavePositionExcluded});
    }
}

void Carver::carveCaves(
    Chunk& chunk,
    JavaRandom& random,
    int originChunkX,
    int originChunkZ,
    int mainChunkX,
    int mainChunkZ) const {
    const int branchCap = (kBranchFactor * 2 - 1) * 16;
    // The triple-nested nextInt is what makes most chunks hold one or two cave
    // systems and a rare one hold a dozen.
    const int caveCount =
        random.nextInt(random.nextInt(random.nextInt(kMaximumCaveCount) + 1) + 1);
    for (int cave = 0; cave < caveCount; ++cave) {
        const double x = static_cast<double>(originChunkX * 16 + random.nextInt(16));
        const double y = static_cast<double>(random.nextInt(random.nextInt(120) + 8));
        const double z = static_cast<double>(originChunkZ * 16 + random.nextInt(16));
        int tunnelCount = 1;
        if (random.nextInt(4) == 0) {
            const float roomWidth = 1.0F + random.nextFloat() * 6.0F;
            const std::int64_t roomSeed = random.nextLong();
            carveRoom(chunk, roomSeed, mainChunkX, mainChunkZ, x, y, z, roomWidth);
            tunnelCount += random.nextInt(4);
        }
        for (int tunnel = 0; tunnel < tunnelCount; ++tunnel) {
            const float yaw = random.nextFloat() * kPi * 2.0F;
            const float pitch = (random.nextFloat() - 0.5F) / 4.0F;
            float width = random.nextFloat() * 2.0F + random.nextFloat();
            if (random.nextInt(10) == 0) {
                width *= random.nextFloat() * random.nextFloat() * 3.0F + 1.0F;
            }
            const int branchCount = branchCap - random.nextInt(branchCap / 4);
            const std::int64_t tunnelSeed = random.nextLong();
            carveTunnel(chunk, tunnelSeed, mainChunkX, mainChunkZ, x, y, z, width, yaw, pitch, 0,
                        branchCount, 1.0);
        }
    }
}

void Carver::carveRavine(
    Chunk& chunk,
    JavaRandom& random,
    int originChunkX,
    int originChunkZ,
    int mainChunkX,
    int mainChunkZ) const {
    const int branchCap = (kBranchFactor * 2 - 1) * 16;
    double x = static_cast<double>(originChunkX * 16 + random.nextInt(16));
    double y = static_cast<double>(random.nextInt(random.nextInt(40) + 8) + 20);
    double z = static_cast<double>(originChunkZ * 16 + random.nextInt(16));
    float yaw = random.nextFloat() * kPi * 2.0F;
    float pitch = (random.nextFloat() - 0.5F) * 2.0F / 8.0F;
    const float width = (random.nextFloat() * 2.0F + random.nextFloat()) * 2.0F;
    const int branchCount = branchCap - random.nextInt(branchCap / 4);

    JavaRandom tunnelRandom{static_cast<std::uint64_t>(random.nextLong())};
    const auto widthProfile = ravineWidthProfile(tunnelRandom);
    // RavineCarver#isPositionExcluded: the width profile squeezes the walls per
    // Y level, and the vertical term stretches the ellipsoid to ~2.45x.
    const auto ravineExcluded = [&widthProfile](double scaledX, double scaledY,
                                                double scaledZ, int worldY) {
        const double squeeze =
            static_cast<double>(widthProfile[static_cast<std::size_t>(
                std::clamp(worldY - 1, 0, 1023))]);
        return (scaledX * scaledX + scaledZ * scaledZ) * squeeze +
               scaledY * scaledY / 6.0 >= 1.0;
    };
    float yawDrift = 0.0F;
    float pitchDrift = 0.0F;
    for (int branch = 0; branch < branchCount; ++branch) {
        double horizontalRadius =
            1.5 + static_cast<double>(std::sin(static_cast<float>(branch) * kPi /
                                               static_cast<float>(branchCount)) *
                                      width);
        double verticalRadius = horizontalRadius * 3.0;
        // RavineCarver#carveRavine jitters both radii by a random 0.75..1.0,
        // which is what leaves the ledges along its walls.
        horizontalRadius *= static_cast<double>(tunnelRandom.nextFloat() * 0.25F + 0.75F);
        verticalRadius *= static_cast<double>(tunnelRandom.nextFloat() * 0.25F + 0.75F);
        const float pitchCosine = std::cos(pitch);
        x += static_cast<double>(std::cos(yaw) * pitchCosine);
        y += static_cast<double>(std::sin(pitch));
        z += static_cast<double>(std::sin(yaw) * pitchCosine);
        pitch *= 0.7F;
        pitch += pitchDrift * 0.05F;
        yaw += yawDrift * 0.05F;
        pitchDrift *= 0.8F;
        yawDrift *= 0.5F;
        pitchDrift += (tunnelRandom.nextFloat() - tunnelRandom.nextFloat()) *
                      tunnelRandom.nextFloat() * 2.0F;
        yawDrift += (tunnelRandom.nextFloat() - tunnelRandom.nextFloat()) *
                    tunnelRandom.nextFloat() * 4.0F;
        if (tunnelRandom.nextInt(4) == 0) {
            continue;
        }
        if (!canCarveBranch(mainChunkX, mainChunkZ, x, z, branch, branchCount, width)) {
            return;
        }
        carveRegion(chunk, mainChunkX, mainChunkZ, x, y, z, horizontalRadius, verticalRadius,
                    PositionExcluded{ravineExcluded});
    }
}

void Carver::carveChunk(Chunk& chunk, int chunkX, int chunkZ) const {
    // Every carver that could reach this chunk gets its turn, seeded from its
    // own origin chunk so the result does not depend on generation order. The
    // neighbourhood is vanilla's hardcoded ±8 chunks, while each carver's step
    // budget comes from kBranchFactor.
    for (int originZ = chunkZ - kCarveNeighborhood; originZ <= chunkZ + kCarveNeighborhood;
         ++originZ) {
        for (int originX = chunkX - kCarveNeighborhood; originX <= chunkX + kCarveNeighborhood;
             ++originX) {
            JavaRandom random;
            random.setCarverSeed(seed_, originX, originZ);
            if (random.nextFloat() <= kCaveProbability) {
                carveCaves(chunk, random, originX, originZ, chunkX, chunkZ);
            }
            // The canyon is the second carver in the biome's AIR step, so its
            // seed carries vanilla's index salt of 1.
            JavaRandom ravineRandom;
            ravineRandom.setCarverSeed(seed_ + 1ULL, originX, originZ);
            if (ravineRandom.nextFloat() <= kRavineProbability) {
                carveRavine(chunk, ravineRandom, originX, originZ, chunkX, chunkZ);
            }
        }
    }
}

} // namespace mc::world::gen

#pragma once

#include "world/Chunk.hpp"
#include "world/gen/JavaRandom.hpp"

#include <cstdint>
#include <functional>

namespace mc::world::gen {

// vanilla's ConfiguredCarvers.CAVE and .CANYON.
//
// Both work the same way: a carver is seeded from a chunk position, digs a
// tunnel system that may run well past that chunk's own borders, and only ever
// writes into the one chunk it was handed. Generating chunk C therefore runs
// every carver seeded in the 17x17 neighbourhood around it, which is what makes
// a cave that starts eight chunks away arrive intact however the chunks happen
// to be visited.
class Carver final {
  public:
    // Carver#getBranchFactor: vanilla returns 4, and a tunnel system budgets
    // (2*4-1)*16 = 112 steps. (The carve neighbourhood is the separate, larger
    // constant below.)
    static constexpr int kBranchFactor = 4;
    // ChunkGenerator#carve runs every carver whose origin chunk lies within
    // this many chunks of the chunk being written; vanilla hardcodes 8 here,
    // independent of the branch factor.
    static constexpr int kCarveNeighborhood = 8;
    // ConfiguredCarvers.CAVE / .CANYON probabilities.
    static constexpr float kCaveProbability = 0.14285715F;
    static constexpr float kRavineProbability = 0.02F;

    explicit Carver(std::uint64_t seed) : seed_(seed) {}

    // Runs every carver whose origin chunk can reach `chunk`, which sits at
    // (chunkX, chunkZ).
    void carveChunk(Chunk& chunk, int chunkX, int chunkZ) const;

  private:
    // Carver#isPositionExcluded decides whether one cell of the carving
    // ellipsoid is carved. The cave keeps the underside flat; the ravine
    // squeezes the sphere with its per-Y width profile and stretches it
    // vertically.
    using PositionExcluded =
        std::function<bool(double scaledX, double scaledY, double scaledZ, int worldY)>;

    void carveCaves(
        Chunk& chunk,
        JavaRandom& random,
        int originChunkX,
        int originChunkZ,
        int mainChunkX,
        int mainChunkZ) const;
    void carveRavine(
        Chunk& chunk,
        JavaRandom& random,
        int originChunkX,
        int originChunkZ,
        int mainChunkX,
        int mainChunkZ) const;
    // CaveCarver#carveTunnels: the random walk that drags an ellipsoid along.
    void carveTunnel(
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
        double heightRatio) const;
    // CaveCarver#carveCave: the single wide room a tunnel system sometimes opens
    // with.
    void carveRoom(
        Chunk& chunk,
        std::int64_t roomSeed,
        int mainChunkX,
        int mainChunkZ,
        double x,
        double y,
        double z,
        float width) const;
    // Carver#carveRegion: hollows one ellipsoid out of the chunk.
    static void carveRegion(
        Chunk& chunk,
        int mainChunkX,
        int mainChunkZ,
        double x,
        double y,
        double z,
        double horizontalRadius,
        double verticalRadius,
        const PositionExcluded& isExcluded);
    // Carver#isRegionUncarvable: no cell of a region is carved when any cell of
    // its bounding box already holds a carvable fluid.
    [[nodiscard]] static bool isRegionUncarvable(
        const Chunk& chunk, int minX, int maxX, int minY, int maxY, int minZ, int maxZ);

    std::uint64_t seed_ = 0U;
};

} // namespace mc::world::gen

#include "world/gen/TheEndBiomeSource.hpp"

#include "world/gen/EndIslandHeight.hpp"

namespace mc::world::gen {

TheEndBiomeSource::TheEndBiomeSource(std::uint64_t seed) : islandNoise_(buildEndIslandNoise(seed)) {}

Biome TheEndBiomeSource::sample(int quartX, int quartZ) const {
    // TheEndBiomeSource#getNoiseBiome: work in chunk space (a biome cell is a
    // chunk-ish region in the end). The quart position is 1:4 blocks, so >>2 more
    // gets to chunk coordinates.
    const int blockX = quartX << 2;
    const int blockZ = quartZ << 2;
    const int chunkX = blockX >> 4;
    const int chunkZ = blockZ >> 4;

    // The central disc (64 chunks radius) is the main island's biome, TheEnd.
    if (static_cast<std::int64_t>(chunkX) * chunkX + static_cast<std::int64_t>(chunkZ) * chunkZ <=
        4096L) {
        return Biome::TheEnd;
    }

    // Outside it, a continuous height value picks the biome by vanilla's threshold
    // ladder (highlands / midlands / barrens / small islands). Vanilla reads this
    // from a noise that reliably spans [-1, 1]; this port samples the island noise
    // continuously (a smooth field, unlike the sparse clamped island *height*,
    // which is -100 across the void) so the four outer biomes form real rings
    // regardless of how sparse the island cones happen to be for a seed. The
    // terrain (EndGenerator) still raises its islands from the clamped cone field;
    // the biome is the softer classification vanilla layers over the same noise.
    const double value = islandNoise_.sample(static_cast<double>(chunkX) * 0.5,
                                             static_cast<double>(chunkZ) * 0.5);
    if (value > 0.25) {
        return Biome::EndHighlands;
    }
    if (value >= -0.0625) {
        return Biome::EndMidlands;
    }
    if (value < -0.21875) {
        return Biome::SmallEndIslands;
    }
    return Biome::EndBarrens;
}

} // namespace mc::world::gen

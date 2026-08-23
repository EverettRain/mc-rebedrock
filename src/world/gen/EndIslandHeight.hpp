#pragma once

#include "world/gen/NoiseSampler.hpp"

#include <cmath>
#include <cstdint>

namespace mc::world::gen {

// Java 1.16.1's ChunkGeneratorEnd island-height field, shared by TheEndBiomeSource
// (which reads it to pick a biome by distance/height) and EndGenerator (which
// reads it to raise the floating islands). Kept in one header so the two can
// never drift — the biome and the terrain agree on where an island is.
//
// The field is a height value in roughly [-100, 80]: the central island is a tall
// plateau around (0,0) that falls off with distance, and the outer islands are
// wherever a coarse simplex "island noise" dips below -0.9, each a small cone of
// its own. Between them the value is deeply negative — the End void.

// The island noise seeded the way vanilla does: a SimplexNoiseSampler drawn after
// the terrain samplers, off a stream salted so it is independent of them. Callers
// build one per generator/biome-source and pass it in.
[[nodiscard]] inline SimplexNoiseSampler buildEndIslandNoise(std::uint64_t seed) {
    // Vanilla draws the island noise from a fresh ChunkRandom(seed) after the
    // terrain samplers consume 17292 entries; standing it up on its own stream
    // with a salt keeps it deterministic per seed and off the terrain stream.
    JavaRandom random{seed ^ 0x1234567ABCDEF01ULL};
    random.consume(17292);
    return SimplexNoiseSampler{random};
}

// ChunkGeneratorEnd#getIslandHeight: the height value for a *biome quart* column,
// computed the way vanilla computes it — over the chunk grid, scanning a 25x25
// neighbourhood of chunks for outer-island seeds. `blockX`/`blockZ` are block
// coordinates; centerX/centerZ are the sub-chunk sample offsets (vanilla passes
// the chunk-cell centre).
[[nodiscard]] inline float endIslandHeight(const SimplexNoiseSampler& islandNoise, int chunkX,
                                           int chunkZ, int centerX, int centerZ) {
    const auto fx = static_cast<float>(chunkX * 2 + centerX);
    const auto fz = static_cast<float>(chunkZ * 2 + centerZ);
    // The central island: 100 at the origin, falling 8 per unit distance, clamped
    // so it neither spikes nor drops past the outer-island band.
    float height = 100.0F - std::sqrt(fx * fx + fz * fz) * 8.0F;
    if (height > 80.0F) {
        height = 80.0F;
    } else if (height < -100.0F) {
        height = -100.0F;
    }

    // The outer islands: any chunk in the 25x25 neighbourhood whose island noise
    // dips below -0.9 seeds a small cone, and this column takes the tallest cone
    // that reaches it. The `> 4096` guard is the void ring — no outer island forms
    // within 64 chunks of the centre, so the central island sits alone.
    for (int rx = -12; rx <= 12; ++rx) {
        for (int rz = -12; rz <= 12; ++rz) {
            const std::int64_t lx = static_cast<std::int64_t>(chunkX) + rx;
            const std::int64_t lz = static_cast<std::int64_t>(chunkZ) + rz;
            if (lx * lx + lz * lz > 4096L &&
                islandNoise.sample(static_cast<double>(lx), static_cast<double>(lz)) < -0.9) {
                const float noiseHeight =
                    static_cast<float>((std::abs(lx) * 3439 + std::abs(lz) * 147) % 13 + 9);
                const auto ex = static_cast<float>(centerX - rx * 2);
                const auto ez = static_cast<float>(centerZ - rz * 2);
                float cone = 100.0F - std::sqrt(ex * ex + ez * ez) * noiseHeight;
                if (cone > 80.0F) {
                    cone = 80.0F;
                } else if (cone < -100.0F) {
                    cone = -100.0F;
                }
                if (cone > height) {
                    height = cone;
                }
            }
        }
    }
    return height;
}

} // namespace mc::world::gen

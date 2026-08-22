#pragma once

#include "world/Dimension.hpp"

#include <cstdint>

namespace mc::world {

// DIM-3: the per-dimension generation seam. Two things live here, both
// front-ready without any Nether/End terrain algorithm (that is the worldgen
// subtree's delivery — see the blocked note below):
//
//   1. dimensionSeed(worldSeed, id): the deterministic per-dimension seed. Same
//      world + same dimension always derives the same terrain seed, and two
//      dimensions of one world derive *different* seeds, so the Nether's noise is
//      never the Overworld's (DIM DESIGN §4: dimSeed = hash(worldSeed, id)).
//
//   2. DimensionGeneratorConfig + dimensionGeneratorConfig(id): the hook a
//      per-dimension streamer reads to know *which* pipeline and *what* height
//      bounds a dimension generates against. The height/ceiling come from the
//      DimensionType (DIM-0), never a hardcoded 256. `hasTerrainGenerator` is the
//      seam: true only where a real generator exists on disk today (the Overworld
//      SurfaceGenerator). The Nether and the End report false until the worldgen
//      subtree delivers their generators; a streamer must not fabricate their
//      terrain (that would be the wrong content, and generating in a tick is the
//      lowframe long-tail root cause anyway).

// A 64-bit avalanche (SplitMix64 finalizer) — a well-distributed, deterministic
// mix so a small dimension ordinal perturbs the whole seed rather than a couple
// of low bits.
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) {
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31;
    return value;
}

// The terrain seed for one dimension of a world. The Overworld keeps the world
// seed unchanged, so an existing single-dimension world regenerates byte-for-byte
// (no migration); the Nether and the End fold their DimensionId ordinal in, so
// each derives its own stream. Deterministic and pure.
[[nodiscard]] constexpr std::uint64_t dimensionSeed(std::uint64_t worldSeed, DimensionId id) {
    if (id == DimensionId::Overworld) {
        return worldSeed;  // unchanged: no regression for the existing world
    }
    // Fold the ordinal through the avalanche with a per-dimension salt, so the
    // Nether and the End are far apart in seed space and neither equals the
    // Overworld's plain worldSeed.
    const auto salt = static_cast<std::uint64_t>(id) * 0x9E3779B97F4A7C15ULL;
    return mix64(worldSeed ^ salt);
}

// What a per-dimension streamer needs to know to generate (or to decline to). A
// value POD read by DimensionId — no vtable, no per-dimension object graph.
struct DimensionGeneratorConfig final {
    DimensionId id = DimensionId::Overworld;
    // The vertical bounds this dimension generates within, straight from the
    // DimensionType — the Overworld's -64..320, the Nether's 0..256 under a
    // ceiling. A generator reads these instead of assuming 256.
    std::int32_t minY = 0;
    std::int32_t height = 256;
    bool hasCeiling = false;
    // The seam: is there a real terrain generator for this dimension on disk?
    // Only the Overworld today. False means "worldgen has not delivered this
    // dimension's generator yet" — a streamer skips generation rather than
    // producing the wrong terrain.
    bool hasTerrainGenerator = false;

    [[nodiscard]] bool operator==(const DimensionGeneratorConfig&) const = default;
};

// The generation config for a dimension: its bounds from the DimensionType and
// whether a generator exists. A subscript-shaped lookup (switch on the dense id),
// not a string map.
[[nodiscard]] constexpr DimensionGeneratorConfig dimensionGeneratorConfig(DimensionId id) {
    const DimensionType& type = dimensionType(id);
    return DimensionGeneratorConfig{
        .id = id,
        .minY = type.minY,
        .height = type.height,
        .hasCeiling = type.hasCeiling,
        // Only the Overworld has a terrain generator (SurfaceGenerator) today.
        // The Nether/End seam stays false until the worldgen subtree fills it.
        .hasTerrainGenerator = (id == DimensionId::Overworld),
    };
}

} // namespace mc::world

#pragma once

#include "world/Block.hpp"
#include "world/Dimension.hpp"
#include "world/WorldConstants.hpp"

namespace mc::world::gen {

// Java 1.16.1's NoiseGeneratorSettings / NoiseSettings, the *data* that turns the
// one NoiseChunkGenerator algorithm into a per-dimension generator without a
// vtable. The overworld, the nether and the end differ only in the values below
// (default block/fluid, sea level, the shape terms, the top/bottom slides that
// cap the sky and floor); the algorithm that reads them is shared.
//
// This is WG-1's whole point: `NoiseChunkGenerator` used to bake the overworld's
// constants into the code, so it could only make the overworld. Now it reads a
// `NoiseGeneratorSettings`, and `overworld()` returns exactly the constants it
// used to hardcode — the overworld terrain is bit-for-bit what it was (the
// world-generation regression asserts this). WG-2/3 supply the nether/end value
// sets; WG-1 does not (it ships only the overworld one, plus a couple of test
// fixtures the acceptance uses to prove the wiring reads settings).

// GenerationShapeConfig.SlideConfig: the linear ramp that pulls the last few
// noise rows toward a fixed target so terrain neither pokes through the sky nor
// floats over the floor. `size == 0` disables the slide (the overworld's bottom
// slide), so the generator must guard on it.
struct NoiseSlide final {
    double target = 0.0;
    int size = 0;
    int offset = 0;
};

// NoiseSettings#noiseSizeVertical / #getMinY etc., plus the sampling scales the
// generator multiplies into every octave. All doubles are the exact overworld
// literals so overworld() reproduces the pre-WG-1 field byte-for-byte.
struct NoiseSamplingSettings final {
    // NoiseChunkGenerator's fixed 684.412 base scale times the sampling config's
    // xz/y scale (overworld: both 0.9999999814507745).
    double xzScale = 684.412 * 0.9999999814507745;
    double yScale = 684.412 * 0.9999999814507745;
    // The interpolation-noise strides: xzScale/80 and yScale/160 in vanilla.
    double xzFactor = (684.412 * 0.9999999814507745) / 80.0;
    double yFactor = (684.412 * 0.9999999814507745) / 160.0;
};

struct NoiseGeneratorSettings final {
    // The vertical span the terrain sits in, read from the DimensionType so the
    // build limit is never a 256 literal. minY is the lowest buildable row;
    // height is the number of rows. Overworld: -64 / 384.
    int minY = kMinY;
    int height = kWorldHeight;
    // NoiseSettings#seaLevel: the row at and below which a non-solid cell becomes
    // the default fluid instead of air. Overworld: 63.
    int seaLevel = kSeaLevel;

    // ChunkGeneratorSettings#getDefaultBlock / #getDefaultFluid: what a positive
    // density becomes, and what fills the water/lava column below sea level.
    // Overworld stone/water; the nether netherrack/lava, the end end_stone/air.
    Block defaultBlock = Block::Stone;
    Block defaultFluid = Block::Water;

    NoiseSamplingSettings sampling{};

    // NoiseSettings#densityFactor / #densityOffset: the linear height bias in the
    // shape term. Overworld: 1.0 / -0.46875.
    double densityFactor = 1.0;
    double densityOffset = -0.46875;

    // GenerationShapeConfig#getTopSlide / #getBottomSlide.
    NoiseSlide topSlide{-10.0, 3, 0};
    NoiseSlide bottomSlide{-30.0, 0, 0};

    // AbstractBlock-level fill for the depth below the historical noise lattice
    // (the overworld's -64..0 extra rows are solid default block so a fall never
    // drops into void). False for a dimension whose floor is the noise lattice
    // itself (the nether/end sit at minY 0). Kept a flag rather than derived from
    // minY so a dimension can opt out explicitly.
    bool fillBelowLatticeFloor = true;

    // The overworld value set: exactly the constants NoiseChunkGenerator used to
    // hardcode. Kept in one place so the regression can diff generated overworld
    // terrain against the pre-WG-1 output.
    [[nodiscard]] static constexpr NoiseGeneratorSettings overworld() {
        const DimensionType& type = dimensionType(DimensionId::Overworld);
        NoiseGeneratorSettings settings;
        settings.minY = type.minY;
        settings.height = type.height;
        settings.seaLevel = kSeaLevel;
        settings.defaultBlock = Block::Stone;
        settings.defaultFluid = Block::Water;
        settings.densityFactor = 1.0;
        settings.densityOffset = -0.46875;
        settings.topSlide = NoiseSlide{-10.0, 3, 0};
        settings.bottomSlide = NoiseSlide{-30.0, 0, 0};
        settings.fillBelowLatticeFloor = true;
        return settings;
    }
};

} // namespace mc::world::gen

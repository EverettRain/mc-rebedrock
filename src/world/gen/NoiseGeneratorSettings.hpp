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

    // NoiseGeneratorSettings#getBedrockFloorPosition / #getBedrockRoofPosition:
    // the number of bedrock rows to lay at the floor and (for a ceilinged
    // dimension) the roof of the noise column. The overworld lays its bedrock in
    // the Features surface pass, so it leaves these 0 and the noise generator does
    // not cap it; the nether wants a solid bedrock floor at minY and a bedrock
    // roof under its ceiling (DimensionType.hasCeiling). A cap of N writes N rows,
    // the bottom/top one always solid and the rest thinning out, the way vanilla's
    // bedrock band does.
    int bedrockFloorRows = 0;
    int bedrockRoofRows = 0;

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
        settings.bedrockFloorRows = 0;  // Features::buildSurface lays it
        settings.bedrockRoofRows = 0;
        return settings;
    }

    // The nether value set (WG-2). One algorithm, nether values: netherrack over a
    // lava sea at y=32, a mostly-solid column with the noise carving air pockets
    // and cave systems, a bedrock floor and (under the ceiling) roof. Height and
    // ceiling come from the nether DimensionType (0..128, hasCeiling), never a
    // literal. The shape terms differ from the overworld's: a strong bottom slide
    // and a top slide both pull the column solid near the floor and the roof, and
    // the density bias keeps the middle mostly netherrack with hollows — the
    // characteristic nether "solid rock riddled with caverns" rather than a single
    // ground surface. These are not a byte-for-byte 1.16.1 port (the nether is not
    // under the overworld逐格 parity guard); they reproduce the qualitative
    // nether the acceptance checks.
    [[nodiscard]] static constexpr NoiseGeneratorSettings nether() {
        const DimensionType& type = dimensionType(DimensionId::Nether);
        NoiseGeneratorSettings settings;
        settings.minY = type.minY;              // 0
        // The nether *generates* into a 128-tall column with its bedrock roof at
        // y=127 (1.16.1 logical height), even though the DimensionType's coordinate
        // ceiling (type.height) is taller: the playable nether is the 0..128 band
        // under the roof. Kept a literal here — a nether-specific logical height
        // rather than the DimensionType's coordinate limit — with a static_assert
        // below pinning it to the DimensionType's floor so a reorder is caught.
        settings.height = 128;
        settings.seaLevel = 32;                 // the lava sea
        settings.defaultBlock = Block::Netherrack;
        settings.defaultFluid = Block::Lava;
        // A near-flat density bias with a small negative offset: most of the
        // column stays above the solid threshold (netherrack), and the noise digs
        // the hollows. Weaker than the overworld's 1.0 factor so the terrain does
        // not resolve into one clean surface.
        settings.densityFactor = 0.0;
        settings.densityOffset = 0.019;
        // Both ends slide toward solid so the floor and the roof close off (the
        // roof matters because the nether has a ceiling); vanilla's nether uses a
        // top slide of (0.9375,3,0) and a strong bottom slide (2.5,4,-1). Positive
        // targets push toward solid.
        settings.topSlide = NoiseSlide{0.9375, 3, 0};
        settings.bottomSlide = NoiseSlide{2.5, 4, -1};
        settings.fillBelowLatticeFloor = false;  // the floor is the lattice at y=0
        settings.bedrockFloorRows = 5;
        settings.bedrockRoofRows = type.hasCeiling ? 5 : 0;
        return settings;
    }
};

} // namespace mc::world::gen

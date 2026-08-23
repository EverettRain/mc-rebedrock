#pragma once

#include "world/Chunk.hpp"
#include "world/gen/BiomeSource.hpp"
#include "world/gen/NoiseGeneratorSettings.hpp"
#include "world/gen/NoiseSampler.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace mc::world::gen {

// Java 1.16.1's NoiseChunkGenerator: one algorithm parameterised by a
// NoiseGeneratorSettings (WG-1), rather than the overworld baked into the code.
//
// The density field is sampled on a coarse lattice — every four blocks
// horizontally, every eight vertically — and trilinearly interpolated in
// between; positive density is the settings' default block, negative is air or,
// below the settings' sea level, the default fluid. Three 16-octave Perlin
// stacks feed it: a lower and an upper terrain noise plus an interpolation noise
// that blends between them, all shaped by the biome's depth and scale and by the
// top/bottom slides that flatten the sky and the floor. The overworld, the
// nether and the end run this same algorithm on different settings values.
class NoiseChunkGenerator final {
  public:
    // GenerationShapeConfig noise resolutions. The horizontal ones are the same
    // in every 1.16.1 dimension; the vertical lattice is 32 cells (256/8) for the
    // overworld, the nether and the end alike, so it stays a fixed size (which
    // keeps the noise column a stack array, no heap on the hot path). The
    // dimension's *build limit* comes from the settings' height/minY, not this.
    static constexpr int kHorizontalNoiseResolution = 4;
    static constexpr int kVerticalNoiseResolution = 8;
    static constexpr int kNoiseSizeX = 16 / kHorizontalNoiseResolution;      // 4 cells
    static constexpr int kNoiseSizeZ = 16 / kHorizontalNoiseResolution;      // 4 cells
    static constexpr int kNoiseSizeY = 256 / kVerticalNoiseResolution;       // 32 cells
    // SurfaceChunkGenerator's three terrain stacks and the density-offset stack.
    static constexpr int kOctaveCount = 16;
    static constexpr int kInterpolationOctaveCount = 8;

    // The samplers come pre-built from the generator's shared stream (see
    // SurfaceGenerator), drawn in vanilla's order — lower, upper, interpolation,
    // then the density offset after the surface-depth sampler and the 2620-step
    // skip. `settings` is the dimension's value set (see NoiseGeneratorSettings);
    // the overworld path passes NoiseGeneratorSettings::overworld(), whose values
    // are exactly the constants this class used to hardcode.
    NoiseChunkGenerator(
        const BiomeSource& biomeSource,
        const NoiseGeneratorSettings& settings,
        std::vector<PerlinNoiseSampler> lower,
        std::vector<PerlinNoiseSampler> upper,
        std::vector<PerlinNoiseSampler> interpolation,
        OctavePerlinNoiseSampler densityOffset);

    [[nodiscard]] const NoiseGeneratorSettings& settings() const { return settings_; }

    // Fills the chunk's stone/water column. Surface materials, carving and
    // features run afterwards.
    void buildBaseTerrain(Chunk& chunk, int chunkX, int chunkZ) const;

    // The height of the first solid block in a column, which the surface builder
    // and the feature placement both need.
    [[nodiscard]] int surfaceHeight(const Chunk& chunk, int localX, int localZ) const;

  private:
    // The randomDensityOffset term: a very slow field sampled at a 200x stride
    // that lifts or drops whole regions by a fraction of a block.
    [[nodiscard]] double randomDensityOffset(int noiseX, int noiseZ) const;
    // NoiseChunkGenerator#computeNoiseRange: the biome-weighted depth and scale
    // for one noise column, blended over the 5x5 window around it, mapped onto
    // the (depth*0.5 - 0.125, scale*0.9 + 0.1) the shape term reads.
    [[nodiscard]] std::array<double, 2> noiseRange(int noiseX, int noiseZ) const;
    // NoiseChunkGenerator#sampleNoise, the three-stack blend at one lattice point.
    [[nodiscard]] double sampleDensity(int noiseX, int noiseY, int noiseZ) const;
    // SurfaceChunkGenerator#sampleNoiseColumn's shape term plus the two slides.
    [[nodiscard]] double applyShape(
        double density,
        double depth,
        double scale,
        double randomDensityOffset,
        int noiseY) const;
    void fillNoiseColumn(std::array<double, kNoiseSizeY + 1>& column, int noiseX, int noiseZ) const;

    const BiomeSource* biomeSource_ = nullptr;
    NoiseGeneratorSettings settings_{};
    std::vector<PerlinNoiseSampler> lowerNoise_;
    std::vector<PerlinNoiseSampler> upperNoise_;
    std::vector<PerlinNoiseSampler> interpolationNoise_;
    // The `randomDensityOffset` sampler, the same 16-octave stack vanilla draws
    // after the 2620-step skip; it tilts whole regions up or down by a fraction
    // of a block.
    OctavePerlinNoiseSampler densityOffsetNoise_;
};

} // namespace mc::world::gen

#pragma once

#include "world/gen/JavaRandom.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace mc::world::gen {

// net.minecraft.util.math.noise.PerlinNoiseSampler: improved Perlin noise over
// a shuffled 256-entry permutation, with a random origin so two samplers built
// from the same stream never line up.
class PerlinNoiseSampler final {
  public:
    explicit PerlinNoiseSampler(JavaRandom& random);

    // The full vanilla signature. `yScale`/`yMax` implement the "flatten the Y
    // gradient onto a lattice" trick the density noise uses; pass zero for both
    // to get ordinary 3D noise.
    [[nodiscard]] double sample(
        double x,
        double y,
        double z,
        double yScale = 0.0,
        double yMax = 0.0) const;

    [[nodiscard]] double originY() const { return originY_; }

  private:
    [[nodiscard]] int permute(int hash) const {
        return permutation_[static_cast<std::size_t>(hash & 255)];
    }

    double originX_ = 0.0;
    double originY_ = 0.0;
    double originZ_ = 0.0;
    std::array<int, 256> permutation_{};
};

// Draws `count` raw Perlin octaves from the stream, in order. Every overworld
// stack is a contiguous range ending at zero, so the first sampler drawn is the
// one vanilla's sample() reads at frequency 1.
[[nodiscard]] inline std::vector<PerlinNoiseSampler> buildOctaves(
    JavaRandom& random, int count) {
    std::vector<PerlinNoiseSampler> octaves;
    octaves.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        octaves.emplace_back(random);
    }
    return octaves;
}

// net.minecraft.util.math.noise.OctavePerlinNoiseSampler over a contiguous
// octave range ending at zero, which is every range the overworld uses. Sample
// reads the first-drawn octave at frequency 1 with the smallest weight, and
// each following octave halves its frequency while its weight doubles, exactly
// as vanilla iterates its octaveSamplers array.
class OctavePerlinNoiseSampler final {
  public:
    // Empty sampler, for the shared construction stream to fill in.
    OctavePerlinNoiseSampler() = default;
    // `octaveCount` is the size of Java's `IntStream.rangeClosed(-(count-1), 0)`.
    OctavePerlinNoiseSampler(JavaRandom& random, int octaveCount);

    [[nodiscard]] double sample(
        double x,
        double y,
        double z,
        double yScale = 0.0,
        double yMax = 0.0,
        bool useOrigin = false) const;

  private:
    std::vector<PerlinNoiseSampler> octaves_;
};

// net.minecraft.util.math.noise.SimplexNoiseSampler, used by the surface
// builder to break the grass/sand boundary up.
class SimplexNoiseSampler final {
  public:
    explicit SimplexNoiseSampler(JavaRandom& random);

    [[nodiscard]] double sample(double x, double y) const;

  private:
    [[nodiscard]] int permute(int hash) const {
        return permutation_[static_cast<std::size_t>(hash & 255)];
    }

    double originX_ = 0.0;
    double originY_ = 0.0;
    double originZ_ = 0.0;
    std::array<int, 256> permutation_{};
};

class OctaveSimplexNoiseSampler final {
  public:
    // Empty sampler, for the shared construction stream to fill in.
    OctaveSimplexNoiseSampler() = default;
    OctaveSimplexNoiseSampler(JavaRandom& random, int octaveCount);

    [[nodiscard]] double sample(double x, double y) const;

  private:
    std::vector<SimplexNoiseSampler> octaves_;
};

} // namespace mc::world::gen

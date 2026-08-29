#pragma once

#include "world/gen/Biome.hpp"
#include "world/gen/NoiseSampler.hpp"

#include <array>
#include <cstdint>

namespace mc::world::gen {

// vanilla's MultiNoiseBiomeSource, the nether's biome map. Rather than the
// overworld's GenLayer zoom stack, the nether places its five biomes by climate:
// four low-octave noise fields (temperature, humidity, altitude, weirdness) are
// sampled at each quart column, and the biome whose parameter point sits nearest
// (squared distance in the 4-space, plus its offset) wins. Ported in structure
// from vanilla; the biome parameter points are the vanilla nether values.
//
// A concrete value type, not a vtable: BiomeSource owns one of these or a
// LayeredBiomeSource and dispatches on a stored tag, so the noise column reads a
// biome through one branch, never a virtual call.
class MultiNoiseBiomeSource final {
  public:
    explicit MultiNoiseBiomeSource(std::uint64_t seed);

    // BiomeSource#getBiomeForNoiseGen: the biome at a quart (1:4) column.
    [[nodiscard]] Biome sample(int quartX, int quartZ) const;

    // One biome's climate parameter point (Biome.MixedNoisePoint in vanilla). The
    // fifth axis, offset, is a fixed bias added to the distance so a biome can be
    // rarer without moving its centre.
    struct NoisePoint final {
        Biome biome = Biome::NetherWastes;
        float temperature = 0.0F;
        float humidity = 0.0F;
        float altitude = 0.0F;
        float weirdness = 0.0F;
        float offset = 0.0F;
    };

  private:
    // The four climate noises, each an octave Perlin stack seeded off the world
    // seed so the nether's biome map is deterministic per seed and independent of
    // the overworld's.
    OctavePerlinNoiseSampler temperature_;
    OctavePerlinNoiseSampler humidity_;
    OctavePerlinNoiseSampler altitude_;
    OctavePerlinNoiseSampler weirdness_;

    static constexpr std::size_t kBiomePointCount = 5;
    std::array<NoisePoint, kBiomePointCount> points_{};
};

} // namespace mc::world::gen

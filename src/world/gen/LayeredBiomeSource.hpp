#pragma once

#include "world/gen/Biome.hpp"

#include <cstdint>
#include <memory>

namespace mc::world::gen {

// Java 1.16.1's VanillaLayeredBiomeSource: the ~30-pass GenLayer zoom pipeline
// (ContinentLayer -> ScaleLayer zooms -> climate / edge / river layers) that
// builds the overworld biome map at 1:4 (quart) resolution. Ported from the
// 1.16.1 Yarn sources; the biome set is reduced to the ones Biome.hpp
// registers, with the hills and temperature variants collapsed onto their base
// biome and the ocean-temperature variants onto Ocean/DeepOcean.
class LayeredBiomeSource final {
  public:
    explicit LayeredBiomeSource(std::uint64_t seed);

    ~LayeredBiomeSource();
    LayeredBiomeSource(const LayeredBiomeSource&) = delete;
    LayeredBiomeSource& operator=(const LayeredBiomeSource&) = delete;

    // The biome at a quart (1:4) grid position, matching
    // BiomeSource#getBiomeForNoiseGen.
    [[nodiscard]] Biome sample(int quartX, int quartZ) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mc::world::gen

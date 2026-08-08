#pragma once

#include "world/gen/Biome.hpp"

#include <cstdint>
#include <memory>

namespace mc::world::gen {

// Which biome sits at a given point.
//
// Java 1.16.1 answers this with VanillaLayeredBiomeSource: a stack of ~30
// GenLayer passes that zoom a 1:256 continent grid up to a 1:4 biome grid.
// That pipeline is ported in LayeredBiomeSource, so the biome map is now
// vanilla's structure rather than the earlier climate-noise stand-in.
class BiomeSource final {
  public:
    explicit BiomeSource(std::uint64_t seed);
    ~BiomeSource();

    BiomeSource(const BiomeSource&) = delete;
    BiomeSource& operator=(const BiomeSource&) = delete;

    // BiomeSource#getBiomeForNoiseGen: coordinates are in quart (1:4) space,
    // which is what the noise column and the surface builder both work in.
    [[nodiscard]] Biome biomeForNoiseGeneration(int quartX, int quartZ) const;

    // The biome at a block position, for the surface builder and the features.
    [[nodiscard]] Biome biomeAtBlock(int blockX, int blockZ) const {
        return biomeForNoiseGeneration(blockX >> 2, blockZ >> 2);
    }

  private:
    std::unique_ptr<class LayeredBiomeSource> layered_;
};

} // namespace mc::world::gen

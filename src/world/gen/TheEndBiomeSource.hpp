#pragma once

#include "world/gen/Biome.hpp"
#include "world/gen/NoiseSampler.hpp"

#include <cstdint>

namespace mc::world::gen {

// vanilla's TheEndBiomeSource, the end's biome map. Not a climate or a
// GenLayer map: the biome at a column is decided by distance to the world origin
// and the island-height field. The central 64-chunk disc is TheEnd (the main
// island's biome); outside it, the same island height the terrain uses picks
// EndHighlands / EndMidlands / SmallEndIslands / EndBarrens by threshold.
//
// A concrete value type owned by BiomeSource (kind tag TheEnd), like
// MultiNoiseBiomeSource — one branch, not a vtable.
class TheEndBiomeSource final {
  public:
    explicit TheEndBiomeSource(std::uint64_t seed);

    // BiomeSource#getBiomeForNoiseGen: the biome at a quart (1:4) column.
    [[nodiscard]] Biome sample(int quartX, int quartZ) const;

  private:
    // The island-height field the terrain shares; the biome thresholds read it.
    SimplexNoiseSampler islandNoise_;
};

} // namespace mc::world::gen

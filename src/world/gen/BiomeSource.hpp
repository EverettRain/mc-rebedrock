#pragma once

#include "world/gen/Biome.hpp"

#include <cstdint>
#include <memory>

namespace mc::world::gen {

// Which biome sits at a given point — the shared front over the per-dimension
// biome maps (WG-DESIGN §2: "生物群系源按维度…共享 BiomeSource 接口").
//
// The overworld answers with VanillaLayeredBiomeSource, a stack of ~30 GenLayer
// passes zooming a 1:256 continent grid up to a 1:4 biome grid (ported in
// LayeredBiomeSource). The nether answers with MultiNoiseBiomeSource, four
// climate-noise fields selecting one of five biomes by nearest parameter point.
// BiomeSource owns whichever one the dimension needs and dispatches on a stored
// kind tag — a branch, not a virtual call, so the noise column stays off the
// vtable (the DOD stance the rest of worldgen keeps).
class BiomeSource final {
  public:
    // The overworld source (the layered GenLayer map), for source compatibility
    // with every existing caller.
    explicit BiomeSource(std::uint64_t seed);
    ~BiomeSource();

    BiomeSource(const BiomeSource&) = delete;
    BiomeSource& operator=(const BiomeSource&) = delete;
    // Movable so the nether factory can return one by value (the owned impls move
    // with their unique_ptrs); defined out of line where the impls are complete.
    BiomeSource(BiomeSource&&) noexcept;
    BiomeSource& operator=(BiomeSource&&) noexcept;

    // The nether source (MultiNoiseBiomeSource). `seed` is already the nether's
    // derived seed (dimensionSeed(worldSeed, Nether)).
    [[nodiscard]] static BiomeSource nether(std::uint64_t seed);

    // BiomeSource#getBiomeForNoiseGen: coordinates are in quart (1:4) space,
    // which is what the noise column and the surface builder both work in.
    [[nodiscard]] Biome biomeForNoiseGeneration(int quartX, int quartZ) const;

    // The biome at a block position, for the surface builder and the features.
    [[nodiscard]] Biome biomeAtBlock(int blockX, int blockZ) const {
        return biomeForNoiseGeneration(blockX >> 2, blockZ >> 2);
    }

  private:
    enum class Kind : std::uint8_t { Layered, MultiNoise };

    // The nether factory constructs this variant directly; the public ctor stays
    // the layered overworld one.
    struct NetherTag {};
    BiomeSource(NetherTag, std::uint64_t seed);

    Kind kind_ = Kind::Layered;
    // Exactly one is populated, per kind_. Held by pointer so the two source types
    // (both with their own heavy impl) do not bloat every BiomeSource, and so the
    // header need not pull their definitions in.
    std::unique_ptr<class LayeredBiomeSource> layered_;
    std::unique_ptr<class MultiNoiseBiomeSource> multiNoise_;
};

} // namespace mc::world::gen

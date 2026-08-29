#pragma once

#include "world/Chunk.hpp"
#include "world/gen/BiomeSource.hpp"
#include "world/gen/EndIslandHeight.hpp"
#include "world/gen/GenerationSamplers.hpp"
#include "world/gen/NoiseGeneratorSettings.hpp"
#include "world/gen/NoiseSampler.hpp"

#include <cstdint>

namespace mc::world {

// WG-3: the end's chunk generator. The end is not shaped like the overworld or
// the nether — there is no single ground surface or solid column, only islands of
// end_stone floating in the void. So its terrain is the vanilla island height field
// (EndIslandHeight): a tall central plateau around (0,0), a void ring, and outer
// islands seeded by a coarse noise, each a body of end_stone centred near y=64
// with a 3D noise roughening its shell. A non-solid cell is the void (air), never
// water — the end has no sea.
//
// Parallel to SurfaceGenerator/NetherGenerator (no switch(dimension)): it shares
// the sampler bundle and the shared BiomeSource front (kind TheEnd), and derives
// its own seed via dimensionSeed(worldSeed, End) so the end never mirrors the
// overworld or the nether. WG-4 wires it behind the DimensionGenerator seam; WG-3
// delivers it standalone and headless-verifiable.
class EndGenerator final {
  public:
    explicit EndGenerator(std::uint64_t worldSeed);

    // Generates one end chunk: the island bodies over the void, then the end_stone
    // surface skin (there is no dirt/grass pass — the end is bare end_stone).
    [[nodiscard]] Chunk generate(int chunkX, int chunkZ) const;

    [[nodiscard]] gen::Biome biomeAt(int worldX, int worldZ) const {
        return biomeSource_.biomeAtBlock(worldX, worldZ);
    }

    [[nodiscard]] const gen::NoiseGeneratorSettings& settings() const { return settings_; }

    // The island height field at a block column, exposed so a test (and the biome
    // source) can agree with the terrain on where an island is.
    [[nodiscard]] float islandHeightAt(int blockX, int blockZ) const;

  private:
    std::uint64_t endSeed_ = 0U;
    gen::NoiseGeneratorSettings settings_;
    gen::BiomeSource biomeSource_;
    gen::GenerationSamplers samplers_;
    // The island-height field (shared with TheEndBiomeSource) and a 3D noise that
    // roughens each island's shell so it is not a flat slab.
    gen::SimplexNoiseSampler islandNoise_;
    gen::OctavePerlinNoiseSampler shapeNoise_;
};

} // namespace mc::world

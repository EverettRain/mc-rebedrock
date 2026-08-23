#pragma once

#include "world/Chunk.hpp"
#include "world/gen/BiomeSource.hpp"
#include "world/gen/GenerationSamplers.hpp"
#include "world/gen/JavaRandom.hpp"
#include "world/gen/NoiseChunkGenerator.hpp"
#include "world/gen/NoiseGeneratorSettings.hpp"
#include "world/gen/NoiseSampler.hpp"

#include <cstdint>

namespace mc::world {

// WG-2: the nether's chunk generator. Not a new algorithm — it is the WG-1
// NoiseChunkGenerator run on NoiseGeneratorSettings::nether() (netherrack over a
// lava sea at y=32, the noise carving the caverns) with a MultiNoiseBiomeSource,
// plus the nether-specific passes the overworld's Features does not do: a bedrock
// floor and roof cap, a biome-driven surface (soul sand in the valleys, basalt in
// the deltas) and the nether decorations the acceptance checks (glowstone
// clusters, quartz ore).
//
// Parallel to SurfaceGenerator rather than folded into it: the two share the noise
// generator and the sampler draw, but their surface/feature pipelines are
// different content, and a switch(dimension) inside one generate() would be the
// coupling WG keeps out. WG-4 wires this behind the DimensionGenerator seam; WG-2
// delivers it standalone and headless-verifiable.
class NetherGenerator final {
  public:
    // `worldSeed` is the world's seed; the generator derives the nether's own
    // stream (dimensionSeed(worldSeed, Nether)) so the nether never mirrors the
    // overworld and two worlds differ.
    explicit NetherGenerator(std::uint64_t worldSeed);

    // Generates one nether chunk: base terrain, bedrock cap, surface, features.
    [[nodiscard]] Chunk generate(int chunkX, int chunkZ) const;

    [[nodiscard]] gen::Biome biomeAt(int worldX, int worldZ) const {
        return biomeSource_.biomeAtBlock(worldX, worldZ);
    }

    [[nodiscard]] const gen::NoiseGeneratorSettings& settings() const { return settings_; }

  private:
    // The bedrock floor/roof cap the nether settings ask for (a ragged band, like
    // the overworld's floor), keyed off the chunk seed for determinism.
    void buildBedrockCap(Chunk& chunk, gen::JavaRandom& random) const;
    // The biome-driven top layer: soul sand/soil in the valley, basalt/blackstone
    // in the deltas, netherrack elsewhere. Runs the first solid rows below any
    // air/lava gap.
    void buildSurface(Chunk& chunk, int chunkX, int chunkZ) const;
    // Glowstone clusters hung near the roof and quartz-ore veins in the rock.
    void generateFeatures(Chunk& chunk, int chunkX, int chunkZ) const;

    std::uint64_t netherSeed_ = 0U;
    gen::NoiseGeneratorSettings settings_;
    gen::BiomeSource biomeSource_;
    gen::GenerationSamplers samplers_;  // reuse SurfaceGenerator's sampler bundle
    gen::NoiseChunkGenerator noiseGenerator_;
};

} // namespace mc::world

#pragma once

#include "world/Chunk.hpp"
#include "world/Dimension.hpp"
#include "world/EndGenerator.hpp"
#include "world/NetherGenerator.hpp"
#include "world/StructureManager.hpp"
#include "world/SurfaceGenerator.hpp"
#include "world/gen/StructureGenerator.hpp"
#include "world/gen/TreeGrower.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace mc::world {

// WG-4: the per-dimension generator binding the DimensionGenerator seam promised.
//
// dimensionGeneratorConfig(id).hasTerrainGenerator says "a real generator exists
// for this dimension"; this is that generator, chosen by DimensionId. It owns the
// overworld SurfaceGenerator, the nether NetherGenerator or the end EndGenerator
// (WG-1/2/3) and forwards generate() to it — one uniform entry the chunk streamer
// calls without a switch(dimension) of its own. No vtable: exactly one sub-
// generator is constructed per dimension and held by value in an optional, and
// generate() branches on the stored id (the DOD stance the rest of worldgen
// keeps). The nether/end generators derive their own stream from the world seed
// (dimensionSeed), so this takes the *world* seed for every dimension.
class DimensionChunkGenerator final {
  public:
    DimensionChunkGenerator(DimensionId dimension, std::uint64_t worldSeed)
        : dimension_(dimension), worldSeed_(worldSeed) {
        switch (dimension) {
        case DimensionId::Overworld:
            overworld_.emplace(worldSeed);
            break;
        case DimensionId::Nether:
            nether_.emplace(worldSeed);
            break;
        case DimensionId::End:
            end_.emplace(worldSeed);
            break;
        case DimensionId::Count:
            break;  // never a real dimension; leaves every generator empty
        }
    }

    [[nodiscard]] DimensionId dimension() const { return dimension_; }

    // Generates one chunk of this dimension. `borderBlocks` receives the crown
    // blocks a tree grew past the chunk border (overworld only — the nether/end
    // have no cross-chunk vegetation, so they leave it empty), the way the
    // overworld streamer already threads them to neighbours.
    [[nodiscard]] Chunk generate(int chunkX, int chunkZ,
                                 std::vector<gen::TreeBorderBlock>& borderBlocks) const {
        switch (dimension_) {
        case DimensionId::Overworld: {
            Chunk chunk = overworld_->generate(chunkX, chunkZ, borderBlocks);
            // STRUCT-2: stamp structures whose origin is this chunk. A no-op until
            // structureManager() carries templates + sets, so an install with no
            // structure content generates exactly as before.
            gen::placeStructures(
                chunk, chunkX, chunkZ, worldSeed_, structureManager(),
                [this](int worldX, int worldZ) { return overworld_->biomeAt(worldX, worldZ); },
                [this](int worldX, int worldZ) { return overworld_->terrainHeightAt(worldX, worldZ); },
                borderBlocks);
            return chunk;
        }
        case DimensionId::Nether:
            return nether_->generate(chunkX, chunkZ);
        case DimensionId::End:
            return end_->generate(chunkX, chunkZ);
        case DimensionId::Count:
            break;
        }
        return Chunk{};  // an unbound dimension generates nothing (all air)
    }

    // Convenience overload for callers with no neighbour to finish a crown in.
    [[nodiscard]] Chunk generate(int chunkX, int chunkZ) const {
        std::vector<gen::TreeBorderBlock> ignored;
        return generate(chunkX, chunkZ, ignored);
    }

  private:
    DimensionId dimension_ = DimensionId::Overworld;
    std::uint64_t worldSeed_ = 0U;
    // Exactly one is engaged, per dimension_. Held by value in an optional so the
    // hot generate() path dereferences a member, not a heap pointer, and no vtable
    // is consulted.
    std::optional<SurfaceGenerator> overworld_;
    std::optional<NetherGenerator> nether_;
    std::optional<EndGenerator> end_;
};

} // namespace mc::world

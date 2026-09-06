#pragma once

#include "world/Block.hpp"
#include "world/ChunkSection.hpp"
#include "world/WorldConstants.hpp"
#include "world/gen/Biome.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace mc::world {

class Chunk final {
  public:
    Chunk();

    [[nodiscard]] Block block(int x, int y, int z) const;
    void setBlock(int x, int y, int z, Block value);
    // The whole interned state in one read/write. block()/orientation()/
    // fluidLevel() each decode one axis and drop the rest, so a caller that
    // needs every axis — LIT above all, which no per-axis accessor carries —
    // must go through these, not through a decompose/recompose round trip.
    [[nodiscard]] BlockState state(int x, int y, int z) const;
    void setState(int x, int y, int z, BlockState value);
    [[nodiscard]] BlockOrientation orientation(int x, int y, int z) const;
    void setOrientation(int x, int y, int z, BlockOrientation value);
    [[nodiscard]] std::uint8_t fluidLevel(int x, int y, int z) const;
    void setFluidLevel(int x, int y, int z, std::uint8_t value);
    [[nodiscard]] std::uint8_t skyLight(int x, int y, int z) const;
    [[nodiscard]] std::uint8_t blockLight(int x, int y, int z) const;
    bool setSkyLight(int x, int y, int z, std::uint8_t value);
    bool setBlockLight(int x, int y, int z, std::uint8_t value);

    // The lowest Y in each column that is still a sky source — 26.1's
    // `ChunkSkyLightSources` heightmap, one int per column rather than the
    // per-cell nibble array this replaced. Everything at or above it is sky 15
    // by definition; everything below it can only be reached by propagation.
    // A fresh chunk reads kMaxY (no sources at all) so a world assembled without
    // running the light engine stays dark, exactly as the zero-initialised
    // nibble array did; the light engine fills it in initializeChunks.
    [[nodiscard]] int lowestSourceY(int localX, int localZ) const;
    void setLowestSourceY(int localX, int localZ, int y);

    // The biome that generated each column, filled by the surface pass. The
    // mesher reads it to tint grass-family blocks the way vanilla's BiomeColors
    // does; biomes never change after generation.
    [[nodiscard]] gen::Biome columnBiome(int localX, int localZ) const;
    void setColumnBiome(int localX, int localZ, gen::Biome biome);

    [[nodiscard]] const ChunkSection& section(int sectionY) const;
    [[nodiscard]] ChunkSection& section(int sectionY);

    // The resident bytes this chunk's data holds: the fixed struct plus every
    // section's palette/light buffers. Used by the M-Chunk side-split memory
    // budget (the server world and the client cache each measure their share).
    [[nodiscard]] std::size_t residentBytes() const;

  private:
    std::array<ChunkSection, kSectionCount> sections_{};
    std::array<gen::Biome, kChunkWidth * kChunkDepth> columnBiomes_{};
    std::array<std::int16_t, kChunkWidth * kChunkDepth> lowestSourceY_{};
};

} // namespace mc::world

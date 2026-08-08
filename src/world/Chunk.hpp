#pragma once

#include "world/Block.hpp"
#include "world/ChunkSection.hpp"
#include "world/WorldConstants.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace mc::world {

class Chunk final {
  public:
    Chunk();

    [[nodiscard]] Block block(int x, int y, int z) const;
    void setBlock(int x, int y, int z, Block value);
    [[nodiscard]] BlockOrientation orientation(int x, int y, int z) const;
    void setOrientation(int x, int y, int z, BlockOrientation value);
    [[nodiscard]] std::uint8_t fluidLevel(int x, int y, int z) const;
    void setFluidLevel(int x, int y, int z, std::uint8_t value);
    [[nodiscard]] std::uint8_t skyLight(int x, int y, int z) const;
    [[nodiscard]] std::uint8_t blockLight(int x, int y, int z) const;
    [[nodiscard]] std::uint8_t directSkyLight(int x, int y, int z) const;
    bool setSkyLight(int x, int y, int z, std::uint8_t value);
    bool setBlockLight(int x, int y, int z, std::uint8_t value);
    bool setDirectSkyLight(int x, int y, int z, std::uint8_t value);

    [[nodiscard]] const ChunkSection& section(int sectionY) const;
    [[nodiscard]] ChunkSection& section(int sectionY);

  private:
    std::array<ChunkSection, kSectionCount> sections_{};
};

} // namespace mc::world

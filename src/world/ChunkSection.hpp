#pragma once

#include "world/Block.hpp"
#include "world/NibbleArray.hpp"
#include "world/WorldConstants.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc::world {

class ChunkSection final {
  public:
    ChunkSection();

    [[nodiscard]] Block block(int x, int y, int z) const;
    void setBlock(int x, int y, int z, Block value);
    [[nodiscard]] BlockOrientation orientation(int x, int y, int z) const;
    void setOrientation(int x, int y, int z, BlockOrientation value);
    [[nodiscard]] std::uint8_t fluidLevel(int x, int y, int z) const;
    void setFluidLevel(int x, int y, int z, std::uint8_t value);
    [[nodiscard]] bool empty() const { return nonAirBlockCount_ == 0U; }
    [[nodiscard]] std::uint8_t skyLight(int x, int y, int z) const;
    [[nodiscard]] std::uint8_t blockLight(int x, int y, int z) const;
    [[nodiscard]] std::uint8_t directSkyLight(int x, int y, int z) const;
    bool setSkyLight(int x, int y, int z, std::uint8_t value);
    bool setBlockLight(int x, int y, int z, std::uint8_t value);
    bool setDirectSkyLight(int x, int y, int z, std::uint8_t value);

  private:
    static constexpr std::size_t kBlockCount =
        static_cast<std::size_t>(kSectionSize) *
        static_cast<std::size_t>(kSectionSize) *
        static_cast<std::size_t>(kSectionSize);

    [[nodiscard]] static std::size_t index(int x, int y, int z);
    // Empty sections dominate a surface-only world.  Keeping these arrays
    // lazy avoids reserving 128 KiB for every Chunk before it contains data.
    std::vector<Block> blocks_;
    std::vector<std::uint8_t> orientations_;
    std::vector<std::uint8_t> fluidLevels_;
    NibbleArray skyLight_;
    NibbleArray blockLight_;
    NibbleArray directSkyLight_;
    std::size_t nonAirBlockCount_ = 0U;
};

} // namespace mc::world

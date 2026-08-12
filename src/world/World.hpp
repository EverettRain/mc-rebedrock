#pragma once

#include "world/BlockState.hpp"
#include "world/Chunk.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace mc::world {

struct ChunkPosition final {
    int x = 0;
    int z = 0;

    [[nodiscard]] bool operator==(const ChunkPosition&) const = default;
};

struct ChunkPositionHash final {
    [[nodiscard]] std::size_t operator()(const ChunkPosition& position) const noexcept;
};

class World final {
  public:
    void setChunk(ChunkPosition position, Chunk chunk);
    bool removeChunk(ChunkPosition position);

    [[nodiscard]] bool hasChunk(ChunkPosition position) const;
    [[nodiscard]] const Chunk* chunk(ChunkPosition position) const;
    [[nodiscard]] Chunk* chunk(ChunkPosition position);
    [[nodiscard]] Block block(int worldX, int y, int worldZ) const;
    bool setBlock(int worldX, int y, int worldZ, Block value);
    // The whole cell as one value. These are the accessors WorldMutationService
    // and the block behaviour callbacks will be written against, so the three
    // separate arrays behind them can be collapsed (T0.4) without touching a
    // caller. They compose and decompose the existing storage for now — the
    // behaviour is identical to reading the three fields by hand.
    [[nodiscard]] BlockState state(int worldX, int y, int worldZ) const;
    bool setState(int worldX, int y, int worldZ, BlockState value);
    [[nodiscard]] BlockOrientation orientation(int worldX, int y, int worldZ) const;
    bool setOrientation(int worldX, int y, int worldZ, BlockOrientation value);
    [[nodiscard]] std::uint8_t fluidLevel(int worldX, int y, int worldZ) const;
    bool setFluidLevel(int worldX, int y, int worldZ, std::uint8_t value);
    [[nodiscard]] std::uint8_t skyLight(int worldX, int y, int worldZ) const;
    [[nodiscard]] std::uint8_t blockLight(int worldX, int y, int worldZ) const;
    [[nodiscard]] std::uint8_t directSkyLight(int worldX, int y, int worldZ) const;
    bool setSkyLight(int worldX, int y, int worldZ, std::uint8_t value);
    bool setBlockLight(int worldX, int y, int worldZ, std::uint8_t value);
    bool setDirectSkyLight(int worldX, int y, int worldZ, std::uint8_t value);
    [[nodiscard]] std::vector<ChunkPosition> positions() const;
    [[nodiscard]] std::size_t chunkCount() const { return chunks_.size(); }
    // The generation biome of the column, used to tint grass-family blocks; an
    // unloaded chunk reads as plains.
    [[nodiscard]] gen::Biome biomeAt(int worldX, int worldZ) const;

  private:
    std::unordered_map<ChunkPosition, Chunk, ChunkPositionHash> chunks_;
};

} // namespace mc::world

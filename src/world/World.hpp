#pragma once

#include "world/BlockState.hpp"
#include "world/Chunk.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
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
    // Installs the same immutable chunk payload in another world. The first
    // write through either World detaches that whole chunk, so the streaming
    // world, simulation world and client cache share static terrain without
    // sharing mutable state.
    void setChunk(ChunkPosition position, std::shared_ptr<const Chunk> chunk);
    bool removeChunk(ChunkPosition position);

    [[nodiscard]] bool hasChunk(ChunkPosition position) const;
    [[nodiscard]] const Chunk* chunk(ChunkPosition position) const;
    [[nodiscard]] Chunk* chunk(ChunkPosition position);
    [[nodiscard]] std::shared_ptr<const Chunk> sharedChunk(ChunkPosition position) const;
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
    // The resident bytes of this world's chunk data (states + light + biomes),
    // including the fixed struct and the map's bucket overhead. M-Chunk uses it
    // to measure the server world and the client cache separately.
    [[nodiscard]] std::size_t residentBytes() const;
    // The resident bytes of only the chunks this World solely owns (the backing
    // shared_ptr's use_count is 1). The server world, client cache and streamer
    // worker world share chunk objects through copy-on-write shared_ptr, so a
    // chunk still shared with another World is physically one copy that
    // residentBytes() counts once per holder (double/triple-counting). This
    // separates the real exclusive cost, so N-Mem can tell how much of the
    // three-world footprint is genuinely duplicated vs still shared.
    [[nodiscard]] std::size_t uniqueResidentBytes() const;

  private:
    std::unordered_map<ChunkPosition, std::shared_ptr<Chunk>, ChunkPositionHash> chunks_;
};

} // namespace mc::world

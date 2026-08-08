#pragma once

#include "world/World.hpp"

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace mc::world {

struct VoxelLightLevel final {
    std::uint8_t sky = 0U;
    std::uint8_t block = 0U;
};

class ChunkLightSampler final {
  public:
    static constexpr int kMaximumLightLevel = 15;

    // Lightweight view over light values maintained by WorldLightEngine.
    explicit ChunkLightSampler(const World& world);

    ChunkLightSampler(const World& world, ChunkPosition chunkPosition,
                      const std::atomic<bool>* cancellation = nullptr);
    ChunkLightSampler(const World& world, std::span<const ChunkPosition> chunkPositions,
                      const std::atomic<bool>* cancellation = nullptr);

    [[nodiscard]] VoxelLightLevel level(int x, int y, int z) const;
    [[nodiscard]] float sky(int x, int y, int z) const;
    [[nodiscard]] float block(int x, int y, int z) const;
    // Block predicates the mesher needs for corner AO, kept behind the same
    // interface as MeshLightingSnapshot so the mesher helpers can be templated
    // over either sampler. For the padded variant the opaque_ array is O(1);
    // the others fall back to a World read (only used on the test/build paths).
    [[nodiscard]] bool isOpaque(int x, int y, int z) const;
    [[nodiscard]] bool aoOccludes(int x, int y, int z) const;
    [[nodiscard]] int opacity(int x, int y, int z) const;
    [[nodiscard]] Block blockType(int x, int y, int z) const;

  private:
    [[nodiscard]] std::size_t index(int x, int y, int z) const;
    [[nodiscard]] bool contains(int x, int y, int z) const;
    void propagate(std::vector<std::uint8_t>& levels) const;
    [[nodiscard]] bool cancelled() const;

    const World& world_;
    const std::atomic<bool>* cancellation_ = nullptr;
    int minimumX_ = 0;
    int minimumY_ = 0;
    int minimumZ_ = 0;
    int width_ = 0;
    int height_ = 0;
    int depth_ = 0;
    std::vector<std::uint8_t> skyLevels_;
    std::vector<std::uint8_t> blockLevels_;
    std::vector<std::uint8_t> opaque_;
    bool stored_ = false;
};

} // namespace mc::world

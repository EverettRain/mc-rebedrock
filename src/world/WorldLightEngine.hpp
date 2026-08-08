#pragma once

#include "world/World.hpp"

#include <atomic>
#include <cstddef>
#include <span>
#include <unordered_set>
#include <vector>

namespace mc::world {

struct LightSectionPosition final {
    int chunkX = 0;
    int sectionY = 0;
    int chunkZ = 0;

    [[nodiscard]] bool operator==(const LightSectionPosition&) const = default;
};

struct LightSectionPositionHash final {
    [[nodiscard]] std::size_t operator()(const LightSectionPosition& position) const noexcept;
};

// Persistent, incremental two-channel voxel lighting. Block mutations and
// light propagation are owned by the streaming worker, so readers only see a
// stable world while section meshes are built.
class WorldLightEngine final {
  public:
    explicit WorldLightEngine(const std::atomic<bool>* cancellation = nullptr)
        : cancellation_(cancellation) {}

    void initializeChunks(World& world, std::span<const ChunkPosition> positions);
    void updateBlock(World& world, int worldX, int y, int worldZ);
    void updateAfterChunkRemoval(World& world, ChunkPosition removed);

    [[nodiscard]] std::vector<LightSectionPosition> takeDirtySections();
    [[nodiscard]] std::size_t lastPropagationVisitCount() const {
        return lastPropagationVisitCount_;
    }

  private:
    struct Node final {
        int x = 0;
        int y = 0;
        int z = 0;
        [[nodiscard]] bool operator==(const Node&) const = default;
    };
    struct NodeHash final {
        [[nodiscard]] std::size_t operator()(const Node& node) const noexcept;
    };

    enum class Channel { Sky, Block };

    [[nodiscard]] bool cancelled() const;
    [[nodiscard]] static bool loaded(const World& world, int x, int y, int z);
    [[nodiscard]] static std::uint8_t level(const World& world, Channel channel,
                                            int x, int y, int z);
    [[nodiscard]] static std::uint8_t desiredLevel(const World& world, Channel channel,
                                                   const Node& node);
    static bool setLevel(World& world, Channel channel, const Node& node,
                         std::uint8_t value);
    void recomputeSkyColumn(World& world, int x, int z,
                            std::vector<Node>& changedSources);
    void settle(World& world, Channel channel, std::span<const Node> seeds);
    void propagateIncreases(World& world, Channel channel,
                            std::vector<Node>& queue);
    void markDirty(const Node& node);

    const std::atomic<bool>* cancellation_ = nullptr;
    std::unordered_set<LightSectionPosition, LightSectionPositionHash> dirtySections_;
    std::size_t lastPropagationVisitCount_ = 0U;
};

} // namespace mc::world

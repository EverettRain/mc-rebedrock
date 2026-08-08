#pragma once

#include "render/MeshData.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"
#include "world/WorldLighting.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace mc::world {

// Per-request O(1) sampler for the meshing hot path. Covers the chunk's
// requested Y-range padded by two cells in every axis (the vanilla AO overhang
// probe reaches pos + 2*faceNormal + in-plane, so two cells is the tight
// bound), reads the request chunk and its eight neighbours' block/light arrays
// directly, and caches opaque/aoOccludes flags so a corner costs one array
// read instead of ~13 unordered-map lookups.
class MeshLightingSnapshot final {
  public:
    static constexpr int kSamplePadding = 2;

    MeshLightingSnapshot(const World& world, ChunkPosition position,
                         int minimumSectionY, int maximumSectionY,
                         SmoothLightingQuality quality);

    [[nodiscard]] VoxelLightLevel level(int x, int y, int z) const;
    [[nodiscard]] float sky(int x, int y, int z) const;
    [[nodiscard]] float block(int x, int y, int z) const;
    [[nodiscard]] bool isOpaque(int x, int y, int z) const;
    [[nodiscard]] bool aoOccludes(int x, int y, int z) const;
    [[nodiscard]] int opacity(int x, int y, int z) const;
    [[nodiscard]] Block blockType(int x, int y, int z) const;
    [[nodiscard]] SmoothLightingQuality quality() const { return quality_; }

  private:
    [[nodiscard]] std::size_t index(int x, int y, int z) const;
    [[nodiscard]] bool contains(int x, int y, int z) const;

    const World& world_;
    int minimumX_ = 0;
    int minimumY_ = 0;
    int minimumZ_ = 0;
    int width_ = 0;
    int height_ = 0;
    int depth_ = 0;
    // bit0 = opaque, bit1 = aoOccludes; blockTypes_ holds the Block enum value.
    std::vector<std::uint8_t> flags_;
    std::vector<std::uint8_t> skyLevels_;
    std::vector<std::uint8_t> blockLevels_;
    std::vector<std::uint8_t> blockTypes_;
    SmoothLightingQuality quality_ = SmoothLightingQuality::Standard;
};

class ChunkMesher final {
  public:
    [[nodiscard]] static render::MeshData build(const Chunk& chunk);
    [[nodiscard]] static render::RenderMeshData buildSection(
        const World& world,
        ChunkPosition position,
        int sectionY);
    [[nodiscard]] static render::RenderMeshData buildSection(
        const World& world,
        ChunkPosition position,
        int sectionY,
        const ChunkLightSampler& lighting);
    // Meshes the section with an explicit quality. Constructs a padded
    // ChunkLightSampler like the 3-arg form (used by tests and previews).
    [[nodiscard]] static render::RenderMeshData buildSection(
        const World& world,
        ChunkPosition position,
        int sectionY,
        SmoothLightingQuality quality);
    // Fills `result` with the section mesh, reusing its vector capacity across
    // calls (clear keeps the buffers, so a pooled RenderMeshData stops the
    // per-section allocation churn). Returns false for out-of-range/missing or
    // empty sections; `result` is left cleared but usable.
    [[nodiscard]] static bool buildSection(
        const World& world,
        ChunkPosition position,
        int sectionY,
        const ChunkLightSampler& lighting,
        render::RenderMeshData& result);
    // Production worker path: samples through the snapshot at the quality it
    // was built with.
    [[nodiscard]] static bool buildSection(
        const World& world,
        ChunkPosition position,
        int sectionY,
        const MeshLightingSnapshot& lighting,
        render::RenderMeshData& result);
};

} // namespace mc::world

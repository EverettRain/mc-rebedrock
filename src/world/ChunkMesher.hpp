#pragma once

#include "render/MeshData.hpp"
#include "world/Block.hpp"
#include "world/BlockShape.hpp"
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
// directly, and caches the two AO predicates so a corner costs one array
// read instead of ~13 unordered-map lookups.
class MeshLightingSnapshot final {
  public:
    static constexpr int kSamplePadding = 2;

    MeshLightingSnapshot(const World& world, ChunkPosition position,
                         int minimumSectionY, int maximumSectionY);

    [[nodiscard]] VoxelLightLevel level(int x, int y, int z) const;
    [[nodiscard]] float sky(int x, int y, int z) const;
    [[nodiscard]] float block(int x, int y, int z) const;
    [[nodiscard]] bool isOpaque(int x, int y, int z) const;
    // 平滑光照的两问，26.1 分开问（见 Block.hpp 的 aoDarkens / aoBlocksView）：
    // 压不压暗一个角，和算不算挡住视线。
    [[nodiscard]] bool aoDarkens(int x, int y, int z) const;
    [[nodiscard]] bool aoBlocksView(int x, int y, int z) const;
    // RN-8a: does the cell's state seal that one face? Precomputed into flags_
    // at fill time, so the mesher's per-face cull test is a single bit read out
    // of an array the AO path has already pulled into cache.
    [[nodiscard]] bool faceOccludes(int x, int y, int z, Face face) const;
    [[nodiscard]] int opacity(int x, int y, int z) const;
    [[nodiscard]] Block blockType(int x, int y, int z) const;

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
    // bit0 = aoDarkens, bit1 = aoBlocksView, bits 2..7 = the RN-8a face-occlusion
    // mask. `isOpaque` used to own bit 0; it now derives from blockTypes_, which
    // this snapshot already stores — that kept the byte at eight bits when the
    // one AO predicate became two, instead of paying the +20% a wider array or a
    // seventh one would cost (the same trade the mask comment below rejects).
    // (bit 2+i seals Face(i), the BlockShape.hpp order). The mask rides in this
    // array's spare bits rather than in one of its own on purpose: it is six bits
    // per cell either way, but a seventh array would be +20% snapshot memory and
    // a second cacheline stream, while the AO path already touches flags_ — so
    // the cull test costs the same load twice over.
    // blockTypes_ holds the Block enum value.
    // blockTypes_ is uint16, not uint8: the Block enum widened past 256 (STRUCT
    // registration), so a u8 here silently truncated any block id > 255 — e.g. a
    // deepslate ore (id 336+) read back as a sapling/wheat (id & 0xFF), which is
    // not a full cube, so the mesher stopped culling the faces of the deepslate
    // around every deep ore and the deepslate layer's vertex count exploded. This
    // must track the Block width.
    std::vector<std::uint8_t> flags_;
    std::vector<std::uint8_t> skyLevels_;
    std::vector<std::uint8_t> blockLevels_;
    std::vector<std::uint16_t> blockTypes_;
};

// Which of vanilla's biome colour resolvers a face reads — 26.1's
// BlockColors.createDefault() registers one BlockTintSource per block, and these
// are the three that vary by biome (`grass()`, `foliage()`, `water()`) plus "no
// tint". A face whose kind is not None multiplies its texel by a per-vertex
// colour the mesher resolved from the biome.
enum class BiomeTintKind : std::uint8_t {
    None,
    Grass,
    Foliage,
    Water,
};

[[nodiscard]] BiomeTintKind biomeTintKind(Block block, Face face);

// The biome tint of one column, for consumers outside the mesher — today the
// water particles, which sample the water block's (untinted) atlas layer and so
// have to carry the same colour the water surface itself is tinted with.
//
// This is vanilla's `BiomeColors.getAverage*Color`: the mean of the biome
// colours over the (2r+1)^2 block window around the column, r = biomeBlendRadius
// = 2. `BlockTintSources.waterParticles()` overrides `colorAsTerrainParticle` to
// exactly that call, which is why a splash matches the water it came out of even
// on a biome border.
//
// The mesher does not call this: it resolves a whole chunk's columns at once
// behind BiomeTintCache, which amortises the window. Both go through the same
// per-sample colour, so the two cannot disagree — the cache is an optimisation,
// not a second answer. Do not add a third path; a tint resolved anywhere else
// is a colour that drifts.
[[nodiscard]] std::array<std::uint8_t, 3> biomeTintAt(const World& world, BiomeTintKind kind,
                                                      int x, int z);

// The atlas layer that face samples in the terrain mesh, in the block's default
// state, with the untinted-terrain overrides applied — the same choice the
// mesher itself makes.
//
// These two exist together for one reason: a face that takes a vertex tint must
// sample an UNTINTED layer. Sampling a layer the atlas bake already multiplied
// by a colour tints it twice, which squares the colour and reads as "everything
// is too dark". `biome_tint_layers` joins this against
// `render::TextureArrayPixels::preTintedLayers` to keep the two halves honest.
[[nodiscard]] float terrainAtlasLayer(Block block, Face face);

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
    // Production worker path.
    [[nodiscard]] static bool buildSection(
        const World& world,
        ChunkPosition position,
        int sectionY,
        const MeshLightingSnapshot& lighting,
        render::RenderMeshData& result);
};

} // namespace mc::world

#pragma once

#include "world/gen/Biome.hpp"
#include "world/gen/JavaRandom.hpp"

#include "world/Chunk.hpp"

namespace mc::world::gen {

// A minimal write target for the tree shape placers, addressed in world
// coordinates. Trees grow in two places with the same shapes: during chunk
// generation (a Chunk, where the crown used to be silently clipped at the
// 0..15 x/z box) and at runtime from a sapling (a World, where a tree may
// cross a chunk border). One writer type per backing store keeps the shape
// code shared and the bounds policy where it belongs.
class TreeWriter {
  public:
    virtual ~TreeWriter() = default;
    [[nodiscard]] virtual Block block(int x, int y, int z) const = 0;
    virtual bool setBlock(int x, int y, int z, Block value) = 0;
    virtual bool setOrientation(int x, int y, int z, BlockOrientation value) = 0;
};

// A tree block that fell outside the chunk being generated: it belongs to a
// neighbouring chunk and is held back so the streamer can apply it once that
// neighbour is published, instead of clipping the crown flat at the border the
// way vanilla's ChunkRegion lets a tree spill into the surrounding chunks.
struct TreeBorderBlock final {
    int worldX = 0;
    int y = 0;
    int worldZ = 0;
    Block block = Block::Air;
    BlockOrientation orientation = BlockOrientation::North;
};

// Writes into one chunk. World coordinates are translated back to chunk-local
// space; cells outside the chunk's 0..15 x/z box are recorded in `borderBlocks`
// rather than dropped, so a crown that crosses the border is completed when the
// neighbouring chunk arrives.
class ChunkTreeWriter final : public TreeWriter {
  public:
    ChunkTreeWriter(Chunk& chunk, int chunkX, int chunkZ,
                    std::vector<TreeBorderBlock>& borderBlocks)
        : chunk_(chunk), chunkX_(chunkX), chunkZ_(chunkZ), borderBlocks_(borderBlocks) {}

    [[nodiscard]] Block block(int x, int y, int z) const override;
    bool setBlock(int x, int y, int z, Block value) override;
    bool setOrientation(int x, int y, int z, BlockOrientation value) override;

  private:
    [[nodiscard]] bool inBounds(int x, int y, int z) const;

    Chunk& chunk_;
    int chunkX_ = 0;
    int chunkZ_ = 0;
    std::vector<TreeBorderBlock>& borderBlocks_;
};

// Whether a cell is free for a trunk or a leaf to occupy: air, leaves, the
// cross plants and water. Used both while placing the tree and again when the
// streamer finishes a crown block in a neighbouring chunk.
[[nodiscard]] bool treeReplaceable(Block block);

// The soil a sapling or tree needs underneath, matching the plant blocks'
// BlockSupport::Soil set plus coarse dirt and podzol.
[[nodiscard]] bool isSoilForPlants(Block block);

// The TreeChoice a sapling grows into, one per wood set. Vanilla's per-species
// SaplingGenerator; the fancy-oak variant is a deliberate omission for now.
[[nodiscard]] TreeChoice treeChoiceForSapling(Block sapling);

[[nodiscard]] bool isSapling(Block block);

// Grows a tree rooted at (worldX, groundY, worldZ), where groundY is the top
// of the soil cell the trunk stands on. Returns false when the soil below is
// unsuitable or the tree would exceed the build height; nothing is written on
// failure.
[[nodiscard]] bool growTree(
    TreeWriter& writer,
    JavaRandom& random,
    const TreeChoice& choice,
    int worldX,
    int groundY,
    int worldZ);

} // namespace mc::world::gen

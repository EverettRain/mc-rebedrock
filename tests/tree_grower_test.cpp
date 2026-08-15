#include "world/gen/TreeGrower.hpp"

#include "world/Chunk.hpp"
#include "world/ChunkSection.hpp"

#include <array>
#include <cassert>
#include <numeric>
#include <vector>

using namespace mc;

// The tree shapes are shared between chunk generation and sapling growth, and
// the chunk-local writer must hold the crown back at the border instead of
// dropping it, so the streamer can finish it in the neighbouring chunk.
int main() {
    world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, world::Block::Grass);
        }
    }
    world::gen::TreeChoice oak{
        world::gen::TreeKind::Oak, world::Block::OakLog, world::Block::OakLeaves, 1.0F};

    // A tree away from the border stays entirely inside the chunk.
    {
        std::vector<world::gen::TreeBorderBlock> border;
        world::gen::ChunkTreeWriter writer{chunk, 0, 0, border};
        world::gen::JavaRandom random{12345U};
        assert(world::gen::growTree(writer, random, oak, 8, 0, 8));
        assert(border.empty());
        // The vanilla BlobFoliagePlacer hangs a round crown off the trunk top:
        // logs climb the centre and leaves fill the layers beneath.
        int logs = 0;
        int leaves = 0;
        for (int y = 1; y <= 8; ++y) {
            for (int x = 4; x <= 12; ++x) {
                for (int z = 4; z <= 12; ++z) {
                    if (chunk.block(x, y, z) == world::Block::OakLog) ++logs;
                    if (chunk.block(x, y, z) == world::Block::OakLeaves) ++leaves;
                }
            }
        }
        assert(logs >= 4);
        assert(leaves >= 40);
        for (int y = 1; y <= 4; ++y) {
            assert(chunk.orientation(8, y, 8) == world::BlockOrientation::Up);
        }
    }

    // A tree hugging the +X border records its spilled crown instead of
    // dropping it, and every held block is a leaf, never a stray log.
    {
        world::Chunk borderChunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                borderChunk.setBlock(x, 0, z, world::Block::Grass);
            }
        }
        std::vector<world::gen::TreeBorderBlock> border;
        world::gen::ChunkTreeWriter writer{borderChunk, 0, 0, border};
        world::gen::JavaRandom random{999U};
        assert(world::gen::growTree(writer, random, oak, 15, 0, 8));
        assert(!border.empty());
        for (const auto& block : border) {
            assert(block.state.block() == world::Block::OakLeaves);
            assert(block.worldX >= 16);
        }
    }

    // A dark oak grows a 2x2 leaning trunk under a wide multi-layer canopy: far
    // more leaves than the old two-layer crown, spread across several layers.
    {
        world::Chunk darkChunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                darkChunk.setBlock(x, 0, z, world::Block::Grass);
            }
        }
        std::vector<world::gen::TreeBorderBlock> darkBorder;
        world::gen::ChunkTreeWriter writer{darkChunk, 0, 0, darkBorder};
        world::gen::JavaRandom random{2026U};
        world::gen::TreeChoice darkOak{
            world::gen::TreeKind::DarkOak, world::Block::DarkOakLog, world::Block::DarkOakLeaves, 1.0F};
        assert(world::gen::growTree(writer, random, darkOak, 8, 0, 8));
        int logs = 0;
        std::array<int, 14> leavesPerLayer{};
        for (int y = 1; y <= 14; ++y) {
            for (int x = 0; x < 16; ++x) {
                for (int z = 0; z < 16; ++z) {
                    if (darkChunk.block(x, y, z) == world::Block::DarkOakLog) ++logs;
                    if (darkChunk.block(x, y, z) == world::Block::DarkOakLeaves) {
                        ++leavesPerLayer[static_cast<std::size_t>(y - 1)];
                    }
                }
            }
        }
        // The 2x2 trunk runs 6..8 blocks tall (24+ logs) plus branch columns.
        assert(logs >= 24);
        const int leaves =
            std::accumulate(leavesPerLayer.begin(), leavesPerLayer.end(), 0);
        // The four-layer canopy (6x6/8x8/6x6 + branches) spreads well beyond the
        // old two-layer crown's ~40 leaves.
        assert(leaves >= 90);
        int layersWithLeaves = 0;
        for (const int count : leavesPerLayer) {
            if (count > 0) ++layersWithLeaves;
        }
        assert(layersWithLeaves >= 3);
    }

    return 0;
}

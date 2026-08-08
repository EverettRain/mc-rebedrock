#include "world/gen/TreeGrower.hpp"

#include "world/Chunk.hpp"
#include "world/ChunkSection.hpp"

#include <cassert>
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
            assert(block.block == world::Block::OakLeaves);
            assert(block.worldX >= 16);
        }
    }

    return 0;
}


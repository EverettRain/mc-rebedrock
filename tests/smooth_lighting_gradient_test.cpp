// RN-18: smooth lighting / AO across a face that does not fill its cell.
//
// The field report: a stair standing on the ground has its side shading broken
// into two bands with a visible seam at half height. The cause was in
// `ChunkMesher`'s `cornerPositions`, which picked which 2x2 ring of cells a
// vertex averages with
//
//     const int signA = cornerCoordinate(corner, tangentA) < 0.5F ? -1 : 1;
//
// — a BINARY "which half of the cell am I in", never a position. Worse, the
// shaped-block path handed it the UNIT cube corner (0 or 1) and only afterwards
// remapped the vertex into the box, so a stair's lower box ran a whole cell's
// gradient across its bottom half and its upper box ran another whole one across
// the top. The two met at y=0.5 from opposite ends of the same gradient, which
// is the seam.
//
// 26.1 does it the other way round (`BlockModelLighter.prepareQuadAmbientOcclusion`,
// BlockModelLighter.java:36-196): compute the four CELL corner values first, then
// weight them by the vertex's fractional position in the face plane. A half-height
// face therefore lands on the MIDDLE of the cell's gradient, and two stacked
// boxes agree at the height they share.
//
// All of this is CPU-side and readable off the built mesh: the vertex carries its
// AO and its light as bytes, so "the value at half height is the average of the
// values at the ends" is an assertion, not a screenshot.

#include "render/MeshData.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/ChunkMesher.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using mc::world::Block;
using mc::world::BlockOrientation;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::World;

// One byte of AO resolution, with a hair of slack for the rounding that happens
// on both sides of an average.
constexpr float kByte = 1.0F / 255.0F;

// Section 0 of a one-chunk world: a stone floor at the bottom row, plus whatever
// the caller places.
[[nodiscard]] World flooredWorld() {
    World world;
    Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, mc::world::kMinY, z, Block::Stone);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

struct Sample final {
    float ambientOcclusion = 0.0F;
    float skyLight = 0.0F;
};

// Every vertex of `mesh` with the given normal at exactly this position. A face
// that two quads share contributes one entry per quad, which is the point: the
// seam is the case where those entries disagree.
[[nodiscard]] std::vector<Sample> samplesAt(const mc::render::RenderMeshData& mesh,
                                            const glm::vec3& normal, const glm::vec3& position) {
    std::vector<Sample> found;
    for (const auto* part : {&mesh.mesh, &mesh.cutoutMesh, &mesh.translucentMesh}) {
        for (const auto& vertex : part->vertices) {
            const auto actual = mc::render::decodeNormal(vertex);
            if (std::abs(actual.x - normal.x) > 0.01F || std::abs(actual.y - normal.y) > 0.01F ||
                std::abs(actual.z - normal.z) > 0.01F) {
                continue;
            }
            const auto p = mc::render::decodeLocalPosition(vertex);
            if (std::abs(p.x - position.x) > 0.002F || std::abs(p.y - position.y) > 0.002F ||
                std::abs(p.z - position.z) > 0.002F) {
                continue;
            }
            found.push_back({mc::render::decodeAmbientOcclusion(vertex),
                             static_cast<float>(vertex.skyLight) / 255.0F});
        }
    }
    return found;
}

// The one value every vertex at this position carries. Asserts they agree, which
// is itself the continuity claim wherever two quads meet.
[[nodiscard]] Sample uniqueSampleAt(const mc::render::RenderMeshData& mesh,
                                    const glm::vec3& normal, const glm::vec3& position,
                                    const char* what) {
    const auto found = samplesAt(mesh, normal, position);
    if (found.empty()) {
        std::cerr << what << ": no vertex at (" << position.x << ", " << position.y << ", "
                  << position.z << ")\n";
    }
    assert(!found.empty());
    for (const Sample& sample : found) {
        if (std::abs(sample.ambientOcclusion - found[0].ambientOcclusion) > kByte ||
            std::abs(sample.skyLight - found[0].skyLight) > kByte) {
            std::cerr << what << ": vertices meeting at (" << position.x << ", " << position.y
                      << ", " << position.z << ") disagree — ao " << found[0].ambientOcclusion
                      << " vs " << sample.ambientOcclusion << ", sky " << found[0].skyLight
                      << " vs " << sample.skyLight << "\n";
        }
        assert(std::abs(sample.ambientOcclusion - found[0].ambientOcclusion) <= kByte);
        assert(std::abs(sample.skyLight - found[0].skyLight) <= kByte);
    }
    return found[0];
}

void expectMidpoint(float low, float middle, float high, const char* what) {
    const float expected = (low + high) * 0.5F;
    if (std::abs(middle - expected) > 2.0F * kByte) {
        std::cerr << what << ": half-height value " << middle << " is not the midpoint of " << low
                  << " and " << high << " (expected " << expected << ")\n";
    }
    assert(std::abs(middle - expected) <= 2.0F * kByte);
}

} // namespace

int main() {
    // --- The report itself: a stair on the ground, seen from the side. --------
    //
    // A north-facing bottom stair is two boxes — (0,0,0)-(1,.5,1) and
    // (0,.5,0)-(1,1,.5) — so its west wall is covered by the lower box below
    // y=0.5 and by the upper box above it. Both quads have a vertex ON y=0.5, and
    // before RN-18 those two vertices carried the two ENDS of the cell's
    // gradient: the lower box's top vertex had the cell's top value, the upper
    // box's bottom vertex had the cell's bottom value. That step is the band.
    {
        World world = flooredWorld();
        world.setState(8, mc::world::kMinY + 1, 8,
                       BlockState{Block::OakStairs, BlockOrientation::North});
        const auto mesh = mc::world::ChunkMesher::buildSection(world, {0, 0}, 0);

        const glm::vec3 west{-1.0F, 0.0F, 0.0F};
        const Sample bottom = uniqueSampleAt(mesh, west, {8.0F, 1.0F, 8.0F}, "stair west bottom");
        // uniqueSampleAt is the continuity assertion: two quads meet here and
        // must carry the same value.
        const Sample middle = uniqueSampleAt(mesh, west, {8.0F, 1.5F, 8.0F}, "stair west middle");
        const Sample top = uniqueSampleAt(mesh, west, {8.0F, 2.0F, 8.0F}, "stair west top");
        assert(samplesAt(mesh, west, {8.0F, 1.5F, 8.0F}).size() >= 2 &&
               "the two boxes must both put a vertex on the shared height, or this proves nothing");

        // There is a gradient to interpolate at all: the floor darkens the bottom
        // of the wall and the open sky brightens the top. Without this the three
        // assertions below would pass on a flat face.
        assert(top.ambientOcclusion - bottom.ambientOcclusion > 0.2F);
        assert(top.skyLight - bottom.skyLight > 0.2F);

        // And the shared height is the MIDDLE of that gradient — 26.1's bilinear
        // weight at v = 0.5, not either end of it.
        expectMidpoint(bottom.ambientOcclusion, middle.ambientOcclusion, top.ambientOcclusion,
                       "stair west face AO");
        expectMidpoint(bottom.skyLight, middle.skyLight, top.skyLight, "stair west face sky light");
    }

    // --- A full cube is untouched, to the byte. ------------------------------
    //
    // The blend degenerates at u,v in {0,1}: weight 1 on one corner, 0 on the
    // other three. That is what makes this node safe for the ~400 blocks that are
    // whole cubes, and it is worth asserting rather than assuming — a weight
    // function that smeared even slightly at the ends would repaint the entire
    // world.
    {
        World world = flooredWorld();
        world.setState(3, mc::world::kMinY + 1, 3, BlockState{Block::Stone});
        const auto mesh = mc::world::ChunkMesher::buildSection(world, {0, 0}, 0);
        const glm::vec3 west{-1.0F, 0.0F, 0.0F};

        std::set<std::string> distinct;
        for (const float y : {1.0F, 2.0F}) {
            for (const float z : {3.0F, 4.0F}) {
                const Sample sample = uniqueSampleAt(mesh, west, {3.0F, y, z}, "stone west");
                distinct.insert(std::to_string(
                    static_cast<int>(std::lround(sample.ambientOcclusion * 255.0F))));
            }
        }
        // Exactly two values — the floor-shadowed bottom pair and the open top
        // pair. A third value would mean a corner had been blended with another.
        assert(distinct.size() == 2);
        // And no vertex anywhere in between: a cube face has no half-height
        // vertex to carry one.
        assert(samplesAt(mesh, west, {3.0F, 1.5F, 3.0F}).empty());
    }

    // --- The same shared-height agreement, on the other shapes that split a
    //     cell. A slab's side stops at 0.5 and a wall's post at 0.25/0.75; both
    //     used to get a whole cell's gradient over a fraction of a cell. ---
    {
        struct Case final {
            Block block;
            float sharedHeight;
            const char* what;
        };
        const Case cases[]{
            {Block::OakSlab, 0.5F, "slab"},
            {Block::StoneSlab, 0.5F, "stone slab"},
        };
        for (const Case& item : cases) {
            World world = flooredWorld();
            world.setState(6, mc::world::kMinY + 1, 6, BlockState{item.block});
            const auto mesh = mc::world::ChunkMesher::buildSection(world, {0, 0}, 0);
            const glm::vec3 west{-1.0F, 0.0F, 0.0F};
            const Sample bottom = uniqueSampleAt(mesh, west, {6.0F, 1.0F, 6.0F}, item.what);
            const Sample cut =
                uniqueSampleAt(mesh, west, {6.0F, 1.0F + item.sharedHeight, 6.0F}, item.what);
            // A slab has no upper box, so there is no second quad to agree with.
            // What is asserted here is the other half of the same claim: the
            // truncated face gets the gradient's value AT its height, not the
            // value from the cell's top.
            assert(bottom.ambientOcclusion < cut.ambientOcclusion);
            World tallWorld = flooredWorld();
            tallWorld.setState(6, mc::world::kMinY + 1, 6, BlockState{Block::Stone});
            const auto tallMesh = mc::world::ChunkMesher::buildSection(tallWorld, {0, 0}, 0);
            const Sample cellTop =
                uniqueSampleAt(tallMesh, west, {6.0F, 2.0F, 6.0F}, "full cube west top");
            expectMidpoint(bottom.ambientOcclusion, cut.ambientOcclusion, cellTop.ambientOcclusion,
                           item.what);
        }
    }

    std::cout << "smooth lighting gradient: ok\n";
    return 0;
}

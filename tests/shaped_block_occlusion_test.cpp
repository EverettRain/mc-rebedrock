// RN-8e: `canOcclude` and `skipRendering` are their own axes, not readings of
// the render bucket.
//
// The field report: stand two stairs on top of each other and the part of the
// upper stair's underside that shows through the lower one's notch is missing.
// Two conflations put it there, and they pull in opposite directions:
//
//   skipsAgainstSame  was `renderLayer != Opaque`, on the reasoning that a
//                     non-Opaque bucket meant glass/water/panes. It also meant
//                     every stair — and the first clause of `shouldRenderFace`
//                     drops a face against the SAME block unconditionally, so
//                     two stacked stairs deleted the faces they shared.
//   canOcclude        was `renderLayer == Opaque` too, so a stair's mask was
//                     zero and it occluded nothing: every face of every block
//                     pressed against a stair was drawn into the dark.
//
// 26.1 keeps these apart. `skipRendering` is a METHOD a handful of blocks
// override — HalfTransparentBlock (glass, ice), LiquidBlock (water),
// IronBarsBlock (panes), LeavesBlock, MangroveRootsBlock — and nothing derives
// it from a render type. `canOcclude` is a Properties flag that defaults to TRUE
// and is cleared by `noOcclusion()`; a stair never clears it
// (`registerLegacyStair` copies its base block, Blocks.java:6831).
//
// What this file holds is both directions at once: the faces that must APPEAR
// (the report), the faces that must DISAPPEAR (the overdraw the stair's mask now
// removes), and — the half that is easy to forget — the faces that must not
// change at all, because glass against glass and leaves against leaves are the
// cases the old blanket rule happened to get right.

#include "render/MeshData.hpp"
#include "world/Block.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/ChunkMesher.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <utility>

namespace {

using mc::world::Block;
using mc::world::BlockOrientation;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::World;

// A world holding only what the caller places. Deliberately not a floored one:
// `facesAt` matches a vertex by position, and a quad in the cell NEXT to the one
// under test shares its corner coordinates — with a full floor those neighbours
// leak into the count and turn "this face is gone" into "some faces are gone".
[[nodiscard]] World emptyWorld() {
    World world;
    world.setChunk({0, 0}, Chunk{});
    return world;
}

// Vertices carrying `normal` whose position lies on the plane `axis` ==
// `coordinate` AND inside the given footprint. A drawn quad contributes four,
// a culled one none.
[[nodiscard]] int facesAt(const mc::render::RenderMeshData& mesh, const glm::vec3& normal,
                          float planeY, float minX, float maxX, float minZ, float maxZ) {
    int count = 0;
    for (const auto* part : {&mesh.mesh, &mesh.cutoutMesh, &mesh.translucentMesh}) {
        for (const auto& vertex : part->vertices) {
            const auto actual = mc::render::decodeNormal(vertex);
            if (std::abs(actual.x - normal.x) > 0.01F || std::abs(actual.y - normal.y) > 0.01F ||
                std::abs(actual.z - normal.z) > 0.01F) {
                continue;
            }
            const auto p = mc::render::decodeLocalPosition(vertex);
            if (std::abs(p.y - planeY) > 0.002F) continue;
            if (p.x < minX - 0.002F || p.x > maxX + 0.002F) continue;
            if (p.z < minZ - 0.002F || p.z > maxZ + 0.002F) continue;
            ++count;
        }
    }
    return count;
}

// A one-chunk world holding `current` at (1, y, 1) and `neighbour` at (2, y, 1),
// so the shared wall is the plane x = 2.
[[nodiscard]] mc::render::RenderMeshData meshPair(BlockState current, BlockState neighbour) {
    World world;
    Chunk chunk;
    chunk.setState(1, mc::world::kMinY + 1, 1, current);
    chunk.setState(2, mc::world::kMinY + 1, 1, neighbour);
    if (current.block() == Block::Water) chunk.setFluidLevel(1, mc::world::kMinY + 1, 1, 0U);
    if (neighbour.block() == Block::Water) chunk.setFluidLevel(2, mc::world::kMinY + 1, 1, 0U);
    world.setChunk({0, 0}, std::move(chunk));
    return mc::world::ChunkMesher::buildSection(world, {0, 0}, 0);
}

// How many vertices sit on the shared wall x = 2 with the given normal.
[[nodiscard]] int wallVertices(const mc::render::RenderMeshData& mesh, float normalX) {
    int count = 0;
    for (const auto* part : {&mesh.mesh, &mesh.cutoutMesh, &mesh.translucentMesh}) {
        for (const auto& vertex : part->vertices) {
            const auto actual = mc::render::decodeNormal(vertex);
            if (std::abs(actual.x - normalX) > 0.01F || std::abs(actual.y) > 0.01F ||
                std::abs(actual.z) > 0.01F) {
                continue;
            }
            const auto p = mc::render::decodeLocalPosition(vertex);
            if (std::abs(p.x - 2.0F) < 0.002F) ++count;
        }
    }
    return count;
}

} // namespace

int main() {
    const BlockState stair{Block::OakStairs, BlockOrientation::North};

    // --- The report: two stacked stairs. -------------------------------------
    //
    // The upper stair's underside is a full-footprint quad at the shared plane.
    // Nothing above the lower stair covers the half its notch leaves open, so
    // vanilla draws it and a player sees it. Before RN-8e it was culled outright
    // by the same-block rule and there was nothing there at all.
    {
        World world = emptyWorld();
        world.setState(8, mc::world::kMinY + 1, 8, stair);
        world.setState(8, mc::world::kMinY + 2, 8, stair);
        const auto mesh = mc::world::ChunkMesher::buildSection(world, {0, 0}, 0);

        const int underside = facesAt(mesh, {0.0F, -1.0F, 0.0F}, 2.0F, 8.0F, 9.0F, 8.0F, 9.0F);
        if (underside != 4) {
            std::cerr << "the upper stair's underside has " << underside
                      << " vertices at the shared plane; expected 4\n";
        }
        assert(underside == 4);

        // And the other side of the same seam: the lower stair's TOP face is
        // covered by the upper stair's slab, which spans the whole footprint, so
        // it must be gone. This is the half a blanket "draw everything against
        // the same block" fix would have got wrong — the answer is the shape's,
        // not "always draw" and not "always cull".
        const int lowerTop = facesAt(mesh, {0.0F, 1.0F, 0.0F}, 2.0F, 8.0F, 9.0F, 8.0F, 9.0F);
        if (lowerTop != 0) {
            std::cerr << "the lower stair's top face survived under the upper stair's slab ("
                      << lowerTop << " vertices)\n";
        }
        assert(lowerTop == 0);
    }

    // --- The other direction: a stair now occludes what it covers. -----------
    //
    // A stair's lower slab fills the cell's whole footprint from y=0 to y=0.5, so
    // the top face of the block underneath cannot be seen. It was drawn anyway —
    // `canOcclude` was false for every Cutout block, so `faceOcclusionMask` was
    // zero and the shape criterion never ran.
    {
        World world = emptyWorld();
        world.setState(8, mc::world::kMinY, 8, BlockState{Block::Stone});
        world.setState(8, mc::world::kMinY + 1, 8, stair);
        const auto mesh = mc::world::ChunkMesher::buildSection(world, {0, 0}, 0);
        const int groundTop = facesAt(mesh, {0.0F, 1.0F, 0.0F}, 1.0F, 8.0F, 9.0F, 8.0F, 9.0F);
        if (groundTop != 0) {
            std::cerr << "the ground under a stair still draws its top face (" << groundTop
                      << " vertices)\n";
        }
        assert(groundTop == 0);

        // The control, so the assertion above cannot pass by the stone simply
        // not being meshed: take the stair away and the face comes back.
        World bare = emptyWorld();
        bare.setState(8, mc::world::kMinY, 8, BlockState{Block::Stone});
        const auto bareMesh = mc::world::ChunkMesher::buildSection(bare, {0, 0}, 0);
        assert(facesAt(bareMesh, {0.0F, 1.0F, 0.0F}, 1.0F, 8.0F, 9.0F, 8.0F, 9.0F) == 4);
    }

    // --- A top-half stair seals UP instead, and the block above loses its
    //     bottom face. The mask moves with the state; nothing here is keyed on
    //     the block. ---
    {
        World world = emptyWorld();
        world.setState(8, mc::world::kMinY + 1, 8,
                       BlockState{Block::OakStairs, BlockOrientation::North}.withStairHalf(
                           mc::world::SlabPortion::Top));
        world.setState(8, mc::world::kMinY + 2, 8, BlockState{Block::Stone});
        const auto mesh = mc::world::ChunkMesher::buildSection(world, {0, 0}, 0);
        assert(facesAt(mesh, {0.0F, -1.0F, 0.0F}, 2.0F, 8.0F, 9.0F, 8.0F, 9.0F) == 0);
        // Whereas a BOTTOM stair under the same stone leaves it visible: the
        // stair's step only covers half the cell there.
        World bottomWorld = emptyWorld();
        bottomWorld.setState(8, mc::world::kMinY + 1, 8, stair);
        bottomWorld.setState(8, mc::world::kMinY + 2, 8, BlockState{Block::Stone});
        const auto bottomMesh = mc::world::ChunkMesher::buildSection(bottomWorld, {0, 0}, 0);
        assert(facesAt(bottomMesh, {0.0F, -1.0F, 0.0F}, 2.0F, 8.0F, 9.0F, 8.0F, 9.0F) == 4);
    }

    // --- MUST NOT CHANGE. -----------------------------------------------------
    //
    // The blanket rule was right about these, and they are the reason it existed.
    // Each one is a block that overrides `skipRendering` in 26.1, named at its
    // declaration; if any of them lost the bit, this is where it shows.
    {
        // Glass against glass: the shared pane is not drawn from either side.
        // (HalfTransparentBlock.java:27.)
        const auto glass = meshPair(BlockState{Block::Glass}, BlockState{Block::Glass});
        assert(wallVertices(glass, 1.0F) == 0);
        assert(wallVertices(glass, -1.0F) == 0);
        // Stained glass is the same class and must behave the same.
        const auto stained =
            meshPair(BlockState{Block::RedStainedGlass}, BlockState{Block::RedStainedGlass});
        assert(wallVertices(stained, 1.0F) == 0);
        assert(wallVertices(stained, -1.0F) == 0);
        // Two DIFFERENT glasses are not "the same block", so the wall is drawn —
        // which is also true in vanilla, where skipRendering tests `is(this)`.
        const auto mixed =
            meshPair(BlockState{Block::Glass}, BlockState{Block::RedStainedGlass});
        assert(wallVertices(mixed, 1.0F) == 4);

        // Water against water. (LiquidBlock.java:136.)
        const auto water = meshPair(BlockState{Block::Water}, BlockState{Block::Water});
        assert(wallVertices(water, 1.0F) == 0);
        assert(wallVertices(water, -1.0F) == 0);

        // Ice against ice. (HalfTransparentBlock again.)
        const auto ice = meshPair(BlockState{Block::Ice}, BlockState{Block::Ice});
        assert(wallVertices(ice, 1.0F) == 0);

        // Leaves keep exactly one internal sheet — this mesher's own decision
        // (both sheets z-fight, neither makes the canopy hollow), preserved
        // because RN-8e moves WHO declares the bit, not what leaves look like.
        const auto leaves = meshPair(BlockState{Block::OakLeaves}, BlockState{Block::OakLeaves});
        assert(wallVertices(leaves, 1.0F) + wallVertices(leaves, -1.0F) == 4);

        // Stone against stone: culled by the SHAPE, not by the same-block rule,
        // and therefore untouched by any of this.
        const auto stone = meshPair(BlockState{Block::Stone}, BlockState{Block::Stone});
        assert(wallVertices(stone, 1.0F) == 0);
        assert(wallVertices(stone, -1.0F) == 0);
    }

    // --- The registered behaviour change on the rest of the Cutout roster. ----
    //
    // Two trapdoors side by side used to hide the wall between them because they
    // were the same block; vanilla draws it (a trapdoor is `noOcclusion()` and
    // overrides no `skipRendering`, Blocks.java:2115). The quads are mutually
    // hidden, so this is overdraw rather than something a player sees — recorded
    // here so it is a decision and not a surprise.
    {
        const auto trapdoors =
            meshPair(BlockState{Block::OakTrapdoor}, BlockState{Block::OakTrapdoor});
        assert(wallVertices(trapdoors, 1.0F) == 4);
        assert(wallVertices(trapdoors, -1.0F) == 4);
    }

    // --- The declarations themselves, so a new block joins a list rather than
    //     inheriting an answer from its render bucket. ---
    {
        using mc::world::canOcclude;
        using mc::world::skipsRenderingAgainstSelf;
        // Everything that overrides skipRendering in 26.1, and nothing else in
        // the shaped families.
        for (const Block block : {Block::Glass, Block::RedStainedGlass, Block::Ice, Block::Water,
                                  Block::OakLeaves, Block::MangroveRoots}) {
            assert(skipsRenderingAgainstSelf(block));
        }
        for (const Block block :
             {Block::OakStairs, Block::CobblestoneWall, Block::OakFenceGate, Block::OakDoor,
              Block::OakTrapdoor, Block::StoneButton, Block::StonePressurePlate, Block::Repeater,
              Block::Torch, Block::Chest, Block::Stone}) {
            assert(!skipsRenderingAgainstSelf(block));
        }
        // A block in the Opaque bucket keeps the default answer on both axes.
        assert(canOcclude(Block::Stone));
        assert(!skipsRenderingAgainstSelf(Block::Stone));
        // And the two axes are genuinely independent: a stair occludes and does
        // not skip; glass skips and does not occlude.
        assert(canOcclude(Block::OakStairs) && !skipsRenderingAgainstSelf(Block::OakStairs));
        assert(!canOcclude(Block::Glass) && skipsRenderingAgainstSelf(Block::Glass));
    }

    std::cout << "shaped block occlusion: ok\n";
    return 0;
}

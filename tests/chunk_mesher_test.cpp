#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/ChunkMesher.hpp"
#include "world/ElementModelBaker.hpp"
#include "world/SurfaceGenerator.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void expectNear(float actual, float expected, std::string_view context) {
    if (std::abs(actual - expected) <= 0.001F) {
        return;
    }
    std::ostringstream message;
    message << context << ": expected " << expected << ", actual " << actual;
    throw std::runtime_error(message.str());
}

// The geometry tests below build section 0 of chunk (0,0), whose section origin
// is the world origin, so decoded local positions equal world positions.
[[nodiscard]] glm::vec3 worldPos(const mc::render::VoxelVertex& vertex) {
    return mc::render::decodeLocalPosition(vertex);
}

// RN-8a: how many vertices of `mesh` carry `normal` and sit exactly on the plane
// `axis` (0=x, 1=y, 2=z) == `coordinate`. A drawn face contributes its 4 corners,
// a culled one contributes none — which is the whole question the RN-8a
// criterion answers, asked at the mesh instead of at the predicate.
[[nodiscard]] int faceVertices(const mc::render::MeshData& mesh, const glm::vec3& normal,
                               int axis, float coordinate) {
    int count = 0;
    for (const auto& vertex : mesh.vertices) {
        const auto actual = mc::render::decodeNormal(vertex);
        if (std::abs(actual.x - normal.x) > 0.01F || std::abs(actual.y - normal.y) > 0.01F ||
            std::abs(actual.z - normal.z) > 0.01F) {
            continue;
        }
        const auto position = worldPos(vertex);
        const float value = axis == 0 ? position.x : (axis == 1 ? position.y : position.z);
        if (std::abs(value - coordinate) < 0.001F) {
            ++count;
        }
    }
    return count;
}

// Meshes section 0 of a one-chunk world holding exactly `current` at (1, y, 1)
// and `neighbour` at (2, y, 1) — an X-adjacent pair, so `current`'s +X face lies
// on the plane x = 2.
[[nodiscard]] mc::render::RenderMeshData meshPair(mc::world::BlockState current,
                                                  mc::world::BlockState neighbour) {
    mc::world::World world;
    mc::world::Chunk chunk;
    chunk.setState(1, mc::world::kMinY + 1, 1, current);
    chunk.setState(2, mc::world::kMinY + 1, 1, neighbour);
    if (current.block() == mc::world::Block::Water) {
        chunk.setFluidLevel(1, mc::world::kMinY + 1, 1, 0U);
    }
    if (neighbour.block() == mc::world::Block::Water) {
        chunk.setFluidLevel(2, mc::world::kMinY + 1, 1, 0U);
    }
    world.setChunk({0, 0}, std::move(chunk));
    return mc::world::ChunkMesher::buildSection(world, {0, 0}, 0);
}

// The same pair stacked instead: `current` at (1, y, 1), `neighbour` directly
// above it, so `current`'s +Y face lies on the plane y = 2 (section 0's origin is
// kMinY, so the cell at kMinY + 1 has its ceiling at local y = 2).
[[nodiscard]] mc::render::RenderMeshData meshStack(mc::world::BlockState current,
                                                   mc::world::BlockState neighbour) {
    mc::world::World world;
    mc::world::Chunk chunk;
    chunk.setState(1, mc::world::kMinY + 1, 1, current);
    chunk.setState(1, mc::world::kMinY + 2, 1, neighbour);
    world.setChunk({0, 0}, std::move(chunk));
    return mc::world::ChunkMesher::buildSection(world, {0, 0}, 0);
}

// RN-8c: the UV a single vertex carries, found by its normal and its exact
// position. Asserts the position is unambiguous, so a moved quad shows up as a
// failure rather than as a silently different vertex.
[[nodiscard]] glm::vec2 uvAt(const mc::render::MeshData& mesh, const glm::vec3& normal,
                             const glm::vec3& position) {
    bool found = false;
    glm::vec2 uv{};
    for (const auto& vertex : mesh.vertices) {
        const auto actual = mc::render::decodeNormal(vertex);
        if (std::abs(actual.x - normal.x) > 0.01F || std::abs(actual.y - normal.y) > 0.01F ||
            std::abs(actual.z - normal.z) > 0.01F) {
            continue;
        }
        const auto p = worldPos(vertex);
        if (std::abs(p.x - position.x) > 0.002F || std::abs(p.y - position.y) > 0.002F ||
            std::abs(p.z - position.z) > 0.002F) {
            continue;
        }
        assert(!found); // the position must name exactly one vertex of this face
        found = true;
        uv = mc::render::decodeUv(vertex);
    }
    assert(found);
    return uv;
}

void expectUv(const mc::render::MeshData& mesh, const glm::vec3& normal,
              const glm::vec3& position, float u, float v, std::string_view context) {
    const glm::vec2 got = uvAt(mesh, normal, position);
    expectNear(got.x, u, context);
    expectNear(got.y, v, context);
}

} // namespace

int main() {
    // The renderer builds the texture-atlas layer table at startup; pin the
    // blocks this test meshes so their resolved layers match the assertions.
    mc::world::setBlockTextureLayers(mc::world::Block::OakLog,
                                     {9.0F, 8.0F, 9.0F});
    mc::world::setBlockTextureLayers(mc::world::Block::OakPlanks,
                                     {7.0F, 7.0F, 7.0F});
    mc::world::Chunk empty;
    assert(mc::world::ChunkMesher::build(empty).empty());

    mc::world::Chunk oneBlock;
    oneBlock.setBlock(1, mc::world::kMinY + 1, 1, mc::world::Block::Grass);
    const auto oneBlockMesh = mc::world::ChunkMesher::build(oneBlock);
    assert(oneBlockMesh.vertices.size() == 24U);
    assert(oneBlockMesh.indices.size() == 36U);

    oneBlock.setBlock(2, mc::world::kMinY + 1, 1, mc::world::Block::Dirt);
    const auto twoBlockMesh = mc::world::ChunkMesher::build(oneBlock);
    assert(twoBlockMesh.vertices.size() == 40U);
    assert(twoBlockMesh.indices.size() == 60U);

    // A truncated block (farmland at 15/16) must not cull its neighbour's face.
    // The dirt keeps its side toward the farmland, or a see-through sliver would
    // appear above the farmland's lower top; the farmland drops only the face
    // directly against the solid dirt. 5 farmland faces + 6 dirt faces = 11.
    mc::world::Chunk farmlandNeighbour;
    farmlandNeighbour.setBlock(1, mc::world::kMinY + 1, 1, mc::world::Block::Farmland);
    farmlandNeighbour.setBlock(2, mc::world::kMinY + 1, 1, mc::world::Block::Dirt);
    const auto farmlandMesh = mc::world::ChunkMesher::build(farmlandNeighbour);
    assert(farmlandMesh.vertices.size() == 44U);
    assert(farmlandMesh.indices.size() == 66U);

    // A crop renders as the vanilla crop.json grid: four orthogonal planes at
    // the quarter offsets (x=4/16, x=12/16, z=4/16, z=12/16), each double-sided
    // — 16 vertices, 48 indices, unlike the two diagonal planes of a `cross`.
    mc::world::Chunk cropChunk;
    cropChunk.setBlock(1, mc::world::kMinY + 1, 1, mc::world::Block::WheatCrops);
    const auto cropMesh = mc::world::ChunkMesher::build(cropChunk);
    assert(cropMesh.vertices.size() == 16U);
    assert(cropMesh.indices.size() == 48U);

    // A lone bottom slab draws all six faces (four half-height sides, the bottom
    // against air and the internal cut on top): 24 vertices like a cube, but its
    // box only reaches halfway, so every vertex sits between y and y + 0.5.
    {
        // Positions decode relative to the section origin (section 0 starts at
        // kMinY), so the cell at kMinY + 1 has its floor one unit up.
        const float floorY = 1.0F;
        mc::world::Chunk slabChunk;
        slabChunk.setState(1, mc::world::kMinY + 1, 1,
                           mc::world::BlockState{mc::world::Block::OakSlab}.withSlabPortion(
                               mc::world::SlabPortion::Bottom));
        const auto bottomMesh = mc::world::ChunkMesher::build(slabChunk);
        assert(bottomMesh.vertices.size() == 24U);
        assert(bottomMesh.indices.size() == 36U);
        float minY = 1e9F;
        float maxY = -1e9F;
        for (const auto& vertex : bottomMesh.vertices) {
            minY = std::min(minY, worldPos(vertex).y);
            maxY = std::max(maxY, worldPos(vertex).y);
        }
        expectNear(minY, floorY, "bottom slab floor");
        expectNear(maxY, floorY + 0.5F, "bottom slab cut");

        // A top slab fills the upper half: floor at y + 0.5, top at y + 1.
        slabChunk.setState(1, mc::world::kMinY + 1, 1,
                           mc::world::BlockState{mc::world::Block::OakSlab}.withSlabPortion(
                               mc::world::SlabPortion::Top));
        const auto topMesh = mc::world::ChunkMesher::build(slabChunk);
        assert(topMesh.vertices.size() == 24U);
        minY = 1e9F;
        maxY = -1e9F;
        for (const auto& vertex : topMesh.vertices) {
            minY = std::min(minY, worldPos(vertex).y);
            maxY = std::max(maxY, worldPos(vertex).y);
        }
        expectNear(minY, floorY + 0.5F, "top slab cut");
        expectNear(maxY, floorY + 1.0F, "top slab top");

        // A double slab is a full cube again, spanning the whole cell.
        slabChunk.setState(1, mc::world::kMinY + 1, 1,
                           mc::world::BlockState{mc::world::Block::OakSlab}.withSlabPortion(
                               mc::world::SlabPortion::Double));
        const auto doubleMesh = mc::world::ChunkMesher::build(slabChunk);
        assert(doubleMesh.vertices.size() == 24U);
        minY = 1e9F;
        maxY = -1e9F;
        for (const auto& vertex : doubleMesh.vertices) {
            minY = std::min(minY, worldPos(vertex).y);
            maxY = std::max(maxY, worldPos(vertex).y);
        }
        expectNear(minY, floorY, "double slab floor");
        expectNear(maxY, floorY + 1.0F, "double slab top");
    }

    mc::world::Chunk sectionBoundary;
    sectionBoundary.setBlock(1, mc::world::kMinY + 15, 1, mc::world::Block::Stone);
    sectionBoundary.setBlock(1, mc::world::kMinY + 16, 1, mc::world::Block::Stone);
    const auto sectionBoundaryMesh = mc::world::ChunkMesher::build(sectionBoundary);
    assert(sectionBoundaryMesh.vertices.size() == 40U);
    assert(sectionBoundaryMesh.indices.size() == 60U);

    mc::world::World boundaryWorld;
    mc::world::Chunk left;
    left.setBlock(15, mc::world::kMinY + 1, 1, mc::world::Block::Stone);
    mc::world::Chunk right;
    right.setBlock(0, mc::world::kMinY + 1, 1, mc::world::Block::Stone);
    boundaryWorld.setChunk({0, 0}, std::move(left));
    boundaryWorld.setChunk({1, 0}, std::move(right));
    const auto leftMesh = mc::world::ChunkMesher::buildSection(boundaryWorld, {0, 0}, 0);
    const auto rightMesh = mc::world::ChunkMesher::buildSection(boundaryWorld, {1, 0}, 0);
    assert(leftMesh.mesh.vertices.size() == 20U);
    assert(leftMesh.mesh.indices.size() == 30U);
    assert(rightMesh.mesh.vertices.size() == 20U);
    assert(rightMesh.mesh.indices.size() == 30U);

    mc::world::World glassWorld;
    mc::world::Chunk glassChunk;
    glassChunk.setBlock(1, mc::world::kMinY + 1, 1, mc::world::Block::Glass);
    glassChunk.setBlock(2, mc::world::kMinY + 1, 1, mc::world::Block::Glass);
    glassWorld.setChunk({0, 0}, std::move(glassChunk));
    const auto glassMesh = mc::world::ChunkMesher::buildSection(glassWorld, {0, 0}, 0);
    assert(glassMesh.mesh.empty());
    assert(glassMesh.translucentMesh.vertices.size() == 40U);
    assert(glassMesh.translucentMesh.indices.size() == 60U);
    static_assert(!mc::world::isOpaque(mc::world::Block::Glass));
    static_assert(mc::world::hasCollision(mc::world::Block::Glass));

    mc::world::World leavesWorld;
    mc::world::Chunk leavesChunk;
    leavesChunk.setBlock(1, mc::world::kMinY + 1, 1, mc::world::Block::OakLeaves);
    leavesChunk.setBlock(2, mc::world::kMinY + 1, 1, mc::world::Block::OakLeaves);
    leavesWorld.setChunk({0, 0}, std::move(leavesChunk));
    const auto leavesMesh =
        mc::world::ChunkMesher::buildSection(leavesWorld, {0, 0}, 0);
    assert(leavesMesh.mesh.empty());
    assert(leavesMesh.cutoutMesh.vertices.size() == 44U);
    assert(leavesMesh.cutoutMesh.indices.size() == 72U);
    static_assert(!mc::world::isOpaque(mc::world::Block::OakLeaves));

    mc::world::World torchWorld;
    mc::world::Chunk torchChunk;
    torchChunk.setBlock(1, mc::world::kMinY + 1, 1, mc::world::Block::Torch);
    torchChunk.setBlock(3, mc::world::kMinY + 1, 1, mc::world::Block::WallTorch);
    torchWorld.setChunk({0, 0}, std::move(torchChunk));
    const auto torchMesh =
        mc::world::ChunkMesher::buildSection(torchWorld, {0, 0}, 0);
    assert(torchMesh.cutoutMesh.vertices.size() == 48U);
    assert(torchMesh.cutoutMesh.indices.size() == 72U);
    constexpr float torchPixel = 1.0F / 16.0F;
    const auto& firstTorchSide = torchMesh.cutoutMesh.vertices;
    expectNear(mc::render::decodeUv(firstTorchSide[8]).x, 7.0F * torchPixel, "torch uv.x 8");
    expectNear(mc::render::decodeUv(firstTorchSide[8]).y, 1.0F, "torch uv.y 8");
    expectNear(mc::render::decodeUv(firstTorchSide[9]).x, 7.0F * torchPixel, "torch uv.x 9");
    expectNear(mc::render::decodeUv(firstTorchSide[9]).y, 6.0F * torchPixel, "torch uv.y 9");
    expectNear(mc::render::decodeUv(firstTorchSide[10]).x, 9.0F * torchPixel, "torch uv.x 10");
    expectNear(mc::render::decodeUv(firstTorchSide[10]).y, 6.0F * torchPixel, "torch uv.y 10");
    expectNear(mc::render::decodeUv(firstTorchSide[11]).x, 9.0F * torchPixel, "torch uv.x 11");
    expectNear(mc::render::decodeUv(firstTorchSide[11]).y, 1.0F, "torch uv.y 11");
    static_assert(!mc::world::hasCollision(mc::world::Block::Torch));
    static_assert(!mc::world::hasCollision(mc::world::Block::WallTorch));
    static_assert(mc::world::emittedLight(mc::world::Block::WallTorch) == 14U);

    // RN-4a follow-up: the furnace is a DirectionalCube whose front faces FACING —
    // the old fixed 167/168 furnace-front layers are gone. Inject known slot
    // layers and confirm an East-facing furnace shows the front on its +X (east)
    // face and the side on a perpendicular (south) face.
    mc::world::setBlockDirectionalLayers(mc::world::Block::Furnace,
                                         {/*front*/ 167.0F, /*frontActive*/ 168.0F, /*back*/ 9.0F,
                                          /*backActive*/ 9.0F, /*top*/ 9.0F, /*bottom*/ 9.0F,
                                          /*side*/ 9.0F});
    mc::world::World directionalWorld;
    mc::world::Chunk directionalChunk;
    directionalChunk.setBlock(1, mc::world::kMinY + 1, 1, mc::world::Block::Furnace);
    directionalChunk.setOrientation(1, mc::world::kMinY + 1, 1, mc::world::BlockOrientation::East);
    directionalChunk.setBlock(3, mc::world::kMinY + 1, 1, mc::world::Block::OakLog);
    directionalChunk.setOrientation(3, mc::world::kMinY + 1, 1, mc::world::BlockOrientation::East);
    directionalWorld.setChunk({0, 0}, std::move(directionalChunk));
    const auto directionalMesh =
        mc::world::ChunkMesher::buildSection(directionalWorld, {0, 0}, 0);
    // Face order is kFaces: +X first (vertices 0-3), +Z fifth (vertices 16-19).
    expectNear(mc::render::decodeTextureLayer(directionalMesh.mesh.vertices[0]), 167.0F,
               "furnace front layer (east face)");
    expectNear(mc::render::decodeTextureLayer(directionalMesh.mesh.vertices[16]), 9.0F,
               "furnace side layer (south face)");
    assert(directionalWorld.orientation(1, mc::world::kMinY + 1, 1) == mc::world::BlockOrientation::East);

    // Horizontal pillar models are baked after rotating the model, leaving UVs
    // attached to their source vertices. For the X-axis log, the local +Z
    // face keeps V along local Y, which has become world X.
    const auto& directionalVertices = directionalMesh.mesh.vertices;
    expectNear(mc::render::decodeTextureLayer(directionalVertices[40]), 8.0F, "log layer");
    expectNear(worldPos(directionalVertices[40]).x, 3.0F, "log -x corner");
    expectNear(worldPos(directionalVertices[43]).x, 4.0F, "log +x corner");
    expectNear(mc::render::decodeUv(directionalVertices[40]).y, 1.0F, "log uv.y 40");
    expectNear(mc::render::decodeUv(directionalVertices[43]).y, 0.0F, "log uv.y 43");

    mc::world::World zAxisLogWorld;
    mc::world::Chunk zAxisLogChunk;
    zAxisLogChunk.setBlock(1, mc::world::kMinY + 1, 1, mc::world::Block::OakLog);
    zAxisLogChunk.setOrientation(1, mc::world::kMinY + 1, 1, mc::world::BlockOrientation::South);
    zAxisLogWorld.setChunk({0, 0}, std::move(zAxisLogChunk));
    const auto zAxisLogMesh =
        mc::world::ChunkMesher::buildSection(zAxisLogWorld, {0, 0}, 0);
    expectNear(mc::render::decodeTextureLayer(zAxisLogMesh.mesh.vertices[0]), 8.0F,
               "z-log layer");
    expectNear(worldPos(zAxisLogMesh.mesh.vertices[0]).z, 1.0F, "z-log -z corner");
    expectNear(worldPos(zAxisLogMesh.mesh.vertices[3]).z, 2.0F, "z-log +z corner");
    expectNear(mc::render::decodeUv(zAxisLogMesh.mesh.vertices[0]).y, 1.0F, "z-log uv.y 0");
    expectNear(mc::render::decodeUv(zAxisLogMesh.mesh.vertices[3]).y, 0.0F, "z-log uv.y 3");

    mc::world::World waterWorld;
    mc::world::Chunk waterChunk;
    waterChunk.setBlock(1, mc::world::kMinY + 1, 1, mc::world::Block::Water);
    waterChunk.setFluidLevel(1, 1, 1, 0U);
    waterChunk.setBlock(2, mc::world::kMinY + 1, 1, mc::world::Block::Water);
    waterChunk.setFluidLevel(2, 1, 1, 7U);
    for (int y = mc::world::kMinY + 1; y <= mc::world::kMinY + 4; ++y) {
        waterChunk.setBlock(4, y, 4, mc::world::Block::Water);
        waterChunk.setFluidLevel(4, y, 4, 0U);
    }
    waterWorld.setChunk({0, 0}, std::move(waterChunk));
    const auto waterMesh = mc::world::ChunkMesher::buildSection(waterWorld, {0, 0}, 0);
    assert(!waterMesh.translucentMesh.empty());
    assert(std::ranges::any_of(
        waterMesh.translucentMesh.vertices,
        [](const auto& vertex) {
            const float localY = worldPos(vertex).y - std::floor(worldPos(vertex).y);
            return localY > 0.01F && localY < 0.99F;
        }));
    assert(std::ranges::any_of(
        waterMesh.translucentMesh.vertices,
        [](const auto& vertex) {
            return mc::render::decodeTextureLayer(vertex) == 32.0F;
        }));
    assert(std::ranges::any_of(
        waterMesh.translucentMesh.vertices,
        [](const auto& vertex) {
            return mc::render::decodeTextureLayer(vertex) == 0.0F &&
                mc::render::decodeWaterDepth(vertex) >= 4.0F;
        }));

    // A water body reaching a chunk border stays flat while the neighbour chunk
    // has not streamed in yet: a missing neighbour must not dip the border
    // corners into a straight crack (and the whole row onto the flowing
    // texture), the exact "long straight crack / same-direction flowing water"
    // symptom in large lakes.
    {
        mc::world::World borderWorld;
        mc::world::Chunk borderChunk;
        // A 6x8 level-0 pool whose east column (x=15) touches the +x border
        // (x=16) of chunk (0,0); chunk (1,0) is deliberately absent. Interior
        // cells have every z-neighbour filled, so only the missing chunk can
        // influence their border corners.
        for (int z = 4; z <= 11; ++z) {
            for (int x = 10; x <= 15; ++x) {
                borderChunk.setBlock(x, mc::world::kMinY + 1, z, mc::world::Block::Water);
                borderChunk.setFluidLevel(x, mc::world::kMinY + 1, z, 0U);
            }
        }
        borderWorld.setChunk({0, 0}, std::move(borderChunk));
        const auto borderMesh =
            mc::world::ChunkMesher::buildSection(borderWorld, {0, 0}, 0);
        // Every vertex on the seam (x=16, inside the missing chunk) at surface
        // height — the border column's top corners and the side-face tops that
        // share them — must sit at the flat 8/9 height. Without the fix they
        // sample Air and collapse to ~1.81, cracking the row.
        int seamSurfaceVertices = 0;
        for (const auto& vertex : borderMesh.translucentMesh.vertices) {
            const auto position = worldPos(vertex);
            if (position.x < 15.9F || position.x > 16.1F ||
                position.z < 6.0F - 1e-3F || position.z > 9.0F + 1e-3F ||
                position.y <= 1.5F) {
                continue;
            }
            ++seamSurfaceVertices;
            expectNear(position.y, 1.0F + 8.0F / 9.0F, "border water surface");
        }
        // The four border cells (x=15, z=6..9) contribute their x=16 top and
        // side-top corners here; far more than a couple, so the check is not
        // vacuous.
        assert(seamSurfaceVertices >= 6);
        // The border cells must NOT render their +X side faces toward the
        // missing chunk: that would draw the waterfall-like vertical cut from
        // the surface to the seabed along the seam. No translucent vertex on
        // the missing-chunk side may carry a horizontal face normal.
        for (const auto& vertex : borderMesh.translucentMesh.vertices) {
            const auto position = worldPos(vertex);
            const auto normal = mc::render::decodeNormal(vertex);
            if (position.x < 15.9F || position.x > 16.1F) {
                continue;
            }
            assert(std::abs(normal.x) < 0.5F);
        }
    }

    mc::world::World negativeWorld;
    mc::world::Chunk negativeChunk;
    negativeChunk.setBlock(15, mc::world::kMinY + 3, 15, mc::world::Block::Sand);
    negativeWorld.setChunk({-1, -1}, std::move(negativeChunk));
    assert(negativeWorld.block(-1, mc::world::kMinY + 3, -1) == mc::world::Block::Sand);
    assert(negativeWorld.block(-16, mc::world::kMinY + 3, -16) == mc::world::Block::Air);

    const mc::world::SurfaceGenerator generator{0x5EEDULL};
    const auto first = generator.generate(0, 0);
    const auto repeated = generator.generate(0, 0);
    for (int z = 0; z < mc::world::kChunkDepth; ++z) {
        for (int x = 0; x < mc::world::kChunkWidth; ++x) {
            assert(first.block(x, mc::world::kMinY, z) == mc::world::Block::Bedrock);
            for (int y = mc::world::kMinY; y < mc::world::kMaxY; ++y) {
                assert(first.block(x, y, z) == repeated.block(x, y, z));
            }
        }
    }
    assert(!mc::world::ChunkMesher::build(first).empty());

    // The generator lays oceans and forests out over a whole continent, so the
    // sample has to be wider than one chunk's neighbourhood to be sure of
    // crossing both.
    bool foundGeneratedWater = false;
    bool foundGeneratedVegetation = false;
    for (int chunkZ = -12; chunkZ <= 12; chunkZ += 3) {
        for (int chunkX = -12; chunkX <= 12; chunkX += 3) {
            const auto generated = generator.generate(chunkX, chunkZ);
            for (int z = 0; z < mc::world::kChunkDepth; ++z) {
                for (int x = 0; x < mc::world::kChunkWidth; ++x) {
                    for (int y = 1; y < 90; ++y) {
                        const auto block = generated.block(x, y, z);
                        foundGeneratedWater |= block == mc::world::Block::Water;
                        foundGeneratedVegetation |=
                            block == mc::world::Block::GrassPlant ||
                            block == mc::world::Block::Dandelion ||
                            mc::world::isLeaves(block);
                    }
                }
            }
        }
    }
    assert(foundGeneratedWater);
    assert(foundGeneratedVegetation);

    // AbstractBlock#getModelOffset: OffsetType.XZ plants are baked a few pixels
    // off their block centre, with the jitter taken from MathHelper.hashCode
    // so it is deterministic per position but looks random across a field.
    // The reference values below were taken from the vanilla Java source.
    mc::world::World plantWorld;
    mc::world::Chunk plantChunk;
    plantChunk.setBlock(0, mc::world::kMinY + 0, 0, mc::world::Block::GrassPlant);
    plantChunk.setBlock(3, mc::world::kMinY + 1, 5, mc::world::Block::Dandelion);
    plantChunk.setBlock(8, mc::world::kMinY + 2, 8, mc::world::Block::OakSapling);
    plantWorld.setChunk({0, 0}, std::move(plantChunk));
    const auto plantMesh = mc::world::ChunkMesher::buildSection(plantWorld, {0, 0}, 0);
    const auto& plantVertices = plantMesh.cutoutMesh.vertices;
    assert(plantVertices.size() == 24U);  // three cross plants, eight vertices each
    // GrassPlant at (0,0,0): hash(0,0,0) -> 0, so the cross slides -0.25 in x
    // and z. Its first vertex sits at origin + offset + corner(0,0,0).
    expectNear(worldPos(plantVertices[0]).x, -0.25F, "grass x");
    expectNear(worldPos(plantVertices[0]).z, -0.25F, "grass z");
    expectNear(worldPos(plantVertices[1]).x, 0.75F, "grass +x corner");
    expectNear(worldPos(plantVertices[1]).z, 0.75F, "grass +z corner");
    // Dandelion at (3,1,5): different hash bits, different magnitude and
    // direction — here +1/12 in x and -11/60 in z.
    expectNear(worldPos(plantVertices[8]).x, 3.0F + 0.083333F, "dandelion x");
    expectNear(worldPos(plantVertices[8]).z, 5.0F - 0.183333F, "dandelion z");
    // Saplings keep OffsetType.None, so they stay pinned to the block centre.
    expectNear(worldPos(plantVertices[16]).x, 8.0F, "sapling x");
    expectNear(worldPos(plantVertices[16]).z, 8.0F, "sapling z");
    expectNear(worldPos(plantVertices[16]).y, 2.0F, "sapling y");

    // RN-2: every shaped block (stairs/door/fence-gate/trapdoor/button/pressure
    // plate/wall) now meshes from the one `BlockShape` box set the pick ray and
    // collision already read, instead of the old full-cube fallthrough. A lone
    // shaped block sits on air, so no face culls: each box draws all six faces
    // (24 vertices, 36 indices), so the mesh has exactly 24 vertices per box in
    // the state's BlockShape. This pins the mesh geometry to the single source —
    // if a box is dropped, or the block falls back to a single full cube, the
    // per-box count no longer matches `blockShape(state).boxes.size()`.
    const auto shapedVertexBudget = [](const mc::world::BlockState& state) -> std::size_t {
        const auto shape = mc::world::blockShape(state);
        if (shape.kind == mc::world::ShapeKind::Column) {
            return 24U; // one full-footprint Y box (pressure plate)
        }
        return 24U * shape.boxes.size();
    };
    {
        // A straight stair is two boxes (full bottom half + one step), so its
        // mesh is 48 vertices — provably not the 24-vertex single cube the old
        // fallthrough produced.
        mc::world::Chunk stairChunk;
        const mc::world::BlockState stair{mc::world::Block::OakStairs};
        stairChunk.setState(1, mc::world::kMinY + 1, 1, stair);
        const auto stairMesh = mc::world::ChunkMesher::build(stairChunk);
        assert(mc::world::blockShape(stair).boxes.size() == 2U);
        assert(stairMesh.vertices.size() == shapedVertexBudget(stair));
        assert(stairMesh.vertices.size() == 48U);
        assert(stairMesh.indices.size() == 72U);
        assert(stairMesh.indices.size() % 3U == 0U);
        // The step box reaches above y+0.5, and the bottom box floors at y, so
        // the mesh is a real partial shape spanning the whole cell height, not a
        // flat half.
        float minY = 1e9F;
        float maxY = -1e9F;
        for (const auto& vertex : stairMesh.vertices) {
            minY = std::min(minY, worldPos(vertex).y);
            maxY = std::max(maxY, worldPos(vertex).y);
        }
        expectNear(minY, 1.0F, "stair floor");
        expectNear(maxY, 2.0F, "stair top");
    }
    {
        // An inner-shape stair is three boxes -> 72 vertices, catching a dropped
        // third box that a two-box straight stair could not.
        mc::world::Chunk innerStairChunk;
        const auto inner =
            mc::world::BlockState{mc::world::Block::OakStairs}.withStairShape(
                mc::world::StairShape::InnerRight);
        innerStairChunk.setState(1, mc::world::kMinY + 1, 1, inner);
        const auto innerMesh = mc::world::ChunkMesher::build(innerStairChunk);
        assert(mc::world::blockShape(inner).boxes.size() == 3U);
        assert(innerMesh.vertices.size() == shapedVertexBudget(inner));
        assert(innerMesh.vertices.size() == 72U);
    }
    {
        // RN-10b: a door leaf now draws from its baked vanilla model, which
        // declares FIVE faces, not six — door_bottom_left.json has no `up` face,
        // because the upper half of the door is standing on it. So 20 vertices,
        // where the BlockShape box path drew 24 and put a quad inside the door.
        // The geometry is otherwise identical: the leaf still occupies exactly
        // the shape's box, which is what makes this a face-count change and not
        // a model change (shaped_block_model_test pins bounds == shape for all
        // 48 variants; here it is checked once through the real mesher).
        mc::world::Chunk doorChunk;
        const mc::world::BlockState door{mc::world::Block::OakDoor};
        doorChunk.setState(3, mc::world::kMinY + 1, 1, door);
        const auto doorMesh = mc::world::ChunkMesher::build(doorChunk);
        assert(mc::world::blockShape(door).boxes.size() == 1U);
        assert(doorMesh.vertices.size() == 20U);
        // The upper half caps up instead of down, and is likewise five faces.
        mc::world::Chunk upperChunk;
        upperChunk.setState(3, mc::world::kMinY + 1, 1,
                            mc::world::BlockState{mc::world::Block::OakDoor}.withDoorUpperHalf(
                                true));
        assert(mc::world::ChunkMesher::build(upperChunk).vertices.size() == 20U);
        // A trapdoor keeps all six (template_trapdoor_bottom.json declares six).
        mc::world::Chunk trapChunk;
        trapChunk.setState(3, mc::world::kMinY + 1, 1,
                           mc::world::BlockState{mc::world::Block::OakTrapdoor});
        assert(mc::world::ChunkMesher::build(trapChunk).vertices.size() == 24U);
        // The leaf is 3/16 thin on Z (0.8125..1), so every vertex's Z lies in
        // that band — not the full 0..1 of a cube.
        float minZ = 1e9F;
        float maxZ = -1e9F;
        for (const auto& vertex : doorMesh.vertices) {
            minZ = std::min(minZ, worldPos(vertex).z - 1.0F);
            maxZ = std::max(maxZ, worldPos(vertex).z - 1.0F);
        }
        expectNear(minZ, 0.8125F, "door leaf near");
        expectNear(maxZ, 1.0F, "door leaf far");
    }
    {
        // RN-6: the fence gate now meshes its real two-post-and-bars geometry —
        // eight boxes, so 8 * 6 faces * 4 = 192 vertices — instead of the old
        // single plank-wall box. Its outline / pick SHAPE stays the one post-pair
        // box (vanilla getShape ignores OPEN — the gate stays visible and
        // selectable), and only its *collision* shape empties when open so entities
        // pass through.
        mc::world::Chunk openGateChunk;
        const auto openGate =
            mc::world::BlockState{mc::world::Block::OakFenceGate}.withOpen(true);
        openGateChunk.setState(5, mc::world::kMinY + 1, 1, openGate);
        const auto openGateMesh = mc::world::ChunkMesher::build(openGateChunk);
        assert(mc::world::blockShape(openGate).boxes.size() == 1U);
        assert(openGateMesh.vertices.size() == 192U);
        assert(mc::world::collisionShape(openGate).boxes.empty());
        // A closed gate meshes the same eight-box geometry, and (unlike open) it
        // still collides.
        const mc::world::BlockState closedGate{mc::world::Block::OakFenceGate};
        openGateChunk.setState(5, mc::world::kMinY + 1, 1, closedGate);
        const auto closedGateMesh = mc::world::ChunkMesher::build(openGateChunk);
        assert(mc::world::blockShape(closedGate).boxes.size() == 1U);
        assert(closedGateMesh.vertices.size() == 192U);
        assert(mc::world::collisionShape(closedGate).boxes.size() == 1U);
    }
    {
        // A pressure plate is a Column shape (thin full-footprint box), meshed
        // through the slab-style Y-box path: 24 vertices, and its top sits at
        // 1/16 rather than a full cube's 1.
        mc::world::Chunk plateChunk;
        const mc::world::BlockState plate{mc::world::Block::StonePressurePlate};
        plateChunk.setState(7, mc::world::kMinY + 1, 1, plate);
        const auto plateMesh = mc::world::ChunkMesher::build(plateChunk);
        assert(mc::world::blockShape(plate).kind == mc::world::ShapeKind::Column);
        assert(plateMesh.vertices.size() == 24U);
        float maxY = -1e9F;
        for (const auto& vertex : plateMesh.vertices) {
            maxY = std::max(maxY, worldPos(vertex).y);
        }
        expectNear(maxY, 1.0F + 1.0F / 16.0F, "pressure plate top");
    }
    {
        // A connected wall is a post plus one arm per side. With one connection
        // its shape is two boxes -> 48 vertices, tying the mesh to the wall's
        // connection-mask box set.
        mc::world::Chunk wallChunk;
        const auto wall =
            mc::world::BlockState{mc::world::Block::CobblestoneWall}.withWallConnected(
                mc::world::BlockOrientation::North, true);
        wallChunk.setState(9, mc::world::kMinY + 1, 1, wall);
        const auto wallMesh = mc::world::ChunkMesher::build(wallChunk);
        assert(mc::world::blockShape(wall).boxes.size() == 2U);
        assert(wallMesh.vertices.size() == shapedVertexBudget(wall));
        assert(wallMesh.vertices.size() == 48U);
    }

    // RN-2 HUD icon routing (the icon half of审计 #1), tested through the
    // Vulkan-free predicates the HudRenderer's drawHudItemIcon consumes: a
    // shaped block that is not a thin leaf draws a 3D block icon; a door or
    // trapdoor stays a flat item sprite, matching vanilla's per-item render.
    static_assert(mc::world::isShapedBlockModel(mc::world::BlockModel::Stairs));
    static_assert(mc::world::isShapedBlockModel(mc::world::BlockModel::Wall));
    static_assert(mc::world::isShapedBlockModel(mc::world::BlockModel::FenceGate));
    static_assert(mc::world::isShapedBlockModel(mc::world::BlockModel::Button));
    static_assert(mc::world::isShapedBlockModel(mc::world::BlockModel::PressurePlate));
    static_assert(mc::world::isShapedBlockModel(mc::world::BlockModel::Door));
    static_assert(mc::world::isShapedBlockModel(mc::world::BlockModel::TrapDoor));
    static_assert(!mc::world::isShapedBlockModel(mc::world::BlockModel::Cube));
    static_assert(!mc::world::isShapedBlockModel(mc::world::BlockModel::Slab));
    static_assert(!mc::world::isShapedBlockModel(mc::world::BlockModel::Chest));
    // Only door and trapdoor are drawn flat; the rest show a 3D block icon.
    static_assert(mc::world::isThinLeafIconModel(mc::world::BlockModel::Door));
    static_assert(mc::world::isThinLeafIconModel(mc::world::BlockModel::TrapDoor));
    static_assert(!mc::world::isThinLeafIconModel(mc::world::BlockModel::Stairs));
    static_assert(!mc::world::isThinLeafIconModel(mc::world::BlockModel::Wall));
    static_assert(!mc::world::isThinLeafIconModel(mc::world::BlockModel::FenceGate));
    static_assert(!mc::world::isThinLeafIconModel(mc::world::BlockModel::Button));
    static_assert(!mc::world::isThinLeafIconModel(mc::world::BlockModel::PressurePlate));

    {
        // Door texture is chosen by HALF, not by geometric face: the upper cell
        // shows oak_door_top, the lower oak_door_bottom. Before the fix both
        // halves read the "side" slot, so the upper half repeated the bottom
        // sprite (缺失上半纹理). The atlas is not baked headless (layers default
        // to 0), so inject two distinct sprite layers for the door, then confirm
        // the upper cell meshes with the top slot and the lower with the side.
        mc::world::setBlockTextureLayers(mc::world::Block::OakDoor,
            mc::world::BlockTextureLayers{/*top*/ 11.0F, /*side*/ 22.0F, /*bottom*/ 22.0F});
        const auto& doorLayers = mc::world::textureLayers(mc::world::Block::OakDoor);
        assert(doorLayers.top != doorLayers.side); // the two door sprites are distinct

        mc::world::Chunk lowerChunk;
        lowerChunk.setState(6, mc::world::kMinY + 1, 6,
            mc::world::BlockState{mc::world::Block::OakDoor}.withDoorUpperHalf(false));
        const auto lowerMesh = mc::world::ChunkMesher::build(lowerChunk);
        assert(!lowerMesh.vertices.empty());
        for (const auto& vertex : lowerMesh.vertices) {
            assert(static_cast<float>(vertex.textureLayer) == doorLayers.side);
        }

        mc::world::Chunk upperChunk;
        upperChunk.setState(6, mc::world::kMinY + 1, 6,
            mc::world::BlockState{mc::world::Block::OakDoor}.withDoorUpperHalf(true));
        const auto upperMesh = mc::world::ChunkMesher::build(upperChunk);
        assert(!upperMesh.vertices.empty());
        for (const auto& vertex : upperMesh.vertices) {
            assert(static_cast<float>(vertex.textureLayer) == doorLayers.top);
        }
    }

    // RN-4 N2b: an ElementModel block (repeater) is meshed by wiring the mesher to
    // the shared FaceBakery baker. This end-to-end check locks the wiring itself —
    // the real mesh's vertex world-positions equal bakeElementModel's quad positions
    // shifted into the cell, so every element face is emitted and translated
    // correctly. (The UV orientation now follows the faithful-vanilla convention and
    // is verified visually on Mac, not here; N2a locks the baker's own geometry/UV.)
    {
        using mc::world::Block;
        using mc::world::BlockOrientation;
        using mc::world::BlockState;
        const BlockState repeater =
            BlockState{Block::Repeater, BlockOrientation::East}.withRepeaterDelay(2);
        mc::world::Chunk repeaterChunk;
        repeaterChunk.setState(3, mc::world::kMinY + 1, 4, repeater);
        const auto repeaterMesh = mc::world::ChunkMesher::build(repeaterChunk);

        const auto bakedQuads = mc::world::bake::bakeElementModel(Block::Repeater, repeater);
        assert(repeaterMesh.vertices.size() == bakedQuads.size() * 4U);

        // Section 0's origin is the world origin at kMinY, so decoded local positions
        // are cell-local + the cell offset (cell y = kMinY + 1 -> local y = 1).
        const glm::vec3 cell{3.0F, 1.0F, 4.0F};
        std::vector<glm::vec3> meshPositions;
        for (const auto& vertex : repeaterMesh.vertices) {
            meshPositions.push_back(worldPos(vertex));
        }
        for (const auto& baked : bakedQuads) {
            for (const auto& corner : baked.quad.position) {
                const glm::vec3 want = cell + corner;
                bool found = false;
                for (const auto& got : meshPositions) {
                    if (std::abs(got.x - want.x) < 0.002F && std::abs(got.y - want.y) < 0.002F &&
                        std::abs(got.z - want.z) < 0.002F) {
                        found = true;
                        break;
                    }
                }
                assert(found);
            }
        }
    }

    // RN-10b: the same end-to-end wiring lock for the door and the trapdoor, now
    // that they draw from the baked store rather than from their BlockShape box.
    // Position AND uv are compared against the baker, because the whole point of
    // migrating them is the uv: a mesh that kept the old projected rects would
    // pass a position-only check while still drawing R1/R2/R4/R6.
    {
        using mc::world::Block;
        using mc::world::BlockOrientation;
        using mc::world::BlockState;
        using mc::world::DoorHinge;
        struct Case final {
            Block block;
            BlockState state;
            int cx, cz;
        };
        const std::array<Case, 4> cases{{
            {Block::OakDoor,
             BlockState{Block::OakDoor, BlockOrientation::East}.withHinge(DoorHinge::Right), 2, 2},
            {Block::OakDoor,
             BlockState{Block::OakDoor, BlockOrientation::North}.withOpen(true).withDoorUpperHalf(
                 true),
             4, 2},
            {Block::OakTrapdoor, BlockState{Block::OakTrapdoor, BlockOrientation::West}, 6, 2},
            {Block::OakTrapdoor,
             BlockState{Block::OakTrapdoor, BlockOrientation::South}.withOpen(true), 8, 2},
        }};
        for (const Case& c : cases) {
            mc::world::Chunk chunk;
            chunk.setState(c.cx, mc::world::kMinY + 1, c.cz, c.state);
            const auto mesh = mc::world::ChunkMesher::build(chunk);
            const auto baked = mc::world::bake::bakedElementModel(c.block, c.state);
            assert(!baked.empty());
            assert(mesh.vertices.size() == baked.size() * 4U);
            const glm::vec3 cell{static_cast<float>(c.cx), 1.0F, static_cast<float>(c.cz)};
            for (const auto& quad : baked) {
                for (std::size_t i = 0; i < 4U; ++i) {
                    const glm::vec3 wantPosition = cell + quad.quad.position[i];
                    const glm::vec2 wantUv = quad.quad.uv[i];
                    bool found = false;
                    for (const auto& vertex : mesh.vertices) {
                        const glm::vec3 got = worldPos(vertex);
                        if (std::abs(got.x - wantPosition.x) < 0.002F &&
                            std::abs(got.y - wantPosition.y) < 0.002F &&
                            std::abs(got.z - wantPosition.z) < 0.002F &&
                            std::abs(mc::render::decodeUv(vertex).x - wantUv.x) < 0.002F &&
                            std::abs(mc::render::decodeUv(vertex).y - wantUv.y) < 0.002F) {
                            found = true;
                            break;
                        }
                    }
                    assert(found);
                }
            }
        }
    }

    // RN-10a/10b: the model's `ambientocclusion` bit, checked where it is
    // actually observable — a leaf standing in a doorway, with stone on both
    // sides and above. On a lone block every corner samples air and every AO
    // value is 1.0, so a bench without the frame proves nothing about the bit.
    //
    // A door declares false (door_bottom_left.json) and must come out flat; the
    // stair beside it declares nothing, inherits true, and must come out shaded.
    // The control is what makes this a test of the bit rather than of the bench.
    {
        using mc::world::Block;
        using mc::world::BlockState;
        // The leaf is picked out of the mesh by its atlas layer, not by position:
        // the frame's own quads touch the very corners the leaf stands on, so a
        // positional filter reads stone vertices as the door's and the test says
        // nothing. Layers are 0 headless, so two distinct ones are injected.
        mc::world::setBlockTextureLayers(Block::OakDoor,
                                         mc::world::BlockTextureLayers{11.0F, 22.0F, 22.0F});
        mc::world::setBlockTextureLayers(Block::OakStairs,
                                         mc::world::BlockTextureLayers{33.0F, 33.0F, 33.0F});
        const auto framed = [](BlockState leaf, float leafLayer) {
            mc::world::Chunk chunk;
            const int y = mc::world::kMinY + 1;
            chunk.setState(4, y, 4, leaf);
            // A real doorway: jambs two deep on both sides, so the ring of cells
            // an AO corner samples around the leaf's large faces is occupied. A
            // frame only in the leaf's own z-plane leaves every one of those
            // samples on air and shades nothing, which is the version of this
            // bench that quietly passed for the wrong reason.
            for (const int jambZ : {3, 4, 5}) {
                chunk.setState(3, y, jambZ, BlockState{Block::Stone});
                chunk.setState(5, y, jambZ, BlockState{Block::Stone});
            }
            chunk.setState(4, y - 1, 4, BlockState{Block::Stone});
            const auto mesh = mc::world::ChunkMesher::build(chunk);
            std::uint8_t darkest = 255U;
            std::size_t seen = 0;
            for (const auto& vertex : mesh.vertices) {
                if (std::abs(static_cast<float>(vertex.textureLayer) - leafLayer) > 0.5F) {
                    continue;
                }
                ++seen;
                darkest = std::min(darkest, vertex.ambientOcclusion);
            }
            assert(seen > 0U); // the bench must actually contain the leaf
            return darkest;
        };
        // Control first: the same frame does darken something that opts in.
        assert(framed(BlockState{Block::OakStairs}, 33.0F) < 255U);
        // The door does not.
        assert(framed(BlockState{Block::OakDoor}, 22.0F) == 255U);
        assert(framed(BlockState{Block::OakDoor}.withDoorUpperHalf(true), 11.0F) == 255U);
    }

    // --- RN-8c: shaped blocks take their UV from the bakery's rect rules --------
    //
    // A shaped block's faces used to derive their UV from the vertex position
    // inline (u along the horizontal axis, v measured down from the cell top).
    // That agreed with vanilla on -X, +Z and -Y and disagreed on the other three.
    // Every value below is hand-computed from JE's FaceBakery.defaultFaceUV for
    // the box the block actually occupies, sampled through FaceInfo — NOT read
    // back out of the mesher.
    //
    // The block is a bottom slab at cell (1, kMinY+1, 1), so its box is
    // from16 = (0,0,0), to16 = (16,8,16) and its world span is x 1..2, y 1..1.5,
    // z 1..2.
    {
        using mc::world::Block;
        using mc::world::BlockState;
        using mc::world::SlabPortion;
        mc::world::World world;
        mc::world::Chunk chunk;
        chunk.setState(1, mc::world::kMinY + 1, 1,
                       BlockState{Block::OakSlab}.withSlabPortion(SlabPortion::Bottom));
        world.setChunk({0, 0}, std::move(chunk));
        const auto& slab = mc::world::ChunkMesher::buildSection(world, {0, 0}, 0).mesh;

        // Up: defaultFaceUV = (from.x, from.z, to.x, to.z) = (0,0,16,16), so
        // u = x and v = z across the cell. This is the face that used to come out
        // V-mirrored.
        constexpr glm::vec3 up{0.0F, 1.0F, 0.0F};
        expectUv(slab, up, {1.0F, 1.5F, 1.0F}, 0.0F, 0.0F, "slab top uv at (x0,z0)");
        expectUv(slab, up, {1.0F, 1.5F, 2.0F}, 0.0F, 1.0F, "slab top uv at (x0,z1)");
        expectUv(slab, up, {2.0F, 1.5F, 2.0F}, 1.0F, 1.0F, "slab top uv at (x1,z1)");
        expectUv(slab, up, {2.0F, 1.5F, 1.0F}, 1.0F, 0.0F, "slab top uv at (x1,z0)");

        // Down: (from.x, 16-to.z, to.x, 16-from.z) = (0,0,16,16) -> u = x,
        // v = 1-z. Unchanged by RN-8c; here as the control that the rect rules
        // reproduce what was already right.
        constexpr glm::vec3 down{0.0F, -1.0F, 0.0F};
        expectUv(slab, down, {1.0F, 1.0F, 2.0F}, 0.0F, 0.0F, "slab bottom uv at (x0,z1)");
        expectUv(slab, down, {2.0F, 1.0F, 1.0F}, 1.0F, 1.0F, "slab bottom uv at (x1,z0)");

        // East: (16-to.z, 16-to.y, 16-from.z, 16-from.y) = (0, 8, 16, 16), so the
        // rect is the sprite's LOWER half (v 0.5..1, the slab is the cell's lower
        // half) and u runs 0 at z=1 to 1 at z=0. The u direction is what used to
        // be mirrored.
        constexpr glm::vec3 east{1.0F, 0.0F, 0.0F};
        expectUv(slab, east, {2.0F, 1.0F, 2.0F}, 0.0F, 1.0F, "slab +x uv at (z1,bottom)");
        expectUv(slab, east, {2.0F, 1.0F, 1.0F}, 1.0F, 1.0F, "slab +x uv at (z0,bottom)");
        expectUv(slab, east, {2.0F, 1.5F, 1.0F}, 1.0F, 0.5F, "slab +x uv at (z0,top)");
        expectUv(slab, east, {2.0F, 1.5F, 2.0F}, 0.0F, 0.5F, "slab +x uv at (z1,top)");

        // West: (from.z, 16-to.y, to.z, 16-from.y) = (0, 8, 16, 16) -> u runs 0 at
        // z=0 to 1 at z=1, the opposite way round from +X, which is exactly the
        // asymmetry one shared formula could not express. Also unchanged.
        constexpr glm::vec3 west{-1.0F, 0.0F, 0.0F};
        expectUv(slab, west, {1.0F, 1.0F, 1.0F}, 0.0F, 1.0F, "slab -x uv at (z0,bottom)");
        expectUv(slab, west, {1.0F, 1.5F, 2.0F}, 1.0F, 0.5F, "slab -x uv at (z1,top)");

        // North: (16-to.x, 16-to.y, 16-from.x, 16-from.y) = (0, 8, 16, 16) -> u
        // runs 0 at x=1 to 1 at x=0. The second face that used to be mirrored.
        constexpr glm::vec3 north{0.0F, 0.0F, -1.0F};
        expectUv(slab, north, {2.0F, 1.0F, 1.0F}, 0.0F, 1.0F, "slab -z uv at (x1,bottom)");
        expectUv(slab, north, {1.0F, 1.0F, 1.0F}, 1.0F, 1.0F, "slab -z uv at (x0,bottom)");

        // South: (from.x, 16-to.y, to.x, 16-from.y) = (0, 8, 16, 16) -> u = x.
        // Unchanged.
        constexpr glm::vec3 south{0.0F, 0.0F, 1.0F};
        expectUv(slab, south, {1.0F, 1.0F, 2.0F}, 0.0F, 1.0F, "slab +z uv at (x0,bottom)");
        expectUv(slab, south, {2.0F, 1.0F, 2.0F}, 1.0F, 1.0F, "slab +z uv at (x1,bottom)");
    }

    // --- RN-8c: a cube's UV comes from its model json, baked with FACING -------
    //
    // Every number below is hand-derived from the vanilla model json through JE's
    // FaceBakery rules (defaultFaceUV / an explicit rect, sampled in FaceInfo
    // order, with the blockstate rotation applied to the vertices), NOT read back
    // out of the mesher.
    {
        using mc::world::Block;
        using mc::world::BlockOrientation;
        using mc::world::BlockState;
        const auto meshOf = [](BlockState state) {
            mc::world::World world;
            mc::world::Chunk chunk;
            chunk.setState(1, mc::world::kMinY + 1, 1, state);
            world.setChunk({0, 0}, std::move(chunk));
            return mc::world::ChunkMesher::buildSection(world, {0, 0}, 0);
        };
        constexpr glm::vec3 up{0.0F, 1.0F, 0.0F};
        constexpr glm::vec3 down{0.0F, -1.0F, 0.0F};
        constexpr glm::vec3 north{0.0F, 0.0F, -1.0F};
        constexpr glm::vec3 east{1.0F, 0.0F, 0.0F};

        // A plain cube: block/cube gives every face uv [0,0,16,16] with no
        // rotation, so defaultFaceUV's up rule (from.x, from.z, to.x, to.z) makes
        // the top read u = x, v = z. The old kUvs assignment gave u = z, v = 1-x —
        // the top face turned a quarter turn, which is invisible on stone and
        // plain on a crafting table or a hay bale.
        {
            const auto& cube = meshOf(BlockState{Block::Stone}).mesh;
            expectUv(cube, up, {1.0F, 2.0F, 1.0F}, 0.0F, 0.0F, "cube top uv (x0,z0)");
            expectUv(cube, up, {1.0F, 2.0F, 2.0F}, 0.0F, 1.0F, "cube top uv (x0,z1)");
            expectUv(cube, up, {2.0F, 2.0F, 2.0F}, 1.0F, 1.0F, "cube top uv (x1,z1)");
            expectUv(cube, up, {2.0F, 2.0F, 1.0F}, 1.0F, 0.0F, "cube top uv (x1,z0)");
            // Down: (from.x, 16-to.z, to.x, 16-from.z) -> u = x, v = 1-z.
            expectUv(cube, down, {1.0F, 1.0F, 2.0F}, 0.0F, 0.0F, "cube bottom uv (x0,z1)");
            expectUv(cube, down, {2.0F, 1.0F, 1.0F}, 1.0F, 1.0F, "cube bottom uv (x1,z0)");
            // A side is unchanged: north reads u = 1-x, v = 1-y.
            expectUv(cube, north, {2.0F, 1.0F, 1.0F}, 0.0F, 1.0F, "cube -z uv (x1,bottom)");
            expectUv(cube, north, {1.0F, 2.0F, 1.0F}, 1.0F, 0.0F, "cube -z uv (x0,top)");
        }

        // The observer, facing north — vanilla's identity variant
        // (blockstates/observer.json has no rotation on facing=north), so its
        // faces come out of the model unturned. This is what the old empirical
        // anchor got wrong: it pinned facing=up as the zero-turn orientation, so
        // every other facing, the identity one included, was a quarter turn off.
        {
            const auto& observer = meshOf(BlockState{Block::Observer,
                                                     BlockOrientation::North}).mesh;
            expectUv(observer, north, {2.0F, 1.0F, 1.0F}, 0.0F, 1.0F, "observer front (x1,bot)");
            expectUv(observer, north, {1.0F, 1.0F, 1.0F}, 1.0F, 1.0F, "observer front (x0,bot)");
            expectUv(observer, north, {1.0F, 2.0F, 1.0F}, 1.0F, 0.0F, "observer front (x0,top)");
            // observer.json's up face declares "uv": [0,16,16,0] — V runs
            // backwards, so the top reads u = x, v = 1-z where a plain cube reads
            // v = z. This is the registered "observer top uv-rect flipped" defect,
            // and no per-face quarter-turn count can express it.
            expectUv(observer, up, {1.0F, 2.0F, 1.0F}, 0.0F, 1.0F, "observer top (x0,z0)");
            expectUv(observer, up, {1.0F, 2.0F, 2.0F}, 0.0F, 0.0F, "observer top (x0,z1)");
            expectUv(observer, up, {2.0F, 2.0F, 2.0F}, 1.0F, 0.0F, "observer top (x1,z1)");
            expectUv(observer, up, {2.0F, 2.0F, 1.0F}, 1.0F, 1.0F, "observer top (x1,z0)");
        }

        // The piston, facing north: template_piston.json puts "rotation": 90 on
        // its east face, which is a Quadrant, i.e. a cyclic shift of the sampled
        // UV corner. Vertex (x1,y0,z1) samples the rect corner one step on.
        {
            const auto& piston = meshOf(BlockState{Block::Piston,
                                                   BlockOrientation::North}).mesh;
            expectUv(piston, east, {2.0F, 1.0F, 2.0F}, 1.0F, 1.0F, "piston +x uv (z1,bot)");
            expectUv(piston, east, {2.0F, 1.0F, 1.0F}, 1.0F, 0.0F, "piston +x uv (z0,bot)");
            expectUv(piston, east, {2.0F, 2.0F, 1.0F}, 0.0F, 0.0F, "piston +x uv (z0,top)");
            expectUv(piston, east, {2.0F, 2.0F, 2.0F}, 0.0F, 1.0F, "piston +x uv (z1,top)");
            // Its north face declares no rotation, so it reads like a plain cube's.
            expectUv(piston, north, {2.0F, 1.0F, 1.0F}, 0.0F, 1.0F, "piston -z uv (x1,bot)");
        }

        // Rotating the block rotates its UVs rigidly, because the bake rotates the
        // vertices and re-winds them. Facing east is vanilla's y=90 variant, which
        // takes the model's north face onto the world's east face carrying its
        // texture with it — so the observer's front, now on +X, reads exactly as
        // the facing=north front did on -Z.
        {
            const auto& turned = meshOf(BlockState{Block::Observer,
                                                   BlockOrientation::East}).mesh;
            // Rotating the model's north face onto +X by y=90 maps its four
            // vertices (1,1,0),(1,0,0),(0,0,0),(0,1,0) — carrying uv (0,0),(0,1),
            // (1,1),(1,0) — onto (1,1,1),(1,0,1),(1,0,0),(1,1,0).
            expectUv(turned, east, {2.0F, 1.0F, 2.0F}, 0.0F, 1.0F, "observer east front (bot,z1)");
            expectUv(turned, east, {2.0F, 1.0F, 1.0F}, 1.0F, 1.0F, "observer east front (bot,z0)");
            expectUv(turned, east, {2.0F, 2.0F, 1.0F}, 1.0F, 0.0F, "observer east front (top,z0)");
            expectUv(turned, east, {2.0F, 2.0F, 2.0F}, 0.0F, 0.0F, "observer east front (top,z1)");
        }
    }

    // --- RN-8a / RN-8b: the face-cull criterion, row by row --------------------
    //
    // Every row of RN-8's zero-regression table, asserted at the mesh. The
    // question each row asks is the same one: does `current` still emit the quad
    // facing `neighbour`? The three rows this round exists for are marked; the
    // rest are the ones that must NOT move, because the criterion changed from
    // "what render bucket is the neighbour in" to "does the neighbour's shape
    // seal this face", and only one cell of that table was supposed to flip.
    {
        using mc::world::Block;
        using mc::world::BlockOrientation;
        using mc::world::BlockState;
        using mc::world::SlabPortion;
        using mc::world::StateProperty;

        constexpr glm::vec3 plusX{1.0F, 0.0F, 0.0F};
        constexpr glm::vec3 plusY{0.0F, 1.0F, 0.0F};

        // `current`'s +X face against `neighbour`, in the mesh bucket named.
        const auto opaqueSideFace = [&](Block current, BlockState neighbour) {
            return faceVertices(meshPair(BlockState{current}, neighbour).mesh, plusX, 0, 2.0F);
        };
        const auto waterSideFace = [&](BlockState neighbour) {
            return faceVertices(meshPair(BlockState{Block::Water}, neighbour).translucentMesh,
                                plusX, 0, 2.0F);
        };
        const auto glassSideFace = [&](Block current, Block neighbour) {
            return faceVertices(meshPair(BlockState{current}, BlockState{neighbour}).translucentMesh,
                                plusX, 0, 2.0F);
        };

        // 1. stone / stone — culled, as it always was. If this row ever draws,
        //    the criterion is inverted and every solid surface doubles.
        assert(opaqueSideFace(Block::Stone, BlockState{Block::Stone}) == 0);
        // 2. stone / glass — drawn (glass cannot occlude).
        assert(opaqueSideFace(Block::Stone, BlockState{Block::Glass}) == 4);
        // 3. stone / leaves — drawn (Cutout cannot occlude).
        assert(opaqueSideFace(Block::Stone, BlockState{Block::OakLeaves}) == 4);
        // 4. stone / stairs — drawn. Overdraw that RN-8e may reclaim; not here.
        assert(opaqueSideFace(Block::Stone, BlockState{Block::OakStairs}) == 4);
        // 5. *** stone top / anvil above — drawn. Defect B: the anvil is in the
        //    Opaque bucket, so the old renderLayer criterion culled the stone's
        //    top face and left a 2px ring of holes around the anvil's 12x12 base.
        //    Nothing about the anvil's tags changed; its four boxes simply seal
        //    no face. This is one of the three acceptance rows. ***
        assert(faceVertices(meshStack(BlockState{Block::Stone}, BlockState{Block::Anvil}).mesh,
                            plusY, 1, 2.0F) == 4);
        // 6. *** water / torch — drawn. Defect A: a Cutout neighbour used to cull
        //    a translucent face, so a torch beside water opened a see-through
        //    hole. Second acceptance row. ***
        assert(waterSideFace(BlockState{Block::Torch}) == 4);
        // 7. water / stairs, wall, door, grass — the rest of defect A's family.
        assert(waterSideFace(BlockState{Block::OakStairs}) == 4);
        assert(waterSideFace(BlockState{Block::MossyCobblestoneWall}) == 4);
        assert(waterSideFace(BlockState{Block::OakDoor}) == 4);
        assert(waterSideFace(BlockState{Block::GrassPlant}) == 4);
        // 8. water / water — culled (skipRendering, same fluid).
        assert(waterSideFace(BlockState{Block::Water}) == 0);
        // 9. water / stone — culled.
        assert(waterSideFace(BlockState{Block::Stone}) == 0);
        // 10. glass / the same glass — culled (skipRendering).
        assert(glassSideFace(Block::Glass, Block::Glass) == 0);
        assert(glassSideFace(Block::WhiteStainedGlass, Block::WhiteStainedGlass) == 0);
        // 11. glass / a different glass — drawn.
        assert(glassSideFace(Block::WhiteStainedGlass, Block::OrangeStainedGlass) == 4);
        // 12. stone / farmland — drawn. The old `modelHeight` exemption is gone;
        //     farmland's 15/16 column seals only its own floor, which is the same
        //     answer without the exemption existing.
        assert(opaqueSideFace(Block::Stone, BlockState{Block::Farmland}) == 4);
        // 13. stone / double slab — now culled. The old signature could not see
        //     SlabType and had to keep the face against every slab; the snapshot
        //     resolves the state, so a double slab occludes like the full cube it
        //     is. A bottom slab still cannot seal a side face.
        assert(opaqueSideFace(Block::Stone,
                              BlockState{Block::StoneSlab}.with(
                                  StateProperty::SlabType,
                                  static_cast<std::uint8_t>(SlabPortion::Double))) == 0);
        assert(opaqueSideFace(Block::Stone,
                              BlockState{Block::StoneSlab}.with(
                                  StateProperty::SlabType,
                                  static_cast<std::uint8_t>(SlabPortion::Bottom))) == 4);
        // 14. *** stone / dirt path — drawn. The regression gate for reading the
        //     declared modelHeight instead of asking "is this Farmland": with the
        //     identity check, dirt path was a full cube, and deleting the
        //     modelHeight exemption would have culled this face and opened a 1/16
        //     see-through seam above the path. Third acceptance row. ***
        assert(opaqueSideFace(Block::Stone, BlockState{Block::DirtPath}) == 4);
        // 15. stone / enchanting table — the side is drawn (Column{0,12/16} seals
        //     no side face), while the table stacked on stone does cull the
        //     stone's top, exactly as vanilla does. Neither needed a tag change.
        assert(opaqueSideFace(Block::Stone, BlockState{Block::EnchantingTable}) == 4);
        assert(faceVertices(
                   meshStack(BlockState{Block::Stone}, BlockState{Block::EnchantingTable}).mesh,
                   plusY, 1, 2.0F) == 0);

        // RN-8b: a cullface declaration is consumed, not ignored. The enchanting
        // table's west face carries `"cullface": "west"` in
        // models/block/enchanting_table.json, so against stone it goes away and
        // against air it stays. Its up face declares no cullface and is drawn
        // either way — the getQuads(null) half of JE's split.
        {
            constexpr glm::vec3 minusX{-1.0F, 0.0F, 0.0F};
            const auto againstStone = meshPair(BlockState{Block::Stone},
                                               BlockState{Block::EnchantingTable});
            assert(faceVertices(againstStone.mesh, minusX, 0, 2.0F) == 0);
            const auto againstAir = meshPair(BlockState{Block::Air},
                                             BlockState{Block::EnchantingTable});
            assert(faceVertices(againstAir.mesh, minusX, 0, 2.0F) == 4);
            // The table is 12/16 tall, so its up face sits at y = 1 + 12/16.
            assert(faceVertices(againstStone.mesh, plusY, 1, 1.0F + 12.0F / 16.0F) == 4);
        }
        // RN-8b: the anvil base's down face is template_anvil.json's only
        // cullface. Over stone it is culled; over air it is drawn.
        {
            constexpr glm::vec3 minusY{0.0F, -1.0F, 0.0F};
            const auto overStone = meshStack(BlockState{Block::Stone}, BlockState{Block::Anvil});
            assert(faceVertices(overStone.mesh, minusY, 1, 2.0F) == 0);
            const auto overAir = meshStack(BlockState{Block::Air}, BlockState{Block::Anvil});
            assert(faceVertices(overAir.mesh, minusY, 1, 2.0F) == 4);
        }

        // The overdraw guard RN-8's design doc asks for by name: a solid volume
        // of one block must not gain a single vertex from the new criterion. A
        // 4x4x4 cube of stone shows only its 6 outer 4x4 faces; a pool of water
        // shows only its own surface and walls. If the criterion were inverted,
        // both would explode by the interior faces.
        {
            mc::world::World solidWorld;
            mc::world::Chunk solidChunk;
            for (int y = 0; y < 4; ++y) {
                for (int z = 0; z < 4; ++z) {
                    for (int x = 0; x < 4; ++x) {
                        solidChunk.setBlock(4 + x, mc::world::kMinY + 1 + y, 4 + z,
                                            Block::Stone);
                    }
                }
            }
            solidWorld.setChunk({0, 0}, std::move(solidChunk));
            const auto solid = mc::world::ChunkMesher::buildSection(solidWorld, {0, 0}, 0);
            // 6 sides x 16 quads x 4 vertices, and nothing else.
            assert(solid.mesh.vertices.size() == 6U * 16U * 4U);
            assert(solid.cutoutMesh.empty() && solid.translucentMesh.empty());

            mc::world::World poolWorld;
            mc::world::Chunk poolChunk;
            for (int y = 0; y < 4; ++y) {
                for (int z = 0; z < 4; ++z) {
                    for (int x = 0; x < 4; ++x) {
                        poolChunk.setBlock(4 + x, mc::world::kMinY + 1 + y, 4 + z, Block::Water);
                        poolChunk.setFluidLevel(4 + x, mc::world::kMinY + 1 + y, 4 + z, 0U);
                    }
                }
            }
            poolWorld.setChunk({0, 0}, std::move(poolChunk));
            const auto pool = mc::world::ChunkMesher::buildSection(poolWorld, {0, 0}, 0);
            assert(pool.translucentMesh.vertices.size() == 6U * 16U * 4U);
        }
    }
    return 0;
}

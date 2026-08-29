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
        // A door is a single thin leaf box: 24 vertices, and its box hugs one
        // face rather than the whole cell — the mesh is a thin box, not a cube.
        mc::world::Chunk doorChunk;
        const mc::world::BlockState door{mc::world::Block::OakDoor};
        doorChunk.setState(3, mc::world::kMinY + 1, 1, door);
        const auto doorMesh = mc::world::ChunkMesher::build(doorChunk);
        assert(mc::world::blockShape(door).boxes.size() == 1U);
        assert(doorMesh.vertices.size() == 24U);
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
    return 0;
}

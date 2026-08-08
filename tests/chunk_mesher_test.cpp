#include "world/ChunkMesher.hpp"
#include "world/SurfaceGenerator.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string_view>

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
    mc::world::Chunk empty;
    assert(mc::world::ChunkMesher::build(empty).empty());

    mc::world::Chunk oneBlock;
    oneBlock.setBlock(1, 1, 1, mc::world::Block::Grass);
    const auto oneBlockMesh = mc::world::ChunkMesher::build(oneBlock);
    assert(oneBlockMesh.vertices.size() == 24U);
    assert(oneBlockMesh.indices.size() == 36U);

    oneBlock.setBlock(2, 1, 1, mc::world::Block::Dirt);
    const auto twoBlockMesh = mc::world::ChunkMesher::build(oneBlock);
    assert(twoBlockMesh.vertices.size() == 40U);
    assert(twoBlockMesh.indices.size() == 60U);

    // A truncated block (farmland at 15/16) must not cull its neighbour's face.
    // The dirt keeps its side toward the farmland, or a see-through sliver would
    // appear above the farmland's lower top; the farmland drops only the face
    // directly against the solid dirt. 5 farmland faces + 6 dirt faces = 11.
    mc::world::Chunk farmlandNeighbour;
    farmlandNeighbour.setBlock(1, 1, 1, mc::world::Block::Farmland);
    farmlandNeighbour.setBlock(2, 1, 1, mc::world::Block::Dirt);
    const auto farmlandMesh = mc::world::ChunkMesher::build(farmlandNeighbour);
    assert(farmlandMesh.vertices.size() == 44U);
    assert(farmlandMesh.indices.size() == 66U);

    // A crop renders as the vanilla crop.json grid: four orthogonal planes at
    // the quarter offsets (x=4/16, x=12/16, z=4/16, z=12/16), each double-sided
    // — 16 vertices, 48 indices, unlike the two diagonal planes of a `cross`.
    mc::world::Chunk cropChunk;
    cropChunk.setBlock(1, 1, 1, mc::world::Block::WheatCrops);
    const auto cropMesh = mc::world::ChunkMesher::build(cropChunk);
    assert(cropMesh.vertices.size() == 16U);
    assert(cropMesh.indices.size() == 48U);

    mc::world::Chunk sectionBoundary;
    sectionBoundary.setBlock(1, 15, 1, mc::world::Block::Stone);
    sectionBoundary.setBlock(1, 16, 1, mc::world::Block::Stone);
    const auto sectionBoundaryMesh = mc::world::ChunkMesher::build(sectionBoundary);
    assert(sectionBoundaryMesh.vertices.size() == 40U);
    assert(sectionBoundaryMesh.indices.size() == 60U);

    mc::world::World boundaryWorld;
    mc::world::Chunk left;
    left.setBlock(15, 1, 1, mc::world::Block::Stone);
    mc::world::Chunk right;
    right.setBlock(0, 1, 1, mc::world::Block::Stone);
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
    glassChunk.setBlock(1, 1, 1, mc::world::Block::Glass);
    glassChunk.setBlock(2, 1, 1, mc::world::Block::Glass);
    glassWorld.setChunk({0, 0}, std::move(glassChunk));
    const auto glassMesh = mc::world::ChunkMesher::buildSection(glassWorld, {0, 0}, 0);
    assert(glassMesh.mesh.empty());
    assert(glassMesh.translucentMesh.vertices.size() == 40U);
    assert(glassMesh.translucentMesh.indices.size() == 60U);
    static_assert(!mc::world::isOpaque(mc::world::Block::Glass));
    static_assert(mc::world::hasCollision(mc::world::Block::Glass));

    mc::world::World leavesWorld;
    mc::world::Chunk leavesChunk;
    leavesChunk.setBlock(1, 1, 1, mc::world::Block::OakLeaves);
    leavesChunk.setBlock(2, 1, 1, mc::world::Block::OakLeaves);
    leavesWorld.setChunk({0, 0}, std::move(leavesChunk));
    const auto leavesMesh =
        mc::world::ChunkMesher::buildSection(leavesWorld, {0, 0}, 0);
    assert(leavesMesh.mesh.empty());
    assert(leavesMesh.cutoutMesh.vertices.size() == 44U);
    assert(leavesMesh.cutoutMesh.indices.size() == 72U);
    static_assert(!mc::world::isOpaque(mc::world::Block::OakLeaves));

    mc::world::World torchWorld;
    mc::world::Chunk torchChunk;
    torchChunk.setBlock(1, 1, 1, mc::world::Block::Torch);
    torchChunk.setBlock(3, 1, 1, mc::world::Block::WallTorchEast);
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
    static_assert(!mc::world::hasCollision(mc::world::Block::WallTorchEast));
    static_assert(mc::world::emittedLight(mc::world::Block::WallTorchEast) == 14U);
    static_assert(mc::world::textureLayers(mc::world::Block::Chest).top == 220.0F);

    mc::world::World directionalWorld;
    mc::world::Chunk directionalChunk;
    directionalChunk.setBlock(1, 1, 1, mc::world::Block::Furnace);
    directionalChunk.setOrientation(1, 1, 1, mc::world::BlockOrientation::East);
    directionalChunk.setBlock(3, 1, 1, mc::world::Block::OakLog);
    directionalChunk.setOrientation(3, 1, 1, mc::world::BlockOrientation::East);
    directionalWorld.setChunk({0, 0}, std::move(directionalChunk));
    const auto directionalMesh =
        mc::world::ChunkMesher::buildSection(directionalWorld, {0, 0}, 0);
    expectNear(mc::render::decodeTextureLayer(directionalMesh.mesh.vertices[0]), 223.0F,
               "furnace front layer");
    expectNear(mc::render::decodeTextureLayer(directionalMesh.mesh.vertices[32]), 9.0F,
               "furnace side layer");
    assert(directionalWorld.orientation(1, 1, 1) == mc::world::BlockOrientation::East);

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
    zAxisLogChunk.setBlock(1, 1, 1, mc::world::Block::OakLog);
    zAxisLogChunk.setOrientation(1, 1, 1, mc::world::BlockOrientation::South);
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
    waterChunk.setBlock(1, 1, 1, mc::world::Block::Water);
    waterChunk.setFluidLevel(1, 1, 1, 0U);
    waterChunk.setBlock(2, 1, 1, mc::world::Block::Water);
    waterChunk.setFluidLevel(2, 1, 1, 7U);
    for (int y = 1; y <= 4; ++y) {
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
            return mc::render::decodeTextureLayer(vertex) == 52.0F;
        }));
    assert(std::ranges::any_of(
        waterMesh.translucentMesh.vertices,
        [](const auto& vertex) {
            return mc::render::decodeTextureLayer(vertex) == 20.0F &&
                mc::render::decodeWaterDepth(vertex) >= 4.0F;
        }));

    mc::world::World negativeWorld;
    mc::world::Chunk negativeChunk;
    negativeChunk.setBlock(15, 3, 15, mc::world::Block::Sand);
    negativeWorld.setChunk({-1, -1}, std::move(negativeChunk));
    assert(negativeWorld.block(-1, 3, -1) == mc::world::Block::Sand);
    assert(negativeWorld.block(-16, 3, -16) == mc::world::Block::Air);

    const mc::world::SurfaceGenerator generator{0x5EEDULL};
    const auto first = generator.generate(0, 0);
    const auto repeated = generator.generate(0, 0);
    for (int z = 0; z < mc::world::kChunkDepth; ++z) {
        for (int x = 0; x < mc::world::kChunkWidth; ++x) {
            assert(first.block(x, 0, z) == mc::world::Block::Bedrock);
            for (int y = 0; y < mc::world::kWorldHeight; ++y) {
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
    // The reference values below were taken from the 1.16.1 Java source.
    mc::world::World plantWorld;
    mc::world::Chunk plantChunk;
    plantChunk.setBlock(0, 0, 0, mc::world::Block::GrassPlant);
    plantChunk.setBlock(3, 1, 5, mc::world::Block::Dandelion);
    plantChunk.setBlock(8, 2, 8, mc::world::Block::OakSapling);
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
    return 0;
}

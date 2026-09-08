#include "world/ChunkMesher.hpp"
#include "world/World.hpp"
#include "world/WorldLighting.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {

// 26.1's getAmbientOcclusionLightLevel: an opaque ring cell contributes 0.2,
// anything else 1.0, and the corner is their mean over the four ring cells.
// One occluded ring cell is (1 + 1 + 1 + 0.2) / 4; a corner boxed in on both
// sides and the diagonal is (1 + 0.2 + 0.2 + 0.2) / 4.
//
// RN-19b: these were 0.8375 and 0.5125 — the deleted "Standard" tier's numbers,
// which used a 0.35 floor instead of 0.2 and pinned the outside cell at a
// constant 1.0. That tier could never produce a corner darker than 0.5125, and
// the shader then remapped [0, 1] into [0.72, 1.0] on top, so its darkest
// possible rendered corner was 0.86 against vanilla's 0.2.
constexpr float kSingleSideOcclusion = 0.8F;
// Both edges occluded, the diagonal cell open: (1 + 0.2 + 0.2 + 1) / 4.
//
// RN-19b's note here predicted vanilla answers 0.4 and that RN-19c would change
// this constant. **It does not, and the prediction was wrong.** 26.1's
// substitution (`BlockModelLighter` :69-113) is gated on `translucentN`, which
// reads the cell one step further along the face normal from the edge neighbour,
// NOT the edge neighbour itself (:60-67). In this scene those cells are air, so
// the rule does not fire and the real diagonal is read — 0.6, unchanged. The
// scene that does fire it is a wall two cells tall, and it lives in
// `testInnerCornerSubstitution` below.
constexpr float kOpenDiagonalCornerOcclusion = 0.6F;
// AO is quantized to a u8 in the packed vertex (1/255 resolution), so the AO
// assertions compare within one quantum instead of exact float.
constexpr float kAoTolerance = 0.01F;
// Packed positions quantize to 17/65535 ≈ 0.00026 blocks; the top-face filter
// must tolerate that.
constexpr float kPositionTolerance = 0.01F;

void expectNear(float actual, float expected, std::string_view context) {
    if (std::abs(actual - expected) <= 0.0001F) {
        return;
    }
    std::ostringstream message;
    message << context << ": expected " << expected << ", actual " << actual;
    throw std::runtime_error(message.str());
}

void expectNearAo(float actual, float expected, std::string_view context) {
    if (std::abs(actual - expected) <= kAoTolerance) {
        return;
    }
    std::ostringstream message;
    message << context << ": expected " << expected << ", actual " << actual;
    throw std::runtime_error(message.str());
}

// Returns the four corner vertices *by value*. An earlier version handed back
// pointers into the mesh, which every call site fed a temporary MeshData built
// inline: the mesh died at the end of the full expression and the assertions
// read freed memory (a segfault on the default stack, a silent AO of 0 with a
// larger one). Copying 4 x 24 bytes makes the lifetime question disappear.
[[nodiscard]] std::array<mc::render::VoxelVertex, 4>
topFaceVertices(const mc::render::MeshData& mesh, int blockX, int blockY, int blockZ) {
    std::array<mc::render::VoxelVertex, 4> result{};
    std::array<bool, 4> found{};
    for (const auto& vertex : mesh.vertices) {
        // All scenes build section 0 of chunk (0,0), whose origin is the world
        // origin, so decoded local positions equal world positions.
        const glm::vec3 normal = mc::render::decodeNormal(vertex);
        const glm::vec3 position = mc::render::decodeLocalPosition(vertex);
        if (std::abs(normal.x) > 0.0001F || std::abs(normal.y - 1.0F) > 0.0001F ||
            std::abs(normal.z) > 0.0001F ||
            std::abs(position.y - static_cast<float>(blockY + 1)) > kPositionTolerance ||
            position.x < static_cast<float>(blockX) - kPositionTolerance ||
            position.x > static_cast<float>(blockX + 1) + kPositionTolerance ||
            position.z < static_cast<float>(blockZ) - kPositionTolerance ||
            position.z > static_cast<float>(blockZ + 1) + kPositionTolerance) {
            continue;
        }
        const int xCorner = static_cast<int>(std::lround(position.x)) - blockX;
        const int zCorner = static_cast<int>(std::lround(position.z)) - blockZ;
        const auto corner = static_cast<std::size_t>(zCorner * 2 + xCorner);
        result[corner] = vertex;
        found[corner] = true;
    }
    for (const bool present : found) {
        if (!present) {
            throw std::runtime_error("top face did not contain all four expected vertices");
        }
    }
    return result;
}

[[nodiscard]] mc::render::MeshData buildLightingScene(std::initializer_list<glm::ivec3> occluders) {
    mc::world::World world;
    mc::world::Chunk chunk;
    chunk.setBlock(1, mc::world::kMinY + 1, 1, mc::world::Block::Stone);
    for (const auto& position : occluders) {
        chunk.setBlock(position.x, position.y, position.z, mc::world::Block::Stone);
    }
    world.setChunk({0, 0}, std::move(chunk));
    return mc::world::ChunkMesher::buildSection(world, {0, 0}, 0).mesh;
}

[[nodiscard]] mc::render::MeshData buildLightingSceneHigh(
    std::initializer_list<glm::ivec3> occluders) {
    mc::world::World world;
    mc::world::Chunk chunk;
    chunk.setBlock(1, mc::world::kMinY + 1, 1, mc::world::Block::Stone);
    for (const auto& position : occluders) {
        chunk.setBlock(position.x, position.y, position.z, mc::world::Block::Stone);
    }
    world.setChunk({0, 0}, std::move(chunk));
    return mc::world::ChunkMesher::buildSection(world, {0, 0}, 0).mesh;
}

} // namespace


// RN-19c: 26.1 does not always read the diagonal cell.
//
// `BlockModelLighter` :69-113 — when the cells BEYOND both of a corner's edges
// block the view, the diagonal is not sampled at all; the sample at `corners[0]`
// is reused. An inner corner (a floor and two walls) therefore averages an open
// cell where this build averaged the solid diagonal, and comes out brighter.
//
// The rule has a fingerprint no approximation of it reproduces, and this test is
// built around that fingerprint rather than around one number: the reused sample
// is `shade0` in all four branches (:71/:82/:93/:104) — including the two whose
// edges are 1&2 and 1&3, for which corner 0 is not even an edge. So the four
// corners of a face are NOT symmetric. `corners[0]` for an UP face is +X
// (`AdjacencyInfo.UP` :327), which means:
//
//   * a corner that owns the +X edge substitutes its own (solid) edge -> 0.4,
//     the same value it had before the rule existed;
//   * a corner that does not own it substitutes the open +X cell -> 0.6.
//
// Both "diagonal = min(edgeA, edgeB)" and "each corner reuses its own edge"
// answer 0.4 for all four, so either would fail this test where a single-scene
// assertion would pass them.
void testInnerCornerSubstitution() {
    // A stone floor with two walls, two cells tall, meeting at the cell whose
    // top face is measured. `dx`/`dz` name which sides the walls are on.
    const auto cornerAo = [](int dx, int dz) {
        mc::world::World world;
        mc::world::Chunk chunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                chunk.setBlock(x, mc::world::kMinY + 1, z, mc::world::Block::Stone);
            }
        }
        for (int y = mc::world::kMinY + 2; y <= mc::world::kMinY + 3; ++y) {
            chunk.setBlock(8 + dx, y, 8, mc::world::Block::Stone);
            chunk.setBlock(8, y, 8 + dz, mc::world::Block::Stone);
            chunk.setBlock(8 + dx, y, 8 + dz, mc::world::Block::Stone);
        }
        mc::world::World scene;
        scene.setChunk({0, 0}, std::move(chunk));
        const mc::world::MeshLightingSnapshot snapshot{scene, {0, 0}, 0, 0};
        mc::render::RenderMeshData mesh;
        static_cast<void>(
            mc::world::ChunkMesher::buildSection(scene, {0, 0}, 0, snapshot, mesh));
        const auto vertices = topFaceVertices(mesh.mesh, 8, 1, 8);
        // The corner vertex nearest the two walls.
        const auto index = static_cast<std::size_t>((dz > 0 ? 1 : 0) * 2 + (dx > 0 ? 1 : 0));
        return mc::render::decodeAmbientOcclusion(vertices[index]);
    };

    // Owns the +X edge -> substitutes a solid sample -> unchanged at 0.4.
    expectNearAo(cornerAo(1, 1), 0.4F, "inner corner owning +X (walls +X,+Z)");
    expectNearAo(cornerAo(1, -1), 0.4F, "inner corner owning +X (walls +X,-Z)");
    // Does not own it -> substitutes the open +X cell -> 0.6.
    expectNearAo(cornerAo(-1, -1), 0.6F, "inner corner without +X (walls -X,-Z)");
    expectNearAo(cornerAo(-1, 1), 0.6F, "inner corner without +X (walls -X,+Z)");
}

// RN-19c: the diagonal is not merely down-weighted when the rule fires — it is
// not read. Replacing the diagonal cell with air must change nothing, which no
// "weigh the diagonal less" approximation can satisfy.
void testInnerCornerIgnoresTheDiagonalEntirely() {
    const auto cornerAo = [](bool solidDiagonal) {
        mc::world::World world;
        mc::world::Chunk chunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                chunk.setBlock(x, mc::world::kMinY + 1, z, mc::world::Block::Stone);
            }
        }
        for (int y = mc::world::kMinY + 2; y <= mc::world::kMinY + 3; ++y) {
            chunk.setBlock(7, y, 8, mc::world::Block::Stone);
            chunk.setBlock(8, y, 7, mc::world::Block::Stone);
            if (solidDiagonal) {
                chunk.setBlock(7, y, 7, mc::world::Block::Stone);
            }
        }
        mc::world::World scene;
        scene.setChunk({0, 0}, std::move(chunk));
        const mc::world::MeshLightingSnapshot snapshot{scene, {0, 0}, 0, 0};
        mc::render::RenderMeshData mesh;
        static_cast<void>(
            mc::world::ChunkMesher::buildSection(scene, {0, 0}, 0, snapshot, mesh));
        const auto vertices = topFaceVertices(mesh.mesh, 8, 1, 8);
        return mc::render::decodeAmbientOcclusion(vertices[0]);
    };
    expectNearAo(cornerAo(true), cornerAo(false),
                 "the diagonal cell is not read when both edges are walled");
}

// RN-19c: `faceCubic` decides where the ring is anchored (`BlockModelLighter`
// :40, :262-270). A face flush with the cell wall anchors one cell out; a face
// that sits inside its cell anchors at the block's own cell, so it sees the
// neighbours of the block it belongs to rather than a ring floating above it.
//
// A closed trapdoor is the cleanest case in this roster: its panel is a box
// 3/16 tall, so its top face is inset. The two scenes differ only in which layer
// the neighbouring stone is on.
void testInsetFaceAnchorsAtItsOwnCell() {
    const auto trapdoorCornerAo = [](int neighbourLayer) {
        mc::world::Chunk chunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                chunk.setBlock(x, mc::world::kMinY, z, mc::world::Block::Stone);
            }
        }
        chunk.setBlock(8, mc::world::kMinY + 1, 8, mc::world::Block::OakTrapdoor);
        chunk.setBlock(9, mc::world::kMinY + 1 + neighbourLayer, 8, mc::world::Block::Stone);
        mc::world::World scene;
        scene.setChunk({0, 0}, std::move(chunk));
        const mc::world::MeshLightingSnapshot snapshot{scene, {0, 0}, 0, 0};
        mc::render::RenderMeshData mesh;
        static_cast<void>(
            mc::world::ChunkMesher::buildSection(scene, {0, 0}, 0, snapshot, mesh));
        float darkest = 1.0F;
        for (const auto* part : {&mesh.mesh, &mesh.cutoutMesh}) {
            for (const auto& vertex : part->vertices) {
                const glm::vec3 normal = mc::render::decodeNormal(vertex);
                const glm::vec3 position = mc::render::decodeLocalPosition(vertex);
                if (normal.y > 0.5F && position.x > 8.9F && position.x < 9.1F &&
                    position.y < static_cast<float>(mc::world::kMinY + 2 - mc::world::kMinY)) {
                    darkest = std::min(darkest, mc::render::decodeAmbientOcclusion(vertex));
                }
            }
        }
        return darkest;
    };
    // Stone in the trapdoor's OWN layer darkens the inset top face, because the
    // ring is anchored there. Before this node the ring sat one cell up and this
    // neighbour was invisible to it.
    expectNearAo(trapdoorCornerAo(0), 0.8F, "inset face sees its own layer's neighbour");
    // Stone one layer up does not, for the same reason in reverse.
    expectNearAo(trapdoorCornerAo(1), 1.0F, "inset face does not see the layer above it");
}

// Regression: a full cube's face is flush with the cell wall, so faceCubic is
// true for it and its ring is where it always was. This is what keeps the change
// confined to inset faces.
void testFullCubeFaceKeepsItsAnchor() {
    mc::world::Chunk chunk;
    chunk.setBlock(8, mc::world::kMinY + 1, 8, mc::world::Block::Stone);
    chunk.setBlock(9, mc::world::kMinY + 1, 8, mc::world::Block::Stone);
    mc::world::World scene;
    scene.setChunk({0, 0}, std::move(chunk));
    const mc::world::MeshLightingSnapshot snapshot{scene, {0, 0}, 0, 0};
    mc::render::RenderMeshData mesh;
    static_cast<void>(mc::world::ChunkMesher::buildSection(scene, {0, 0}, 0, snapshot, mesh));
    const auto vertices = topFaceVertices(mesh.mesh, 8, 1, 8);
    // A neighbour in the cube's own layer is beside the face, not above it, so
    // it cannot darken the top face: every corner stays fully bright.
    for (std::size_t corner = 0; corner < vertices.size(); ++corner) {
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[corner]), 1.0F,
                     "a flush face ignores its own layer's neighbour");
    }
}

int main() {
    testInnerCornerSubstitution();
    testInnerCornerIgnoresTheDiagonalEntirely();
    testInsetFaceAnchorsAtItsOwnCell();
    testFullCubeFaceKeepsItsAnchor();
    {
        mc::world::World world;
        mc::world::Chunk chunk;
        chunk.setBlock(8, mc::world::kMinY + 10, 8, mc::world::Block::Stone);
        chunk.setBlock(4, mc::world::kMinY + 8, 4, mc::world::Block::Torch);
        chunk.setBlock(12, mc::world::kMinY + 8, 12, mc::world::Block::Glowstone);
        world.setChunk({0, 0}, std::move(chunk));
        const mc::world::ChunkLightSampler lighting{world, {0, 0}};
        assert(lighting.level(8, mc::world::kMinY + 11, 8).sky == 15U);
        assert(lighting.level(8, mc::world::kMinY + 9, 8).sky == 14U);
        assert(lighting.level(4, mc::world::kMinY + 8, 4).block == 14U);
        assert(lighting.level(5, mc::world::kMinY + 8, 4).block == 13U);
        assert(lighting.level(12, mc::world::kMinY + 8, 12).block == 15U);
        assert(lighting.level(13, mc::world::kMinY + 8, 12).block == 14U);
    }

    {
        mc::world::World world;
        mc::world::Chunk left;
        left.setBlock(15, mc::world::kMinY + 8, 8, mc::world::Block::Torch);
        world.setChunk({0, 0}, std::move(left));
        world.setChunk({1, 0}, mc::world::Chunk{});
        const std::array positions{
            mc::world::ChunkPosition{0, 0},
            mc::world::ChunkPosition{1, 0},
        };
        const mc::world::ChunkLightSampler sharedLighting{
            world, std::span<const mc::world::ChunkPosition>{positions}};
        assert(sharedLighting.level(15, mc::world::kMinY + 8, 8).block == 14U);
        assert(sharedLighting.level(16, mc::world::kMinY + 8, 8).block == 13U);
    }

    {
        const auto vertices = topFaceVertices(buildLightingScene({}), 1, 1, 1);
        for (std::size_t corner = 0; corner < vertices.size(); ++corner) {
            expectNearAo(mc::render::decodeAmbientOcclusion(vertices[corner]), 1.0F,
                         "isolated cube AO corner " + std::to_string(corner));
            expectNear(mc::render::decodeSkyLight(vertices[corner]), 1.0F,
                       "isolated cube sky light corner " + std::to_string(corner));
        }
    }

    {
        const auto vertices = topFaceVertices(buildLightingScene({{0, mc::world::kMinY + 2, 1}}), 1, 1, 1);
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[0]), kSingleSideOcclusion,
                     "single side AO at north-west corner");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[1]), 1.0F,
                     "single side AO at north-east corner");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[2]), kSingleSideOcclusion,
                     "single side AO at south-west corner");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[3]), 1.0F,
                     "single side AO at south-east corner");
    }

    {
        const auto vertices = topFaceVertices(buildLightingScene({{0, mc::world::kMinY + 2, 1}, {1, mc::world::kMinY + 2, 0}}), 1, 1, 1);
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[0]),
                     kOpenDiagonalCornerOcclusion, "open-diagonal corner AO");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[1]), kSingleSideOcclusion,
                     "north edge AO");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[2]), kSingleSideOcclusion,
                     "west edge AO");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[3]), 1.0F,
                     "unoccluded opposite corner AO");
    }

    {
        mc::world::World world;
        mc::world::Chunk left;
        left.setBlock(15, mc::world::kMinY + 1, 1, mc::world::Block::Stone);
        mc::world::Chunk right;
        right.setBlock(0, mc::world::kMinY + 2, 1, mc::world::Block::Stone);
        world.setChunk({0, 0}, std::move(left));
        world.setChunk({1, 0}, std::move(right));
        const auto mesh = mc::world::ChunkMesher::buildSection(world, {0, 0}, 0);
        const auto vertices = topFaceVertices(mesh.mesh, 15, 1, 1);
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[0]), 1.0F,
                     "chunk-border west corner AO");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[1]), kSingleSideOcclusion,
                     "chunk-border east corner AO");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[2]), 1.0F,
                     "chunk-border south-west corner AO");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[3]), kSingleSideOcclusion,
                     "chunk-border south-east corner AO");
    }

    {
        // The production MeshLightingSnapshot must mirror World::block / skyLight
        // / blockLight across its whole sampling window, including the neighbour
        // ring and the out-of-world Y fallbacks.
        mc::world::World world;
        mc::world::Chunk left;
        left.setBlock(8, mc::world::kMinY + 8, 8, mc::world::Block::Stone);
        left.setSkyLight(8, mc::world::kMinY + 9, 8, 14U);
        left.setBlockLight(8, mc::world::kMinY + 8, 8, 10U);
        mc::world::Chunk right;
        right.setBlock(0, mc::world::kMinY + 8, 8, mc::world::Block::Glowstone);
        right.setBlockLight(0, mc::world::kMinY + 8, 8, 15U);
        world.setChunk({0, 0}, std::move(left));
        world.setChunk({1, 0}, std::move(right));
        const mc::world::MeshLightingSnapshot snapshot{world, {0, 0}, 0, 0};
        assert(snapshot.level(8, mc::world::kMinY + 9, 8).sky == 14U);
        assert(snapshot.level(8, mc::world::kMinY + 8, 8).block == 10U);
        assert(snapshot.blockType(8, mc::world::kMinY + 8, 8) == mc::world::Block::Stone);
        assert(snapshot.isOpaque(8, mc::world::kMinY + 8, 8));
        assert(snapshot.aoDarkens(8, mc::world::kMinY + 8, 8));
        // Neighbour-chunk cell routes to the right chunk, not the request one.
        assert(snapshot.blockType(16, mc::world::kMinY + 8, 8) == mc::world::Block::Glowstone);
        assert(snapshot.level(16, mc::world::kMinY + 8, 8).block == 15U);
        // 萤石**会**压暗 AO 角。这一句从前断言的是反面，理由写的是「它的 vanilla
        // 材质是玻璃」——那是 1.16 Material 时代的答案。26.1 里 Material 整套已经
        // 没了，`getShadeBrightness` 只问 `isCollisionShapeFullBlock`
        // （BlockBehaviour.java:319-321），而 GLOWSTONE 是一个普通 `Block`
        // （Blocks.java:2050-2059，没有覆写、没有 noOcclusion）⇒ 0.2F。
        // 真正被覆写回 1.0F 的只有 `TransparentBlock` 那一族，也就是玻璃与染色玻璃。
        assert(snapshot.aoDarkens(16, mc::world::kMinY + 8, 8));
        // 而它同时也挡视线（isViewBlocking 走默认的「挡住移动且碰撞满格」，
        // 光衰减 15），所以两条谓词对它给同一个答案——玻璃才是那个分道扬镳的。
        assert(snapshot.aoBlocksView(16, mc::world::kMinY + 8, 8));
        // Missing neighbour chunk resolves to air, fully sky-lit.
        assert(snapshot.blockType(-2, mc::world::kMinY + 8, 8) == mc::world::Block::Air);
        assert(snapshot.level(-2, mc::world::kMinY + 8, 8).sky == 15U);
        // Out-of-world Y fallbacks match ChunkLightSampler::level: below the new
        // bottom is dark, above the new top is full sky.
        assert(snapshot.level(8, mc::world::kMinY - 1, 8).sky == 0U);
        assert(snapshot.level(8, mc::world::kMaxY, 8).sky == 15U);
    }

    {
        // The snapshot production path must produce the same Standard AO as the
        // padded-sampler test path (AO is light-independent, so the zeroed light
        // in this scene does not matter).
        mc::world::World world;
        mc::world::Chunk chunk;
        chunk.setBlock(1, mc::world::kMinY + 1, 1, mc::world::Block::Stone);
        chunk.setBlock(0, mc::world::kMinY + 2, 1, mc::world::Block::Stone);
        world.setChunk({0, 0}, std::move(chunk));
        const mc::world::MeshLightingSnapshot snapshot{world, {0, 0}, 0, 0};
        mc::render::RenderMeshData snapshotMesh;
        static_cast<void>(mc::world::ChunkMesher::buildSection(
            world, {0, 0}, 0, snapshot, snapshotMesh));
        const auto vertices = topFaceVertices(snapshotMesh.mesh, 1, 1, 1);
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[0]), kSingleSideOcclusion,
                     "snapshot single side AO at north-west corner");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[1]), 1.0F,
                     "snapshot single side AO at north-east corner");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[2]), kSingleSideOcclusion,
                     "snapshot single side AO at south-west corner");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[3]), 1.0F,
                     "snapshot single side AO at south-east corner");
    }

    {
        // High quality (vanilla AO): an isolated cube stays full-bright.
        const auto vertices = topFaceVertices(buildLightingSceneHigh({}), 1, 1, 1);
        for (std::size_t corner = 0; corner < vertices.size(); ++corner) {
            expectNearAo(mc::render::decodeAmbientOcclusion(vertices[corner]), 1.0F,
                         "high isolated cube AO corner " + std::to_string(corner));
        }
    }

    {
        // One occluding side darkens the two corners beside it: the 2×2 ring
        // average puts one 0.2 cell among three open ones → (0.2+1+1+1)/4 = 0.8.
        const auto vertices = topFaceVertices(buildLightingSceneHigh({{0, mc::world::kMinY + 2, 1}}), 1, 1, 1);
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[0]), 0.8F,
                     "high single side AO at north-west corner");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[1]), 1.0F,
                     "high single side AO at north-east corner");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[2]), 0.8F,
                     "high single side AO at south-west corner");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[3]), 1.0F,
                     "high single side AO at south-east corner");
    }

    {
        // Two perpendicular sides occluded with the diagonal open: two 0.2 cells
        // → (0.2+0.2+1+1)/4 = 0.6 at the closed corner; the edge corners carry
        // one 0.2 cell → 0.8; the far corner stays open.
        const auto vertices =
            topFaceVertices(buildLightingSceneHigh({{0, mc::world::kMinY + 2, 1}, {1, mc::world::kMinY + 2, 0}}), 1, 1, 1);
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[0]), 0.6F,
                     "high closed-corner AO");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[1]), 0.8F,
                     "high north edge AO");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[2]), 0.8F,
                     "high west edge AO");
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[3]), 1.0F,
                     "high unoccluded opposite corner AO");
    }

    {
        // An opaque diagonal darkens the corner via the symmetric ring average
        // (0.2 among three open cells → 0.8). A block above the diagonal is not
        // part of the ring and does not change it — the corner value only
        // depends on the four ring cells, which is what keeps adjacent blocks
        // consistent at the shared corner.
        const auto withDiagonal =
            topFaceVertices(buildLightingSceneHigh({{0, mc::world::kMinY + 2, 0}}), 1, 1, 1);
        expectNearAo(mc::render::decodeAmbientOcclusion(withDiagonal[0]), 0.8F,
                     "high diagonal occluder AO");
        const auto withOverhangAbove =
            topFaceVertices(buildLightingSceneHigh({{0, mc::world::kMinY + 2, 0}, {0, mc::world::kMinY + 3, 0}}), 1, 1, 1);
        expectNearAo(mc::render::decodeAmbientOcclusion(withOverhangAbove[0]), 0.8F,
                     "high diagonal occluder AO with a block above it");
    }

    {
        // Glass above the cube is a full cube but its material is not an AO
        // occluder (vanilla glass): the sampled center stays open, so the top
        // face remains full-bright.
        mc::world::World world;
        mc::world::Chunk chunk;
        chunk.setBlock(1, mc::world::kMinY + 1, 1, mc::world::Block::Stone);
        chunk.setBlock(1, mc::world::kMinY + 2, 1, mc::world::Block::Glass);
        world.setChunk({0, 0}, std::move(chunk));
        const auto mesh = mc::world::ChunkMesher::buildSection(world, {0, 0}, 0);
        const auto vertices = topFaceVertices(mesh.mesh, 1, 1, 1);
        for (std::size_t corner = 0; corner < vertices.size(); ++corner) {
            expectNearAo(mc::render::decodeAmbientOcclusion(vertices[corner]), 1.0F,
                         "high glass center AO corner " + std::to_string(corner));
        }
    }

    return 0;
}

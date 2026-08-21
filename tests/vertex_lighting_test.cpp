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

constexpr float kSingleSideOcclusion = 0.8375F;
constexpr float kClosedCornerOcclusion = 0.5125F;
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
    return mc::world::ChunkMesher::buildSection(
        world, {0, 0}, 0, mc::world::SmoothLightingQuality::High).mesh;
}

} // namespace

int main() {
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
        expectNearAo(mc::render::decodeAmbientOcclusion(vertices[0]), kClosedCornerOcclusion,
                     "closed-corner AO");
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
        const mc::world::MeshLightingSnapshot snapshot{
            world, {0, 0}, 0, 0, mc::world::SmoothLightingQuality::Standard};
        assert(snapshot.level(8, mc::world::kMinY + 9, 8).sky == 14U);
        assert(snapshot.level(8, mc::world::kMinY + 8, 8).block == 10U);
        assert(snapshot.blockType(8, mc::world::kMinY + 8, 8) == mc::world::Block::Stone);
        assert(snapshot.isOpaque(8, mc::world::kMinY + 8, 8));
        assert(snapshot.aoOccludes(8, mc::world::kMinY + 8, 8));
        // Neighbour-chunk cell routes to the right chunk, not the request one.
        assert(snapshot.blockType(16, mc::world::kMinY + 8, 8) == mc::world::Block::Glowstone);
        assert(snapshot.level(16, mc::world::kMinY + 8, 8).block == 15U);
        // Glowstone's vanilla material is glass: it does not darken AO corners.
        assert(!snapshot.aoOccludes(16, mc::world::kMinY + 8, 8));
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
        const mc::world::MeshLightingSnapshot snapshot{
            world, {0, 0}, 0, 0, mc::world::SmoothLightingQuality::Standard};
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
        // High quality (vanilla 1.16.1 AO): an isolated cube stays full-bright.
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
        const auto mesh = mc::world::ChunkMesher::buildSection(
            world, {0, 0}, 0, mc::world::SmoothLightingQuality::High);
        const auto vertices = topFaceVertices(mesh.mesh, 1, 1, 1);
        for (std::size_t corner = 0; corner < vertices.size(); ++corner) {
            expectNearAo(mc::render::decodeAmbientOcclusion(vertices[corner]), 1.0F,
                         "high glass center AO corner " + std::to_string(corner));
        }
    }

    return 0;
}

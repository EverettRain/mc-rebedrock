// RN-19b/c: the cost of the mesher's per-face corner lighting, the pass that
// RN-19c is going to change (vanilla's "both edges opaque -> reuse the edge
// sample" rule, and the faceCubic sampling anchor). This is its A/B baseline.
//
// It was written for a different question first, and that answer is the reason
// this file exists at all. RN-19b had to decide whether the self-invented
// "Standard" smooth-lighting tier — the one that held the default and that no
// vanilla setting corresponds to — was worth keeping. The whole argument for it
// was that it was the cheap tier. Measured, on this machine, Release, median of
// 201 builds per section (the tier arm has been deleted with the tier, so these
// numbers are history, not something this file still reproduces):
//
//   scene                     Standard      High     High vs Standard
//   solid cube grid            406.75us   396.25us        -2.58%
//   stairs grid                689.79us   684.92us        -0.71%
//   fence-gate grid           1767.63us  1757.50us        -0.57%
//   generated, surface         130.25us   128.42us        -1.41%
//   generated, underground     274.83us   272.08us        -1.00%
//
// Five scenes, all the same direction: the "cheap" tier was the *slower* one.
// Both walk four corners x four ring cells; Standard resolved occupiedA/occupiedB
// twice (once in its AO half, once in its light half) and saved only the
// diagonal `level()` read in the both-sides-occupied case. It was never cheaper,
// only paler — so it had no reason to exist and RN-19b deleted it.
//
// Reading this file's numbers honestly:
//
//  * wall clock on one machine — only ratios from the same run mean anything.
//    There is no target here and there must not be one.
//  * it must be built RELEASE. A debug build measures the standard library.
//  * each case prints the darkest and the mean AO byte the mesh came out with,
//    so a run that silently stopped baking AO is visible in the output rather
//    than showing up as a speedup. That is the B4-0b lesson — a performance gate
//    whose benchmark never called the function it was gating — answered with a
//    value rather than a symbol.

#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkMesher.hpp"
#include "world/DimensionChunkGenerator.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"
#include "world/WorldLightEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using mc::world::Block;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::ChunkPosition;
using mc::world::MeshLightingSnapshot;
using mc::world::World;

constexpr int kBaseY = mc::world::kMinY + 8;
constexpr int kStride = 2;
constexpr int kLayers = 4;
constexpr int kRepetitions = 201;

// Isolated cells on a stride, so every face of every model is drawn and none is
// culled: the measurement is per-face corner work, which is where the tiers
// differ, not the cull's.
[[nodiscard]] World makeGridWorld(BlockState state) {
    World world;
    Chunk chunk;
    for (int x = 0; x < mc::world::kChunkWidth; ++x) {
        for (int z = 0; z < mc::world::kChunkDepth; ++z) {
            chunk.setBlock(x, kBaseY, z, Block::Stone);
        }
    }
    for (int layer = 0; layer < kLayers; ++layer) {
        for (int x = 0; x < mc::world::kChunkWidth; x += kStride) {
            for (int z = 0; z < mc::world::kChunkDepth; z += kStride) {
                chunk.setState(x, kBaseY + 1 + layer * 2, z, state);
            }
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
    mc::world::WorldLightEngine lighting;
    const std::array positions{ChunkPosition{0, 0}};
    lighting.initializeChunks(world, std::span<const ChunkPosition>{positions});
    return world;
}

// The other half of the picture: real generated terrain, where most faces are
// culled and the ring samples hit solid neighbours. A grid of isolated cubes is
// the worst case for corner work; this is the average one.
[[nodiscard]] World makeGeneratedWorld(std::uint64_t seed) {
    World world;
    mc::world::DimensionChunkGenerator generator{mc::world::DimensionId::Overworld, seed};
    std::vector<ChunkPosition> positions;
    for (int z = -1; z <= 1; ++z) {
        for (int x = -1; x <= 1; ++x) {
            std::vector<mc::world::gen::TreeBorderBlock> border;
            world.setChunk({x, z}, generator.generate(x, z, border));
            positions.push_back({x, z});
        }
    }
    mc::world::WorldLightEngine lighting;
    lighting.initializeChunks(world, std::span<const ChunkPosition>{positions});
    return world;
}

struct Result final {
    double medianMicroseconds = 0.0;
    std::size_t vertices = 0;
    int darkestAmbient = 255;
    double meanAmbient = 0.0;
};

[[nodiscard]] Result measure(const World& world, ChunkPosition position, int sectionIndex) {
    // The snapshot is built once and handed in: rebuilding it costs several
    // times what the geometry pass does and is untouched by the corner lighting,
    // so timing it would bury the thing being measured under noise.
    const MeshLightingSnapshot lighting{world, position, sectionIndex, sectionIndex};
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(kRepetitions));
    mc::render::RenderMeshData mesh;
    for (int i = 0; i < kRepetitions; ++i) {
        const auto start = std::chrono::steady_clock::now();
        static_cast<void>(
            mc::world::ChunkMesher::buildSection(world, position, sectionIndex, lighting, mesh));
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }
    std::sort(samples.begin(), samples.end());

    Result result;
    result.medianMicroseconds = samples[samples.size() / 2];
    long total = 0;
    long count = 0;
    const auto walk = [&](const mc::render::MeshData& data) {
        for (const auto& vertex : data.vertices) {
            result.darkestAmbient = std::min(result.darkestAmbient,
                                             static_cast<int>(vertex.ambientOcclusion));
            total += vertex.ambientOcclusion;
            ++count;
        }
    };
    walk(mesh.mesh);
    walk(mesh.cutoutMesh);
    result.vertices = static_cast<std::size_t>(count);
    result.meanAmbient = count > 0 ? static_cast<double>(total) / static_cast<double>(count) : 0.0;
    return result;
}

void run(const std::string& label, const World& world, ChunkPosition position, int sectionIndex) {
    const Result result = measure(world, position, sectionIndex);
    std::cout << std::left << std::setw(26) << label << std::right << std::fixed
              << std::setprecision(2) << "  median=" << std::setw(9)
              << result.medianMicroseconds << " us"
              << "   verts=" << std::setw(6) << result.vertices
              << "   darkest AO=" << std::setw(4) << result.darkestAmbient
              << "   mean AO=" << std::setprecision(1) << result.meanAmbient << "\n";
}

} // namespace

int main() {
    const int gridSection = mc::world::sectionIndexFromWorldY(kBaseY);
    std::cout << "Per-section mesh cost with vanilla AO baked, snapshot cost excluded.\n"
              << "A darkest AO of 51 (0.2 x 255) is the proof the AO pass really ran.\n\n";

    run("solid cube grid", makeGridWorld(BlockState{Block::Stone}), {0, 0}, gridSection);
    run("stairs grid", makeGridWorld(BlockState{Block::OakStairs}), {0, 0}, gridSection);
    run("fence-gate grid", makeGridWorld(BlockState{Block::OakFenceGate}), {0, 0}, gridSection);

    const World generated = makeGeneratedWorld(698280189473500ULL);
    run("generated, surface", generated, {0, 0}, mc::world::sectionIndexFromWorldY(68));
    run("generated, underground", generated, {0, 0}, mc::world::sectionIndexFromWorldY(20));
    return 0;
}

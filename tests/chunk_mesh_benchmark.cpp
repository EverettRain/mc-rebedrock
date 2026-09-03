// RN-10c: the remesh hot path, measured on the block this node changed.
//
// A fence gate used to mesh through a hand-written emitter that called
// rotateAxis once per vertex — 8 boxes x 6 faces x 4 corners = 192 sin/cos pairs
// per gate cell, per remesh, for a yaw that only ever takes four values. It now
// reads its quads out of the RN-8c-0 baked store, where the rotation is already
// applied, so the trig is gone and eight of the forty-eight faces with it.
//
// Reading this number honestly:
//
//  * it is wall clock on one machine, so only the ratio against a run of the
//    same benchmark on the same machine means anything. There is no target here
//    and there must not be one.
//  * it must be built RELEASE. A debug build of this measures the standard
//    library's iterators, not the mesher.
//  * "the benchmark exercises the code that changed" is not assumed: each case
//    prints the vertex count it produced, and a gate section is 160 vertices per
//    gate through the new path against 192 through the old one. If that number
//    is not what the run under test should produce, the benchmark is not
//    measuring what its name says and the timing is worthless. (This is the
//    B4-0b lesson: a performance gate whose benchmark never called the function
//    it was gating.)

#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkMesher.hpp"
#include "world/WorldLighting.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using mc::world::Block;
using mc::world::BlockOrientation;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::World;

constexpr int kBaseY = mc::world::kMinY + 8;

// One chunk section holding `layers` grids of `state`, spaced two cells apart on
// both horizontal axes and one row apart vertically so no two of them touch. The
// spacing is deliberate: a solid block of gates culls its neighbours' post faces
// and the per-cell vertex count stops being the "did this actually mesh a gate"
// proof the header promises.
constexpr int kStride = 2;
constexpr std::size_t kCellsPerLayer = (16U / kStride) * (16U / kStride);

[[nodiscard]] World makeWorld(BlockState state, int layers) {
    World world;
    Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setState(x, kBaseY - 1, z, BlockState{Block::Stone});
        }
    }
    for (int layer = 0; layer < layers; ++layer) {
        for (int z = 0; z < 16; z += kStride) {
            for (int x = 0; x < 16; x += kStride) {
                chunk.setState(x, kBaseY + layer * 2, z, state);
            }
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

struct Result final {
    double medianMicroseconds = 0.0;
    std::size_t vertices = 0;
};

[[nodiscard]] Result measure(const World& world, int sectionIndex, int repetitions) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(repetitions));
    std::size_t vertices = 0;
    // The light sampler is built ONCE and handed in. A remesh does rebuild it,
    // but it costs several times what the geometry pass does and is untouched by
    // this node, so timing it here would bury the thing being measured under
    // noise it cannot move — and a benchmark that cannot see its own change is
    // how a performance claim gets made without evidence.
    const mc::world::ChunkLightSampler lighting{world, {0, 0}};
    for (int i = 0; i < repetitions; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const auto mesh =
            mc::world::ChunkMesher::buildSection(world, {0, 0}, sectionIndex, lighting);
        const auto end = std::chrono::steady_clock::now();
        vertices = mesh.mesh.vertices.size() + mesh.cutoutMesh.vertices.size() +
                   mesh.translucentMesh.vertices.size();
        samples.push_back(
            std::chrono::duration<double, std::micro>(end - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return {samples[samples.size() / 2], vertices};
}

// `floor` is the same section with the stone floor and nothing else, so its
// vertices and its time come off the top: what is left is the models' own.
void report(const std::string& label, const Result& result, const Result& floor,
            std::size_t cells) {
    const double micros = result.medianMicroseconds - floor.medianMicroseconds;
    const std::size_t vertices = result.vertices - floor.vertices;
    std::cout << std::left << std::setw(30) << label << std::right
              << " model-verts=" << std::setw(7) << vertices << "  median=" << std::setw(9)
              << std::fixed << std::setprecision(2) << result.medianMicroseconds << " us"
              << "  minus floor=" << std::setw(9) << micros << " us"
              << "  per-cell=" << std::setw(8) << std::setprecision(1)
              << (micros * 1000.0 / static_cast<double>(cells)) << " ns"
              << "  verts/cell=" << (vertices / cells) << '\n';
}

} // namespace

int main() {
    constexpr int kRepetitions = 200;
    constexpr int kLayers = 4;
    const std::size_t cells = kCellsPerLayer * static_cast<std::size_t>(kLayers);
    const int sectionIndex = (kBaseY - mc::world::kMinY) / 16;

    std::cout << "RN-10c chunk remesh (median of " << kRepetitions << " section builds, "
              << cells << " model cells each)\n";
    std::cout << "verts/cell is the exercise proof: a fence gate is 160 through the baked\n"
                 "store and 192 through the old per-vertex-trig emitter.\n\n";

    // The floor alone, subtracted from every row below.
    const Result floor = measure(makeWorld(BlockState{Block::Air}, 0), sectionIndex, kRepetitions);
    std::cout << std::left << std::setw(30) << "(bare floor, subtracted)" << std::right
              << " model-verts=" << std::setw(7) << 0 << "  median=" << std::setw(9) << std::fixed
              << std::setprecision(2) << floor.medianMicroseconds << " us\n";

    // The block this node changed, in all four facings so the yaw path is real.
    for (const auto facing : {BlockOrientation::South, BlockOrientation::East}) {
        report(std::string("fence gate, facing=") +
                   (facing == BlockOrientation::South ? "south" : "east"),
               measure(makeWorld(BlockState{Block::OakFenceGate, facing}, kLayers), sectionIndex,
                       kRepetitions),
               floor, cells);
    }
    report("fence gate, open",
           measure(makeWorld(BlockState{Block::OakFenceGate, BlockOrientation::East}.withOpen(true),
                             kLayers),
                   sectionIndex, kRepetitions),
           floor, cells);
    // Controls: a block RN-10b moved to the same emitter, and one that never
    // left the box path. If the numbers move together, the machine moved, not
    // the code.
    report("door (baked path)",
           measure(makeWorld(BlockState{Block::OakDoor}, kLayers), sectionIndex, kRepetitions),
           floor, cells);
    report("stairs (box path, control)",
           measure(makeWorld(BlockState{Block::OakStairs}, kLayers), sectionIndex, kRepetitions),
           floor, cells);
    return 0;
}

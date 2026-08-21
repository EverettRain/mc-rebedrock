// W-5 benchmark (manual tool, not a ctest): the AC descending-power wavefront vs
// the naive serial relaxation (RedstoneWire.hpp's computeWireNetwork), on
// wire-heavy workloads. Both settle the whole network then emit the same
// downstream block updates, so the changed-cell count is identical — the win is
// compute: the serial relaxation resweeps the whole network O(diameter) times
// (~O(n^1.5) on a mesh, ~O(n^2) on a line fed from the far end), while the
// wavefront finalises every cell in one descending pass (O(n)). Reports relative
// per-solve microseconds and the speedup, workload-labelled, never an absolute
// target (W-DESIGN §6 honest calibre).

#include "gameplay/RedstoneWire.hpp"
#include "gameplay/RedstoneWireEvaluator.hpp"

#include "world/Block.hpp"
#include "world/BlockPos.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using mc::world::Block;
using mc::world::BlockPos;
using mc::world::BlockState;
using mc::world::World;
namespace redstone = mc::gameplay::redstone;

void set(World& world, BlockPos pos, Block block) {
    static_cast<void>(world.setState(pos.x, pos.y, pos.z, BlockState{block}));
}

World bigWorld() {
    World world;
    // Wide enough in x for a long straight line (up to x~416) and in z for the
    // mesh; a couple of chunks of margin on the low side for far-end sources.
    for (int cx = -1; cx <= 26; ++cx) {
        for (int cz = -1; cz <= 4; ++cz) {
            world.setChunk({cx, cz}, mc::world::Chunk{});
        }
    }
    return world;
}

[[nodiscard]] double medianMicros(std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void runWorkload(const std::string& name, const World& world, BlockPos start,
                 std::size_t cellHint) {
    constexpr int kReps = 200;
    redstone::WireNetworkEvaluator evaluator;

    // Warm both paths (arena buffers, caches).
    const auto serialWarm = redstone::computeWireNetwork(world, start);
    const auto& acWarm = evaluator.solve(world, start);
    const std::size_t cells = acWarm.size();

    std::vector<double> serialUs;
    std::vector<double> acUs;
    serialUs.reserve(kReps);
    acUs.reserve(kReps);
    std::uint64_t sink = 0;
    for (int r = 0; r < kReps; ++r) {
        auto t0 = std::chrono::steady_clock::now();
        const auto serial = redstone::computeWireNetwork(world, start);
        auto t1 = std::chrono::steady_clock::now();
        sink += serial.empty() ? 0U : static_cast<std::uint64_t>(serial.back().power);
        const auto& ac = evaluator.solve(world, start);
        auto t2 = std::chrono::steady_clock::now();
        sink += ac.empty() ? 0U : static_cast<std::uint64_t>(ac.back().power);
        serialUs.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        acUs.push_back(std::chrono::duration<double, std::micro>(t2 - t1).count());
    }
    // Consume the sink so neither solve is optimised away.
    if (sink == 0xFFFFFFFFFFFFFFFFULL) {
        std::printf("unreachable\n");
    }
    static_cast<void>(cellHint);

    const double serialMed = medianMicros(serialUs);
    const double acMed = medianMicros(acUs);
    std::printf("%-28s cells=%-5zu serial=%8.2f us   AC=%8.2f us   speedup=%5.1fx\n",
                name.c_str(), cells, serialMed, acMed, serialMed / acMed);
}

} // namespace

int main() {
    std::printf("W-5 AC wire evaluator vs naive serial relaxation (median of 200 solves)\n");
    std::printf("(both emit identical downstream block updates; this measures settle compute)\n\n");

    // Lines fed from the far end — the serial relaxation's worst case: power
    // crawls one cell per sweep against the discovery/iteration direction, so it
    // resweeps ~n times (O(n^2)) while the wavefront stays O(n). The gap widens
    // with n; two lengths make the asymptotic argument concrete.
    for (const int kLen : {120, 400}) {
        World world = bigWorld();
        for (int x = 0; x < kLen; ++x) {
            set(world, {x, 64, 0}, Block::RedstoneWire);
        }
        set(world, {kLen, 64, 0}, Block::RedstoneBlock); // far end
        char label[48];
        std::snprintf(label, sizeof(label), "line-%d (far-end source)", kLen);
        runWorkload(label, world, {0, 64, 0}, static_cast<std::size_t>(kLen));
    }

    // A dense mesh — a big connected sheet with a corner source.
    {
        World world = bigWorld();
        constexpr int kSide = 48;
        for (int x = 0; x < kSide; ++x) {
            for (int z = 0; z < kSide; ++z) {
                set(world, {x, 64, z}, Block::RedstoneWire);
            }
        }
        set(world, {-1, 64, 0}, Block::RedstoneBlock);
        runWorkload("mesh-48x48 (corner source)", world, {0, 64, 0},
                    static_cast<std::size_t>(kSide * kSide));
    }

    return 0;
}

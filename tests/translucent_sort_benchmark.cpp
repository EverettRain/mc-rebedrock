// RN-22：半透明逐 quad 重排的 Release 成本。
//
// 量的是重排本身：读排序状态、算距离平方、稳定排序、把 6 个一组的索引写回一条新缓冲。
// 不含 GPU 上传（那是一次 memcpy 加一次 vkCmdCopyBuffer，与网格上传同一条路径）、也不
// 含网格化。REGULAR §5-4 要的是相对指标，所以这里同时给出「建状态一次」与「重排一次」，
// 前者只在 section 上传时发生，后者才在热路径上。
//
// 三种 section 各量一次，因为 quad 数差两个量级：
//   稀疏  零星几块玻璃
//   水面  一整层 16x16 的水（最常见的大宗半透明 section）
//   最坏  整个 16^3 全是交替色染色玻璃，每一面都留着
//
// 跑法：cmake --build <release> --target mc_rebedrock_translucent_sort_benchmark

#include "render/MeshData.hpp"
#include "render/TranslucentSort.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/ChunkMesher.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

std::size_t checksum = 0;

template <class F> double medianMicros(F&& work, int inner) {
    std::array<double, 21> samples{};
    for (auto& sample : samples) {
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < inner; ++i) work();
        sample = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin)
                     .count() /
                 inner;
    }
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
}

using mc::world::Block;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::World;

[[nodiscard]] mc::render::RenderMeshData meshOf(const World& world) {
    return mc::world::ChunkMesher::buildSection(world, {0, 0}, 0);
}

[[nodiscard]] World sparseGlass() {
    World world;
    Chunk chunk;
    for (int i = 0; i < 8; ++i) {
        chunk.setState(2 + i, mc::world::kMinY + 1 + i, 2 + i, BlockState{Block::Glass});
    }
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

[[nodiscard]] World waterSheet() {
    World world;
    Chunk chunk;
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            chunk.setState(x, mc::world::kMinY + 1, z, BlockState{Block::Water});
            chunk.setFluidLevel(x, mc::world::kMinY + 1, z, 0U);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

[[nodiscard]] World worstCaseStainedGlass() {
    // 交替两色，所以内部面一个都不被同种跳过规则删掉 —— 这是半透明 section 能有的
    // 最多 quad 数，实际存档里不会出现，作为上界。
    World world;
    Chunk chunk;
    for (int y = 0; y < 16; ++y) {
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                chunk.setState(x, mc::world::kMinY + y, z,
                               BlockState{(x + y + z) % 2 == 0 ? Block::RedStainedGlass
                                                               : Block::BlueStainedGlass});
            }
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

void report(const char* label, const World& world) {
    const auto mesh = meshOf(world);
    const auto& translucent = mesh.translucentMesh;
    const auto state = mc::render::buildTranslucentSortState(translucent);
    if (state.empty()) {
        std::printf("%-28s 没有半透明几何\n", label);
        return;
    }
    std::vector<std::uint32_t> indices;
    // 预热容量：稳态下重排是零分配的，启动那次分配不算进去。
    mc::render::writeSortedTranslucentIndices(state, glm::vec3{0.0F}, indices);

    const int inner = state.quads.size() > 20000U ? 5 : 200;
    const double build = medianMicros(
        [&] {
            const auto rebuilt = mc::render::buildTranslucentSortState(translucent);
            checksum += rebuilt.quads.size();
        },
        inner);
    // 视点每次都换，免得分支预测器把一个固定的比较序列学会了。
    float phase = 0.0F;
    const double resort = medianMicros(
        [&] {
            phase += 0.37F;
            mc::render::writeSortedTranslucentIndices(state, glm::vec3{8.0F + phase, 40.0F, -30.0F},
                                                      indices);
            checksum += indices.size();
        },
        inner);

    std::printf("%-28s quad %6zu | 常驻 %6.1f KB | 建状态 %8.1f us | 重排一次 %8.2f us\n", label,
                state.quads.size(), static_cast<double>(state.residentBytes()) / 1024.0, build,
                resort);
}

// 「一次多贵」乘「一秒几次」才是这条改动的真实代价。第二个因数靠模拟一段行走量出来，
// 用的是**渲染器同一份**调度函数 `selectTranslucentResorts`，不是另抄一遍策略。
//
// 场景：一片湖 —— 视距内每个区块列各有一个含水面的 section，全在同一层 y。这是最常见
// 的大宗半透明几何，也是重排次数的现实上界（散落的玻璃比它少得多）。
void reportWalk(const char* label, int chunkRadius, float blocksPerSecond) {
    std::vector<mc::render::TranslucentResortCandidate> candidates;
    for (int x = -chunkRadius; x <= chunkRadius; ++x) {
        for (int z = -chunkRadius; z <= chunkRadius; ++z) {
            candidates.push_back({glm::ivec3{x, 3, z}, {}});
        }
    }
    std::vector<std::size_t> selected;
    std::size_t cursor = 0;
    // 相机在水面那一层里（y 落在 section 3 内），沿 +x 直线走。
    glm::vec3 eye{0.5F, 56.5F, 0.5F};
    glm::ivec3 lastBlock{0, 56, 0};
    constexpr int kFramesPerSecond = 60;
    constexpr int kSeconds = 10;
    const float step = blocksPerSecond / static_cast<float>(kFramesPerSecond);

    // 先跑一帧把所有象限初始化，否则第一帧会把全场都排一遍，那不是稳态。
    mc::render::selectTranslucentResorts(candidates, eye, true, cursor, selected);
    for (const std::size_t index : selected) {
        candidates[index].pointOfView =
            mc::render::translucencyPointOfViewOf(eye, candidates[index].sectionCoordinates);
    }

    std::size_t totalResorts = 0;
    std::size_t peakPerFrame = 0;
    for (int frame = 0; frame < kFramesPerSecond * kSeconds; ++frame) {
        eye.x += step;
        const glm::ivec3 block{static_cast<int>(std::floor(eye.x)),
                               static_cast<int>(std::floor(eye.y)),
                               static_cast<int>(std::floor(eye.z))};
        const bool changed = block != lastBlock;
        lastBlock = block;
        mc::render::selectTranslucentResorts(candidates, eye, changed, cursor, selected);
        for (const std::size_t index : selected) {
            candidates[index].pointOfView =
                mc::render::translucencyPointOfViewOf(eye, candidates[index].sectionCoordinates);
        }
        totalResorts += selected.size();
        peakPerFrame = std::max(peakPerFrame, selected.size());
    }
    std::printf("%-34s 半透明 section %4zu | 重排 %5.1f 次/秒 | 峰值 %3zu 次/帧\n", label,
                candidates.size(), static_cast<double>(totalResorts) / kSeconds, peakPerFrame);
}

} // namespace

int main() {
    std::printf("RN-22 半透明逐 quad 重排 —— 单次成本\n");
    report("稀疏玻璃", sparseGlass());
    report("一层 16x16 水面", waterSheet());
    report("最坏：16^3 交替染色玻璃", worstCaseStainedGlass());

    std::printf("\nRN-22 —— 典型移动下的重排频率（湖面，视距内每列一个水面 section）\n");
    // vanilla 的行走 4.317 格/秒、疾跑 5.612 格/秒。
    reportWalk("视距 8，行走 4.317 格/秒", 8, 4.317F);
    reportWalk("视距 8，疾跑 5.612 格/秒", 8, 5.612F);
    reportWalk("视距 16，疾跑 5.612 格/秒", 16, 5.612F);
    std::printf("\n（checksum %zu，防止优化器删掉被测代码）\n", checksum);
    return 0;
}

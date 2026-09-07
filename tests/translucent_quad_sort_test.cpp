// RN-22 第一条：半透明层的 quad 级排序。
//
// 混合不可交换，所以绘制顺序就是结果。此前地形只按 section 排序，section 内的 quad
// 保持网格化时的发射顺序（`buildSectionImpl` 的 y→z→x 三重循环），与相机无关 —— 于是
// 一格玻璃屋里远处那面墙被画在近处那面之上是必然的。
//
// 本文件钉三件事：
//
//   排序结果   重排后的索引，按 quad 中心到视点的距离，必须**由远及近**。
//   重排触发   26.1 `LevelRenderer.scheduleResort`（LevelRenderer.java:1018）的三条判据，
//              不是「27 格换格」这一条。见 checkResortTrigger 的注释。
//   不许抹平   重排只换 6 个索引一组的**先后**，不重建索引。AO 按分数翻的对角线
//              （ChunkMesher.cpp 的 kFlippedIndices）与水面背面的反向绕序都必须原样
//              活下来 —— 照 vanilla 那样按固定 `0,1,2,2,3,0` 重建会把两者一起抹掉。

#include "render/MeshData.hpp"
#include "render/TranslucentSort.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/ChunkMesher.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using mc::render::MeshData;
using mc::render::SortableQuad;
using mc::render::TranslucencyPointOfView;
using mc::world::Block;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::World;

void require(bool condition, const std::string& message, int line) {
    if (!condition) {
        throw std::runtime_error{"translucent_quad_sort_test line " + std::to_string(line) + ": " +
                                 message};
    }
}

#define REQUIRE(condition, message) require(condition, message, __LINE__)

[[nodiscard]] World worldWith(const std::vector<std::pair<glm::ivec3, BlockState>>& cells) {
    World world;
    Chunk chunk;
    for (const auto& [position, state] : cells) {
        chunk.setState(position.x, position.y, position.z, state);
        if (state.block() == Block::Water) {
            chunk.setFluidLevel(position.x, position.y, position.z, 0U);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

constexpr int kY = mc::world::kMinY + 1;

// 把一条索引缓冲按 6 个一组拆开，每组算出它那片 quad 的中心。
[[nodiscard]] std::vector<glm::vec3> quadCentresInDrawOrder(
    const MeshData& mesh, const std::vector<std::uint32_t>& indices) {
    std::vector<glm::vec3> centres;
    for (std::size_t group = 0; group + 5 < indices.size(); group += 6) {
        glm::vec3 centre{0.0F};
        const std::uint32_t base =
            *std::min_element(indices.begin() + static_cast<std::ptrdiff_t>(group),
                              indices.begin() + static_cast<std::ptrdiff_t>(group) + 6);
        for (std::uint32_t corner = 0U; corner < 4U; ++corner) {
            centre += mc::render::decodeLocalPosition(mesh.vertices[base + corner]);
        }
        centres.push_back(centre * 0.25F);
    }
    return centres;
}

// --- 一、排序结果 -------------------------------------------------------------

void checkSortedFarToNear() {
    // 沿 Z 摆一串交替颜色的染色玻璃：颜色不同，所以相邻面不会被同种跳过规则删掉，
    // 一串下来 quad 的深度是密的、彼此可分辨的。
    std::vector<std::pair<glm::ivec3, BlockState>> cells;
    for (int i = 0; i < 6; ++i) {
        cells.push_back({{8, kY, 6 + i},
                         BlockState{i % 2 == 0 ? Block::RedStainedGlass
                                               : Block::BlueStainedGlass}});
    }
    const auto mesh = mc::world::ChunkMesher::buildSection(worldWith(cells), {0, 0}, 0);
    const MeshData& translucent = mesh.translucentMesh;
    REQUIRE(!translucent.indices.empty(), "夹具没有产出半透明几何");

    const auto state = mc::render::buildTranslucentSortState(translucent);
    REQUIRE(!state.empty(), "半透明层建不出排序状态 —— 「每片 quad 独占 4 个连续顶点」"
                            "这个前提被破坏了，见 buildTranslucentSortState 的注释");
    REQUIRE(state.indexCount() == translucent.indices.size(), "排序状态的索引数与源不符");

    // 先确认**排序前**的顺序确实不是由远及近，否则这条断言证明不了任何事情
    // （发射顺序恰好正确的夹具会让排序看起来永远是对的）。
    const glm::vec3 viewpoint{8.5F, 1.5F, -8.0F};
    {
        const auto emitted = quadCentresInDrawOrder(translucent, translucent.indices);
        bool alreadySorted = true;
        for (std::size_t i = 1; i < emitted.size(); ++i) {
            alreadySorted = alreadySorted && glm::length(emitted[i - 1] - viewpoint) >=
                                                 glm::length(emitted[i] - viewpoint) - 1e-4F;
        }
        REQUIRE(!alreadySorted, "夹具的发射顺序碰巧已经是由远及近，换一个夹具 —— "
                                "否则排序断言恒真");
    }

    std::vector<std::uint32_t> sorted;
    mc::render::writeSortedTranslucentIndices(state, viewpoint, sorted);
    REQUIRE(sorted.size() == translucent.indices.size(), "重排后的索引数变了");

    const auto centres = quadCentresInDrawOrder(translucent, sorted);
    for (std::size_t i = 1; i < centres.size(); ++i) {
        const float previous = glm::length(centres[i - 1] - viewpoint);
        const float current = glm::length(centres[i] - viewpoint);
        REQUIRE(previous >= current - 1e-4F,
                "第 " + std::to_string(i) + " 片 quad 比它前一片更远（" +
                    std::to_string(current) + " > " + std::to_string(previous) +
                    "）—— 排序方向反了，混合会按由近及远叠加");
    }

    // 反过来站到另一侧：顺序必须整体翻过来，不是「碰巧一个方向对」。
    const glm::vec3 opposite{8.5F, 1.5F, 24.0F};
    std::vector<std::uint32_t> sortedOpposite;
    mc::render::writeSortedTranslucentIndices(state, opposite, sortedOpposite);
    const auto oppositeCentres = quadCentresInDrawOrder(translucent, sortedOpposite);
    REQUIRE(oppositeCentres.front().z < centres.front().z,
            "从另一侧看时最先画的仍是同一片 quad —— 排序没有跟着视点走");
}

// --- 二、不许抹平：索引组整组搬 -----------------------------------------------

void checkIndexGroupsArePermutedNotRebuilt() {
    // 水池：顶面有反向绕序的背面副本，AO 也会在某些面上翻对角线。两者都必须活着。
    std::vector<std::pair<glm::ivec3, BlockState>> cells;
    for (int x = 0; x < 3; ++x) {
        for (int z = 0; z < 3; ++z) {
            for (int y = 0; y < 2; ++y) {
                cells.push_back({{7 + x, kY + y, 7 + z}, BlockState{Block::Water}});
            }
        }
    }
    cells.push_back({{7, kY, 10}, BlockState{Block::Stone}});
    const auto mesh = mc::world::ChunkMesher::buildSection(worldWith(cells), {0, 0}, 0);
    const MeshData& translucent = mesh.translucentMesh;
    const auto state = mc::render::buildTranslucentSortState(translucent);
    REQUIRE(!state.empty(), "水池建不出排序状态");

    std::vector<std::uint32_t> sorted;
    mc::render::writeSortedTranslucentIndices(state, glm::vec3{2.0F, 6.0F, 2.0F}, sorted);

    // 原始的 6 元组集合与重排后的必须**逐组相同**（只是先后变了）。
    const auto groupsOf = [](const std::vector<std::uint32_t>& indices) {
        std::map<std::array<std::uint32_t, 6>, int> counts;
        for (std::size_t group = 0; group + 5 < indices.size(); group += 6) {
            std::array<std::uint32_t, 6> key{};
            for (std::size_t k = 0; k < 6; ++k) key[k] = indices[group + k];
            ++counts[key];
        }
        return counts;
    };
    REQUIRE(groupsOf(translucent.indices) == groupsOf(sorted),
            "重排改变了索引组的内容，不只是先后 —— 照 vanilla 按固定 0,1,2,2,3,0 重建"
            "会抹掉 AO 的对角线选择和水面背面的反向绕序");

    // 而且这个夹具里确实有非默认的组，否则上面那条断言接不住「重建」这种改法。
    bool sawNonDefaultGroup = false;
    for (const auto& [group, count] : groupsOf(translucent.indices)) {
        static_cast<void>(count);
        const std::uint32_t base = *std::min_element(group.begin(), group.end());
        const std::array<std::uint32_t, 6> canonical{base + 0U, base + 1U, base + 2U,
                                                     base + 2U, base + 3U, base + 0U};
        sawNonDefaultGroup = sawNonDefaultGroup || group != canonical;
    }
    REQUIRE(sawNonDefaultGroup,
            "夹具里没有一组非默认绕序的索引 —— 换个夹具，否则上一条断言恒真");
}

void checkStableForEqualKeys() {
    // 水面的正面与背面共面，键完全相等。稳定排序必须让它们保持发射顺序 ——
    // 这不只是对齐 vanilla 的 mergeSort，`block_preview --verify` 要求同一条命令两次
    // 运行逐字节相同，键相等时顺序不定会直接毁掉它。
    const auto mesh = mc::world::ChunkMesher::buildSection(
        worldWith({{{8, kY, 8}, BlockState{Block::Water}}}), {0, 0}, 0);
    const auto state = mc::render::buildTranslucentSortState(mesh.translucentMesh);
    std::vector<std::uint32_t> first;
    std::vector<std::uint32_t> second;
    mc::render::writeSortedTranslucentIndices(state, glm::vec3{8.5F, -4.0F, 8.5F}, first);
    mc::render::writeSortedTranslucentIndices(state, glm::vec3{8.5F, -4.0F, 8.5F}, second);
    REQUIRE(first == second, "同一视点两次排序结果不同 —— 排序不确定");

    // 共面的那两片必须紧挨着，且保持原来的先后。
    std::size_t frontPosition = first.size();
    std::size_t backPosition = first.size();
    for (std::size_t group = 0; group + 5 < first.size(); group += 6) {
        const std::uint32_t base =
            *std::min_element(first.begin() + static_cast<std::ptrdiff_t>(group),
                              first.begin() + static_cast<std::ptrdiff_t>(group) + 6);
        const auto normal = mc::render::decodeNormal(mesh.translucentMesh.vertices[base]);
        if (normal.y < 0.9F) continue;
        if (frontPosition == first.size()) {
            frontPosition = group;
        } else {
            backPosition = group;
        }
    }
    REQUIRE(backPosition == frontPosition + 6U,
            "水面的正反两片在重排后不相邻 —— 键相等的两片被拆开了，排序不稳定");
}

// --- 三、重排触发 -------------------------------------------------------------

void checkPointOfViewQuantisation() {
    // 26.1 TranslucencyPointOfView：相机所在 section 减去被排序的 section，每轴钳 [-1,1]。
    const glm::ivec3 section{0, 0, 0};
    REQUIRE(mc::render::translucencyPointOfViewOf(glm::vec3{8.0F, 8.0F, 8.0F}, section) ==
                (TranslucencyPointOfView{0, 0, 0}),
            "相机在 section 之内，三轴都该是 0");
    REQUIRE(mc::render::translucencyPointOfViewOf(glm::vec3{40.0F, 8.0F, 8.0F}, section) ==
                (TranslucencyPointOfView{1, 0, 0}),
            "相机在 +x 方向两个 section 外，钳到 +1");
    REQUIRE(mc::render::translucencyPointOfViewOf(glm::vec3{-1.0F, 8.0F, 8.0F}, section) ==
                (TranslucencyPointOfView{-1, 0, 0}),
            "x = -1 属于 section -1，不是 section 0 —— 向下取整，不是截断");
    REQUIRE(mc::render::translucencyPointOfViewOf(glm::vec3{-16.0F, 8.0F, 8.0F}, section) ==
                (TranslucencyPointOfView{-1, 0, 0}),
            "x = -16 是 section -1 的下边界");
    REQUIRE(mc::render::translucencyPointOfViewOf(glm::vec3{-17.0F, 8.0F, 8.0F}, section) ==
                (TranslucencyPointOfView{-1, 0, 0}),
            "更远也仍然钳在 -1");
}

void checkResortTrigger() {
    // 判据是 26.1 `LevelRenderer.scheduleResort`（LevelRenderer.java:1018）的**三条**，
    // 不是「27 格换格」这一条：
    //
    //   pointOfViewChanged                              → 排
    //   blockPosChanged && (isAxisAligned() || isNearby) → 排
    //
    // 后两条是 vanilla 对「同一 27 格内 quad 前后关系不变」这个说法的自我否定。它不成
    // 立：只要相机有任一轴落在该 section 的板层内（`axisAligned`），格内的移动就能把相
    // 机挪到两片 quad 之间，前后关系随之翻转。玩家站在水里、站在玻璃屋里，正是这一档，
    // 也正是这一轮要修的那个现场缺陷的场景。
    //
    // 于是「同格内不重排」只对**远处的斜向 section** 成立。下面按这个真实规则钉。

    const TranslucencyPointOfView diagonal{1, 1, 1};   // 三轴都不对齐，远处斜着看
    const TranslucencyPointOfView aligned{1, 0, 1};    // y 轴对齐
    const TranslucencyPointOfView elsewhere{1, 1, -1}; // 换了象限

    // 远处斜向：象限没变、相机没换格 → 不排。
    REQUIRE(!mc::render::shouldResortTranslucency(diagonal, diagonal, false, false),
            "象限没变、相机没换方块，远处斜向的 section 不该重排");
    // 远处斜向：相机换了方块但没换象限 → 仍然不排。这就是 27 格量化省下来的那些重排。
    REQUIRE(!mc::render::shouldResortTranslucency(diagonal, diagonal, true, false),
            "远处斜向的 section，格内换方块不该触发重排 —— 27 格量化的全部意义就在这里");
    // 换象限 → 必排，不管相机有没有换方块。
    REQUIRE(mc::render::shouldResortTranslucency(diagonal, elsewhere, false, false),
            "跨 27 格必须重排");
    // 轴对齐：换方块就要排。这一条是「27 格内不变」这个说法不成立的地方。
    REQUIRE(mc::render::shouldResortTranslucency(aligned, aligned, true, false),
            "轴对齐的 section，相机换方块就必须重排 —— 相机可能挪到了两片 quad 之间");
    REQUIRE(!mc::render::shouldResortTranslucency(aligned, aligned, false, false),
            "相机连方块都没换，轴对齐也不该重排");
    // 近处：换方块就要排，即使三轴都不对齐。
    REQUIRE(mc::render::shouldResortTranslucency(diagonal, diagonal, true, true),
            "近处的 section，相机换方块就必须重排");
}

void checkScheduler() {
    // 渲染器与 benchmark 共用的那份调度。这里钉的是它的**整体**行为，不是单条判据：
    // 站着不动一次都不排、跨格只排该排的、近处的一定被覆盖到。
    std::vector<mc::render::TranslucentResortCandidate> candidates;
    for (int x = -8; x <= 8; ++x) {
        for (int z = -8; z <= 8; ++z) {
            candidates.push_back({glm::ivec3{x, 3, z}, {}});
        }
    }
    std::vector<std::size_t> selected;
    std::size_t cursor = 0;
    glm::vec3 eye{0.5F, 56.5F, 0.5F};

    // 第一帧：所有象限都还是默认值 {0,0,0}，与真实象限不符的都要排。这是冷启动，
    // 不是稳态，跑几帧把它收敛掉。
    for (int frame = 0; frame < 200; ++frame) {
        mc::render::selectTranslucentResorts(candidates, eye, false, cursor, selected);
        for (const std::size_t index : selected) {
            candidates[index].pointOfView = mc::render::translucencyPointOfViewOf(
                eye, candidates[index].sectionCoordinates);
        }
    }

    // 稳态、相机纹丝不动：一次都不该排。
    mc::render::selectTranslucentResorts(candidates, eye, false, cursor, selected);
    REQUIRE(selected.empty(), "相机不动却还在重排 " + std::to_string(selected.size()) +
                                  " 个 section —— 调度空转，每帧都在做无用功");

    // 同一格内挪一点点（不跨方块）：仍然一次都不排。
    eye.x += 0.25F;
    mc::render::selectTranslucentResorts(candidates, eye, false, cursor, selected);
    REQUIRE(selected.empty(), "相机在同一个方块内移动不该触发任何重排");

    // 跨一个方块：近处的与轴对齐的要排，**远处的斜向 section 不该排**。
    eye.x += 1.0F;
    mc::render::selectTranslucentResorts(candidates, eye, true, cursor, selected);
    REQUIRE(!selected.empty(), "相机换了方块，近处的 section 必须重排");
    std::size_t nearbyResorted = 0;
    for (const std::size_t index : selected) {
        const glm::ivec3 delta = candidates[index].sectionCoordinates - glm::ivec3{0, 3, 0};
        const int chebyshev =
            std::max({std::abs(delta.x), std::abs(delta.y), std::abs(delta.z)});
        if (chebyshev <= mc::render::kTranslucentNearbySections) ++nearbyResorted;
    }
    REQUIRE(nearbyResorted > 0U, "近处的 section 一个都没被选中");

    // 这个夹具里所有 section 的 y 都与相机同层，所以三轴中 y 恒为 0 —— 全都是
    // `axisAligned`，跨格时全都该排。换一个 y 上错开的 section，它才是「远处斜向」。
    std::vector<mc::render::TranslucentResortCandidate> diagonal{
        {glm::ivec3{20, 20, 20}, {}}, {glm::ivec3{21, 20, 20}, {}}};
    std::size_t diagonalCursor = 0;
    for (int frame = 0; frame < 8; ++frame) {
        mc::render::selectTranslucentResorts(diagonal, eye, false, diagonalCursor, selected);
        for (const std::size_t index : selected) {
            diagonal[index].pointOfView = mc::render::translucencyPointOfViewOf(
                eye, diagonal[index].sectionCoordinates);
        }
    }
    eye.x += 1.0F;
    mc::render::selectTranslucentResorts(diagonal, eye, true, diagonalCursor, selected);
    REQUIRE(selected.empty(),
            "远处、三轴都不对齐的 section 在相机跨格时被重排了 —— 27 格量化没起作用，"
            "重排会退化成每格全排");
}

// --- 四、前提：每片 quad 独占 4 个连续顶点 ------------------------------------

void checkEveryTranslucentBlockMeshesToSortableQuads() {
    // `buildTranslucentSortState` 在前提不成立时整体退回不排序 —— 那是安全的降级，
    // 但会静默。把全体半透明方块过一遍，让「新增半透明块破坏了前提」红在这里。
    std::vector<Block> translucentBlocks;
    for (std::size_t raw = 0; raw < static_cast<std::size_t>(Block::Count); ++raw) {
        const auto block = static_cast<Block>(raw);
        if (block == Block::Air) continue;
        if (mc::world::blockDefinition(block).renderLayer !=
            mc::world::BlockRenderLayer::Translucent) {
            continue;
        }
        translucentBlocks.push_back(block);
    }
    REQUIRE(translucentBlocks.size() >= 19U,
            "半透明方块只找到 " + std::to_string(translucentBlocks.size()) +
                " 个（玻璃 + 16 色染色玻璃 + 冰 + 水 至少 19）—— 名单读错了");

    for (const Block block : translucentBlocks) {
        // 两格同种加一格异种，把「同种跳过」和「异种保留」两条路径都走到。
        const auto mesh = mc::world::ChunkMesher::buildSection(
            worldWith({{{8, kY, 8}, BlockState{block}},
                       {{8, kY, 9}, BlockState{block}},
                       {{8, kY, 10}, BlockState{Block::Glass}}}),
            {0, 0}, 0);
        const auto state = mc::render::buildTranslucentSortState(mesh.translucentMesh);
        REQUIRE(state.indexCount() == mesh.translucentMesh.indices.size(),
                std::string{mc::world::blockDefinition(block).displayName} +
                    " 的半透明网格建不出排序状态：它有一片 quad 不是「4 个连续顶点 + 6 个"
                    "索引」。要么改回那个形状，要么改 buildTranslucentSortState —— 别让它"
                    "静默退回不排序。");
    }
}

} // namespace

int main() {
    try {
        checkSortedFarToNear();
        checkIndexGroupsArePermutedNotRebuilt();
        checkStableForEqualKeys();
        checkPointOfViewQuantisation();
        checkResortTrigger();
        checkScheduler();
        checkEveryTranslucentBlockMeshesToSortableQuads();
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}

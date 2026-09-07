#pragma once

// RN-22 第一条：半透明层的 quad 级排序。
//
// 半透明面靠 alpha 混合叠加，混合不可交换，所以**绘制顺序就是结果**。此前地形只按
// section 排序（`WorldRenderer.hpp` 的 `visibleTranslucentMeshes`），section 内的
// quad 保持网格化时的发射顺序 —— 那是 `buildSectionImpl` 的 y→z→x 三重循环，与相机
// 无关。配上 `depthWriteEnable = VK_FALSE`（先画的挡不住后画的），一格玻璃屋里远处
// 那面墙被画在近处那面之上是必然的，不是偶发。
//
// 26.1 的做法照搬在这里：网格是四边形流，排序**不动顶点，只重排索引**
// （`MeshData.sortQuads` / `MeshData.SortState.buildSortedIndexBuffer`）。
//
// 三处与 vanilla 不同的地方，都写在这里免得下次重新怀疑：
//
// 1. **索引组整组搬，不按固定 `0,1,2,2,3,0` 重建。** vanilla 的
//    `buildSortedIndexBuffer` 写死了那个模式，因为它的四边形永远是那个绕法。本作的
//    `ChunkMesher` 会按 AO 分数把对角线翻成 `0,1,3,1,2,3`（ChunkMesher.cpp:1285/1589
//    的 `kFlippedIndices`），流体水面的背面还是**反向绕序**的一组。照 vanilla 重建
//    索引会把这两件事一起抹平——AO 的对角线选择没了，水面背面被剔回去了。所以这里存
//    的是每片 quad 自己那 6 个索引的**局部编号**，排序只换组的先后。
//
// 2. **quad 中心取四角平均，不取 0/2 号顶点的中点。** 对平面四边形两者恒等（对角线
//    中点 == 四角平均），但四角平均不依赖「哪两个顶点是对角」这个随模式而变的前提。
//
// 3. **稳定排序。** vanilla 用的 `IntArrays.mergeSort` 是稳定的；这里必须也稳定，
//    而且理由更硬：`block_preview --verify` 的验收条件是同一条命令两次运行**逐字节
//    相同**（[[block-preview-export-tool]]），键相等时顺序不定就会毁掉它。

#include "render/MeshData.hpp"

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace mc::render {

// 一片四边形在排序状态里的样子：中心（section 局部坐标）加它自己那 6 个索引相对
// 组内首顶点的编号。24 字节，顶点数据一个字节都不留。
struct SortableQuad final {
    glm::vec3 centre{};
    std::uint32_t baseVertex = 0U;
    std::array<std::uint8_t, 6> localIndices{};
};

// 一个 section 的半透明层重排所需的全部信息。顶点缓冲上传后就不再需要 CPU 副本，
// 重排只重写索引，所以这里刻意**不**留顶点。
//
// 对齐 vanilla 的 `MeshData.SortState`：它同样只留质心，不留顶点。
struct TranslucentSortState final {
    std::vector<SortableQuad> quads;

    [[nodiscard]] bool empty() const { return quads.empty(); }
    [[nodiscard]] std::size_t indexCount() const { return quads.size() * 6U; }
    [[nodiscard]] std::size_t residentBytes() const {
        return quads.capacity() * sizeof(SortableQuad);
    }
};

// 从半透明层的索引缓冲里读出每片 quad。
//
// 前提是「索引每 6 个一组、组内恰好 4 个不同的顶点、且这 4 个顶点编号连续」——本作
// 半透明层只有两个发射点（`appendFace` 走玻璃/冰/染色玻璃，`appendWaterFace` 走水），
// 两者都严格 4 顶点 + 6 索引、不与别的 quad 共享顶点。前提不成立时**整体退回不排序**
// （返回空状态），绘制顺序还是发射顺序 —— 那是今天的行为，不会更糟。
// `translucent_quad_sort_test` 拿全体半透明方块把这个前提钉住，新增半透明块破坏它时
// 会红在那里，而不是在这里静默降级。
[[nodiscard]] inline TranslucentSortState buildTranslucentSortState(const MeshData& mesh) {
    TranslucentSortState state;
    if (mesh.indices.size() % 6U != 0U) {
        return state;
    }
    state.quads.reserve(mesh.indices.size() / 6U);
    for (std::size_t group = 0; group < mesh.indices.size(); group += 6U) {
        std::uint32_t minimum = mesh.indices[group];
        std::uint32_t maximum = mesh.indices[group];
        for (std::size_t k = 1U; k < 6U; ++k) {
            minimum = std::min(minimum, mesh.indices[group + k]);
            maximum = std::max(maximum, mesh.indices[group + k]);
        }
        if (maximum - minimum != 3U || maximum >= mesh.vertices.size()) {
            return {};
        }
        SortableQuad quad{};
        quad.baseVertex = minimum;
        bool present[4]{};
        for (std::size_t k = 0U; k < 6U; ++k) {
            const auto local = static_cast<std::uint8_t>(mesh.indices[group + k] - minimum);
            quad.localIndices[k] = local;
            present[local] = true;
        }
        if (!(present[0] && present[1] && present[2] && present[3])) {
            return {};
        }
        // 四角平均。对平面四边形等同于 vanilla 的对角线中点，但不依赖绕序模式。
        glm::vec3 centre{0.0F};
        for (std::uint32_t corner = 0U; corner < 4U; ++corner) {
            centre += decodeLocalPosition(mesh.vertices[minimum + corner]);
        }
        quad.centre = centre * 0.25F;
        state.quads.push_back(quad);
    }
    return state;
}

// 按到 `viewpoint`（section 局部坐标下的相机位置）的距离由远及近重排索引。
//
// 键是距离**平方**，与 vanilla 的 `VertexSorting.byDistance` 一致：单调变换，省一次
// 开方。`stable_sort` 的理由见文件头第 3 条。
inline void writeSortedTranslucentIndices(const TranslucentSortState& state,
                                          const glm::vec3& viewpoint,
                                          std::vector<std::uint32_t>& indices) {
    indices.clear();
    if (state.quads.empty()) {
        return;
    }
    // 每帧要跑很多次，暂存表复用容量而不是每次分配（REGULAR §2 的零分配热路径）。
    // 排序只在渲染线程发生，thread_local 是为了万一将来搬去工作线程时不必再回来改。
    static thread_local std::vector<std::pair<float, std::uint32_t>> order;
    order.clear();
    order.reserve(state.quads.size());
    for (std::size_t i = 0; i < state.quads.size(); ++i) {
        const glm::vec3 offset = state.quads[i].centre - viewpoint;
        order.emplace_back(glm::dot(offset, offset), static_cast<std::uint32_t>(i));
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const std::pair<float, std::uint32_t>& first,
                        const std::pair<float, std::uint32_t>& second) {
                         return first.first > second.first;
                     });
    indices.resize(state.quads.size() * 6U);
    std::size_t write = 0;
    for (const auto& [key, quadIndex] : order) {
        static_cast<void>(key);
        const SortableQuad& quad = state.quads[quadIndex];
        for (std::size_t k = 0U; k < 6U; ++k) {
            indices[write++] = quad.baseVertex + quad.localIndices[k];
        }
    }
}

// 26.1 的 `TranslucencyPointOfView`（TranslucencyPointOfView.java）逐字移植：相机在
// 哪个 section，减去被排序的那个 section，每轴钳在 [-1, 1]。三轴各三档，一共 27 格。
//
// 它回答的不是「相机动了多少」而是「相机相对这个 section 换象限了没有」。同一格内
// **不**保证 quad 前后关系不变——vanilla 自己也不这么认为，见 `axisAligned` 的注释。
struct TranslucencyPointOfView final {
    int x = 0;
    int y = 0;
    int z = 0;

    [[nodiscard]] bool operator==(const TranslucencyPointOfView&) const = default;

    // 有任一轴的量化值为 0，就是说相机落在这个 section 沿该轴的板层之内。此时格内的
    // 移动**确实**会翻转 quad 的前后关系（相机在两片 quad 中间挪过去了），所以 vanilla
    // 对这种 section 退回按方块位置重排，而不是信 27 格。
    // 玩家站在水里、站在玻璃屋里，正是这一档。
    [[nodiscard]] bool axisAligned() const { return x == 0 || y == 0 || z == 0; }
};

// 一个坐标轴上的量化。`sectionCoordinate` 是被排序 section 在该轴的 section 号。
[[nodiscard]] inline int translucencyPointOfViewAxis(float cameraCoordinate,
                                                     int sectionCoordinate) {
    // 向下取整的除法：section 号在负半轴也必须是「往下取」，否则 y = -0.5 与 y = 0.5
    // 会落进同一个 section。
    const auto blockCoordinate = static_cast<int>(std::floor(cameraCoordinate));
    const int cameraSection = blockCoordinate >= 0 ? blockCoordinate / 16
                                                   : -(((-blockCoordinate) + 15) / 16);
    return std::clamp(cameraSection - sectionCoordinate, -1, 1);
}

[[nodiscard]] inline TranslucencyPointOfView translucencyPointOfViewOf(
    const glm::vec3& camera, const glm::ivec3& sectionCoordinates) {
    return {translucencyPointOfViewAxis(camera.x, sectionCoordinates.x),
            translucencyPointOfViewAxis(camera.y, sectionCoordinates.y),
            translucencyPointOfViewAxis(camera.z, sectionCoordinates.z)};
}

// 26.1 `LevelRenderer.scheduleResort`（LevelRenderer.java:1018）的判据，原样。
//
// 三条，不是一条：27 格换格必排；此外只要相机换了**方块**，轴对齐的 section 与近处的
// section 也要排。后两条是 vanilla 对「同格内顺序不变」这个说法的自我否定——它只在
// 相机三轴都不与该 section 对齐（斜着看、且不近）时才敢信 27 格。
[[nodiscard]] inline bool shouldResortTranslucency(const TranslucencyPointOfView& previous,
                                                   const TranslucencyPointOfView& current,
                                                   bool cameraBlockChanged, bool nearby) {
    if (previous != current) {
        return true;
    }
    return cameraBlockChanged && (current.axisAligned() || nearby);
}

// 一个有半透明几何的 section，在调度眼里的全部样子。
struct TranslucentResortCandidate final {
    glm::ivec3 sectionCoordinates{};
    // 上次排序时的视点象限。被选中的那些由调用方在真正重排后更新。
    TranslucencyPointOfView pointOfView{};
};

// 「近处」的判据：与相机所在 section 的切比雪夫距离。26.1
// `SectionOcclusionGraph.addSectionsInFrustum(frustum, ..., 32)` 里那个 32 是格数，
// 即两个 section。
inline constexpr int kTranslucentNearbySections = 2;

// 本帧要重排哪些 section。26.1 `LevelRenderer.scheduleTranslucentSectionResort`
// （LevelRenderer.java:997）的调度，抽成纯函数。
//
// 抽出来是为了它能被**同一份代码**量和测：渲染器那边还要管缓冲与拷贝，benchmark 只想知道
// 「典型移动下一秒重排几次」。策略在两处各写一遍就等于量的是另一套策略。
//
// 两趟：近处的全查一遍，其余按轮询切一片（配额 max(N/8, 15)，vanilla 的数）。同一个
// section 被两趟同时选中只算一次。
inline void selectTranslucentResorts(std::span<const TranslucentResortCandidate> candidates,
                                     const glm::vec3& eye, bool cameraBlockChanged,
                                     std::size_t& roundRobinCursor,
                                     std::vector<std::size_t>& selected) {
    selected.clear();
    if (candidates.empty()) {
        roundRobinCursor = 0;
        return;
    }
    const glm::ivec3 eyeSection{static_cast<int>(std::floor(eye.x / 16.0F)),
                                static_cast<int>(std::floor(eye.y / 16.0F)),
                                static_cast<int>(std::floor(eye.z / 16.0F))};
    // 已选标记。逐帧重建一张位表比往 selected 里线性查找便宜，也比 set 便宜。
    static thread_local std::vector<bool> chosen;
    chosen.assign(candidates.size(), false);

    const auto consider = [&](std::size_t index, bool nearby) {
        if (chosen[index]) {
            return;
        }
        const TranslucentResortCandidate& candidate = candidates[index];
        const auto pointOfView =
            translucencyPointOfViewOf(eye, candidate.sectionCoordinates);
        if (!shouldResortTranslucency(candidate.pointOfView, pointOfView, cameraBlockChanged,
                                      nearby)) {
            return;
        }
        chosen[index] = true;
        selected.push_back(index);
    };

    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const glm::ivec3 delta = candidates[index].sectionCoordinates - eyeSection;
        const int chebyshev =
            std::max({std::abs(delta.x), std::abs(delta.y), std::abs(delta.z)});
        if (chebyshev <= kTranslucentNearbySections) {
            consider(index, true);
        }
    }
    std::size_t budget = std::max<std::size_t>(candidates.size() / 8U, 15U);
    roundRobinCursor %= candidates.size();
    while (budget-- > 0U) {
        const std::size_t index = roundRobinCursor;
        roundRobinCursor = (roundRobinCursor + 1U) % candidates.size();
        consider(index, false);
    }
}

} // namespace mc::render

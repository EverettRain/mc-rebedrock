#pragma once

// The selection-box wireframe's geometry contract: what the lines ARE, and how
// far the shader is allowed to move them.
//
// RN-13-2 settled the second half. The defect then was a size-proportional
// expansion in block_outline.vert —
//
//     local = boxCenter + (local - boxCenter) * 1.02;
//
// a 2% expansion **about the box centre**, so the offset grew with the box: a
// full cube's edges sat 0.01 blocks proud, a 2px diode's base 0.00125. Vanilla's
// ShapeRenderer.renderShape emits its edge coordinates untouched and separates
// the line from the surface in the DEPTH domain instead — RenderTypes.LINES
// carries LayeringTransform.VIEW_OFFSET_Z_LAYERING, which for a perspective
// projection is `modelViewStack.scale(1 - 1/4096)` (ProjectionType.java:7), i.e.
// the vertex is pulled a four-thousandth of its camera distance toward the eye.
// That half has not changed: the endpoint IS the coordinate, and the depth nudge
// is camera-relative, never geometry-relative.
//
// RN-16 settles the first half, which RN-10f got wrong in the other direction.
// RN-10f replaced "one box around the whole shape" with "one wireframe per box
// of the shape", and a wireframe per box draws the seams where two boxes MEET —
// a stair grew a horizontal line across its back and both its sides at y=0.5,
// where its two boxes are flush and vanilla draws nothing.
//
// Vanilla does not outline box by box either. ShapeRenderer.renderShape calls
// `VoxelShape.forAllEdges`, and a block's shape is a `Shapes.or(...)` of its
// boxes — a MERGED DiscreteVoxelShape, one occupancy grid over the coordinate
// planes the boxes contribute. `DiscreteVoxelShape.forAllAxisEdges` then walks
// every grid line and emits a segment only where the occupancy of the four cells
// around that line actually changes, so an interior seam emits nothing. The
// stair's front does keep its line at y=0.5 — there the lower box reaches z=1
// and the upper one does not, so that line is a real silhouette boundary, not a
// seam. `outlineEdgesOf` below is that algorithm, transcribed.
//
// The merging is vanilla's too: `forAllEdges` passes `mergeNeighbors = true`, so
// a run of collinear emitted grid segments comes out as ONE segment. That is
// what keeps a full cube at twelve lines instead of one per grid cell.

#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mc::render {

// RN-45：vanilla 的描边**不是线**。
//
// `assets/minecraft/shaders/core/rendertype_lines.vsh`（26.1 资源包里的原文）把每条棱在
// 顶点着色器里撑成一个**屏幕空间四边形**：顶点格式 POSITION_COLOR_NORMAL_LINE_WIDTH 的
// Normal 装的是这条棱的方向（ShapeRenderer.renderShape 逐棱写），图元模式 LINES 的索引数
// 是 `vertexCount / 4 * 6`（VertexFormat.java:210，与 QUADS 同一条）——每条棱 4 个顶点、
// 两个三角形。着色器把起点与「起点 + 棱方向」各投影一次求出屏幕方向，取它的法向乘
// `LineWidth / ScreenSize`，按 gl_VertexID 的奇偶推向两侧。
//
// 这一条同时解释了本作此前的两个症状：
//
//   * RN-39 的虚线——线光栅化取像素中心，而覆盖同一条棱的三角形在半个像素之外采样。
//     vanilla 两边都是三角形，取的是同一批采样点。
//   * MSAA 下仍旧闪（RN-44 §4）——一条 1 像素宽的 GL 线在 2x 靶上平均只盖住每像素两个
//     采样点中的一个，resolve 之后是半透明的，亮度逐像素起伏。四边形有真实覆盖率。
//
// ★ 深度推近量也抄漏了一半。RN-39 抄的是 LayeringTransform.VIEW_OFFSET_Z_LAYERING →
// `ProjectionType.PERSPECTIVE` 的 `scale(1 - bias/4096)`（ProjectionType.java:7），
// 但 `rendertype_lines.vsh` **自己还有一份** `VIEW_SHRINK = 1 - 1/256`，两者**相乘**。
// vanilla 实际约 1/241，我们当时落地的 1/1024 比它保守四倍——「我们比 vanilla 激进」
// 那句记录是反的（RN-39 §4 已标注更正）。这里取两项的乘积，与 vanilla 逐位相同。
inline constexpr float kOutlineViewShrink = (1.0F - 1.0F / 256.0F) * (1.0F - 1.0F / 4096.0F);

// 线宽（像素），`Window.getAppropriateLineWidth`：`max(2.5, 宽度 / 1920 * 2.5)`。
// 1920 以下恒为 2.5 像素，再宽才按比例长——高 DPI 上描边不会退化成一根细丝。
[[nodiscard]] inline constexpr float outlineLineWidthPixels(float framebufferWidth) {
    const float scaled = framebufferWidth / 1920.0F * 2.5F;
    return scaled > 2.5F ? scaled : 2.5F;
}

// 一条棱是两个三角形。着色器没有顶点缓冲，端点与推向哪一侧都按 gl_VertexIndex 查表，
// 所以这个数与 block_outline.vert 的两张表必须同源。
inline constexpr std::uint32_t kOutlineSegmentVertexCount = 6U;

// An axis-aligned box in block-local (0..1) coordinates. Mirrors
// `world::BlockBounds` without depending on it: this header is pure geometry and
// `block_outline_geometry_test` exercises it on hand-written box sets that no
// block in the roster produces.
struct OutlineBox final {
    glm::vec3 minimum{0.0F};
    glm::vec3 maximum{1.0F};
};

struct OutlineSegment final {
    glm::vec3 start{0.0F};
    glm::vec3 end{0.0F};

    [[nodiscard]] bool operator==(const OutlineSegment&) const = default;
};

// The roster's widest shape is five boxes (`world::kMaxSelectionBoxes`, a wall's
// post plus four arms). Each box contributes two planes per axis, so the merged
// grid is at most ten planes and nine cells on a side.
inline constexpr std::size_t kMaxOutlineBoxes = 5;
inline constexpr std::size_t kMaxOutlinePlanes = 2U * kMaxOutlineBoxes;

// The output cap. A full cube emits 12 and the roster's worst case is far below
// this; `block_outline_geometry_test` walks every state of every block and
// asserts none is truncated, so a future shape that would overflow fails a test
// rather than losing lines on screen.
inline constexpr std::size_t kMaxOutlineSegments = 128;

struct OutlineEdges final {
    std::array<OutlineSegment, kMaxOutlineSegments> segments{};
    std::size_t count = 0;
    // Set when the cap above was reached. Never silently drop the rest: a shape
    // that outgrows the cap is a change to the roster, and a half-drawn outline
    // is exactly the kind of thing nobody reports.
    bool truncated = false;
};

namespace detail {

// The distinct coordinate planes one axis contributes, sorted. This is
// `Shapes.or`'s coordinate list: the merged shape's grid lines.
struct OutlinePlanes final {
    std::array<float, kMaxOutlinePlanes> values{};
    std::size_t count = 0;
};

[[nodiscard]] inline OutlinePlanes outlinePlanesOf(std::span<const OutlineBox> boxes,
                                                   int axis) {
    OutlinePlanes planes;
    const auto push = [&planes](float value) {
        for (std::size_t i = 0; i < planes.count; ++i) {
            // Exact equality on purpose. Every coordinate in the roster comes
            // from the same `n/16` constants in BlockShape, so two boxes that
            // share a plane share the bits; an epsilon here would instead fuse
            // two planes that are genuinely a pixel apart and erase a real edge.
            if (planes.values[i] == value) {
                return;
            }
        }
        if (planes.count < planes.values.size()) {
            planes.values[planes.count++] = value;
        }
    };
    for (const OutlineBox& box : boxes) {
        push(box.minimum[axis]);
        push(box.maximum[axis]);
    }
    std::sort(planes.values.begin(), planes.values.begin() +
                                         static_cast<std::ptrdiff_t>(planes.count));
    return planes;
}

} // namespace detail

// `Shapes.or(boxes...)` followed by `VoxelShape.forAllEdges`: the outline of the
// UNION, not of each box.
//
// The grid is the boxes' own coordinate planes (at most ten per axis), a cell is
// occupied when its centre lies inside some box, and a grid line emits a segment
// exactly where `DiscreteVoxelShape.forAllAxisEdges` would — one or three of the
// four surrounding cells full, or two of them diagonally opposite (a saddle).
// Two ADJACENT full cells is a flat surface, and a flat surface has no edge:
// that single case is the whole difference between this and outlining box by
// box.
[[nodiscard]] inline OutlineEdges outlineEdgesOf(std::span<const OutlineBox> boxes) {
    OutlineEdges edges;
    if (boxes.empty()) {
        return edges;
    }
    const std::array<detail::OutlinePlanes, 3> planes{detail::outlinePlanesOf(boxes, 0),
                                                      detail::outlinePlanesOf(boxes, 1),
                                                      detail::outlinePlanesOf(boxes, 2)};
    std::array<std::size_t, 3> cells{};
    for (int axis = 0; axis < 3; ++axis) {
        // A box with zero extent on an axis contributes one plane and no cell.
        cells[static_cast<std::size_t>(axis)] =
            planes[static_cast<std::size_t>(axis)].count == 0
                ? 0U
                : planes[static_cast<std::size_t>(axis)].count - 1U;
    }
    if (cells[0] == 0U || cells[1] == 0U || cells[2] == 0U) {
        return edges; // a degenerate shape has no volume and therefore no edges
    }

    // Occupancy of the merged grid. `DiscreteVoxelShape.isFull`, resolved once.
    constexpr std::size_t kMaxCells = kMaxOutlinePlanes - 1U;
    std::array<bool, kMaxCells * kMaxCells * kMaxCells> full{};
    const auto cellIndex = [](std::size_t x, std::size_t y, std::size_t z) {
        return (x * kMaxCells + y) * kMaxCells + z;
    };
    for (std::size_t x = 0; x < cells[0]; ++x) {
        for (std::size_t y = 0; y < cells[1]; ++y) {
            for (std::size_t z = 0; z < cells[2]; ++z) {
                const glm::vec3 centre{
                    (planes[0].values[x] + planes[0].values[x + 1]) * 0.5F,
                    (planes[1].values[y] + planes[1].values[y + 1]) * 0.5F,
                    (planes[2].values[z] + planes[2].values[z + 1]) * 0.5F};
                bool inside = false;
                for (const OutlineBox& box : boxes) {
                    if (centre.x > box.minimum.x && centre.x < box.maximum.x &&
                        centre.y > box.minimum.y && centre.y < box.maximum.y &&
                        centre.z > box.minimum.z && centre.z < box.maximum.z) {
                        inside = true;
                        break;
                    }
                }
                full[cellIndex(x, y, z)] = inside;
            }
        }
    }

    // `isFullWide`: outside the grid reads empty, which is what makes the outer
    // hull emit at all.
    const auto isFullWide = [&](std::ptrdiff_t x, std::ptrdiff_t y, std::ptrdiff_t z) {
        if (x < 0 || y < 0 || z < 0) return false;
        if (static_cast<std::size_t>(x) >= cells[0] || static_cast<std::size_t>(y) >= cells[1] ||
            static_cast<std::size_t>(z) >= cells[2]) {
            return false;
        }
        return full[cellIndex(static_cast<std::size_t>(x), static_cast<std::size_t>(y),
                              static_cast<std::size_t>(z))];
    };

    const auto emit = [&edges](const glm::vec3& start, const glm::vec3& end) {
        if (edges.count >= edges.segments.size()) {
            edges.truncated = true;
            return;
        }
        edges.segments[edges.count++] = {start, end};
    };

    // `forAllAxisEdges`, once per axis the edge can run along. `along` is that
    // axis; `first`/`second` are the two the grid line is indexed by.
    for (int along = 0; along < 3; ++along) {
        const auto a0 = static_cast<std::size_t>((along + 1) % 3);
        const auto a1 = static_cast<std::size_t>((along + 2) % 3);
        const auto ac = static_cast<std::size_t>(along);
        for (std::size_t i = 0; i <= cells[a0]; ++i) {
            for (std::size_t j = 0; j <= cells[a1]; ++j) {
                // -1 means "no run open". Vanilla's `lastStart`.
                std::ptrdiff_t runStart = -1;
                for (std::size_t k = 0; k <= cells[ac]; ++k) {
                    int fullSectors = 0;
                    int oddSectors = 0;
                    for (int di = 0; di <= 1; ++di) {
                        for (int dj = 0; dj <= 1; ++dj) {
                            std::array<std::ptrdiff_t, 3> probe{};
                            probe[a0] = static_cast<std::ptrdiff_t>(i) + di - 1;
                            probe[a1] = static_cast<std::ptrdiff_t>(j) + dj - 1;
                            probe[ac] = static_cast<std::ptrdiff_t>(k);
                            if (isFullWide(probe[0], probe[1], probe[2])) {
                                ++fullSectors;
                                oddSectors ^= di ^ dj;
                            }
                        }
                    }
                    // One or three full cells is a silhouette corner; two full
                    // cells is an edge only when they are diagonally opposite
                    // (a saddle) — two ADJACENT full cells is a flat surface,
                    // and drawing that is exactly the interior seam RN-10f's
                    // per-box outlining drew.
                    const bool onEdge = fullSectors == 1 || fullSectors == 3 ||
                                        (fullSectors == 2 && (oddSectors & 1) == 0);
                    if (onEdge) {
                        if (runStart < 0) {
                            runStart = static_cast<std::ptrdiff_t>(k);
                        }
                        continue;
                    }
                    if (runStart < 0) {
                        continue;
                    }
                    glm::vec3 start{};
                    glm::vec3 end{};
                    start[static_cast<int>(a0)] = planes[a0].values[i];
                    start[static_cast<int>(a1)] = planes[a1].values[j];
                    start[static_cast<int>(ac)] =
                        planes[ac].values[static_cast<std::size_t>(runStart)];
                    end = start;
                    end[static_cast<int>(ac)] = planes[ac].values[k];
                    emit(start, end);
                    runStart = -1;
                }
            }
        }
    }
    return edges;
}

} // namespace mc::render

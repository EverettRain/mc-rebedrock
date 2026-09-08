// RN-16 / RN-13-2: the selection wireframe's geometry.
//
// Two defects, one file. RN-13-2's was a size-proportional expansion in
// block_outline.vert (`local = boxCenter + (local - boxCenter) * 1.02`) that put
// a full cube's edges 0.01 blocks proud and a diode's base an eighth of that.
// RN-16's is the other half of the same line: RN-10f started outlining BOX BY
// BOX, so every place two boxes of a shape sit flush got a line drawn along the
// seam — a stair grew a horizontal bar across its back and its two sides at
// y=0.5. Vanilla draws neither: `ShapeRenderer.renderShape` calls
// `VoxelShape.forAllEdges`, which walks the MERGED occupancy grid
// (`DiscreteVoxelShape.forAllAxisEdges`) and emits only where occupancy changes.
//
// Headless cannot look at a wireframe, so what is pinned here is:
//
//   * the edge set itself, against an INDEPENDENT geometric predicate — not the
//     grid the algorithm walks, but epsilon-probes of the union around each
//     candidate line. That is what makes "the back seam is gone and the front
//     line at y=0.5 is still there" an assertion rather than a claim;
//   * that merging happened (a full cube is twelve lines, not one per grid
//     cell);
//   * that the shader computes the endpoint and nothing else — its main() may
//     carry no constant but the homogeneous 1.0 and the named, checked view-space
//     nudge. That second half is where RN-13-2's defect actually lived, so it is
//     asserted against the shader source directly, the way item_cube_uv_test
//     asserts the item shaders' UV literals.

#include "render/BlockOutlineGeometry.hpp"
#include "world/Block.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/BlockStateTable.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef MC_REBEDROCK_SHADER_SRC_DIR
#error "MC_REBEDROCK_SHADER_SRC_DIR must point at resources/shaders/src"
#endif

namespace {

using mc::render::OutlineBox;
using mc::render::OutlineEdges;
using mc::render::OutlineSegment;
using mc::render::outlineEdgesOf;

[[nodiscard]] std::string readShader(const char* name) {
    const std::filesystem::path path = std::filesystem::path{MC_REBEDROCK_SHADER_SRC_DIR} / name;
    std::ifstream stream{path};
    if (!stream) {
        throw std::runtime_error("cannot open " + path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// The source with `//` comments removed. Everything below reads literals out of
// it, and a comment that mentions a number ("0.01 blocks proud") is not a
// constant the shader applies.
[[nodiscard]] std::string stripComments(const std::string& source) {
    std::string out;
    out.reserve(source.size());
    for (std::size_t i = 0; i < source.size();) {
        if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            while (i < source.size() && source[i] != '\n') {
                ++i;
            }
            continue;
        }
        out.push_back(source[i]);
        ++i;
    }
    return out;
}

// The body of `void main()`, from its head to end of file. Every numeric literal
// the vertex position passes through lives here; the constants above it do not.
[[nodiscard]] std::string mainBody(const std::string& source) {
    const auto begin = source.find("void main()");
    assert(begin != std::string::npos && "block_outline.vert lost its main()");
    return source.substr(begin);
}

// The value of a `const float <name> = <literal>;` declaration.
[[nodiscard]] float constantValue(const std::string& source, const std::string& name) {
    const auto begin = source.find("const float " + name + " = ");
    assert(begin != std::string::npos && "block_outline.vert lost its depth nudge constant");
    const auto from = begin + std::string{"const float "}.size() + name.size() + 3U;
    const auto end = source.find(';', from);
    assert(end != std::string::npos);
    return std::stof(source.substr(from, end - from));
}

// Every floating-point literal in `text`, as written. Integers that are plainly
// array indices or comparisons (no decimal point) are ignored: the question is
// what the position is multiplied or offset by.
[[nodiscard]] std::vector<std::string> floatLiterals(const std::string& text) {
    std::vector<std::string> found;
    for (std::size_t i = 0; i < text.size();) {
        if (std::isdigit(static_cast<unsigned char>(text[i])) == 0) {
            ++i;
            continue;
        }
        // Skip an identifier that merely contains digits (vec4, xyz1...).
        if (i > 0 && (std::isalnum(static_cast<unsigned char>(text[i - 1])) != 0 ||
                      text[i - 1] == '_')) {
            while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) != 0 ||
                                       text[i] == '_')) {
                ++i;
            }
            continue;
        }
        const std::size_t start = i;
        while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) != 0 ||
                                   text[i] == '.')) {
            ++i;
        }
        const std::string literal = text.substr(start, i - start);
        if (literal.find('.') != std::string::npos) {
            found.push_back(literal);
        }
        ++i;
    }
    return found;
}

// --- The independent oracle. -------------------------------------------------
//
// `outlineEdgesOf` decides "is this grid line an edge" from the occupancy of the
// four cells around it. This decides the same question from the SOLID itself:
// probe four points a hair off the line, one in each quadrant of the plane
// perpendicular to it, and ask whether each is inside the union of boxes. A
// point on a line is on an edge of the union exactly when the solid occupies one
// quadrant, three quadrants, or two diagonally opposite ones — two ADJACENT
// quadrants is a flat surface passing through, and a flat surface has no edge.
//
// Two adjacent quadrants is precisely the interior seam: at the back of a stair,
// y=0.5, both the box below and the box above are solid on the same side of the
// face, so the surface runs straight through. That is the case per-box outlining
// drew and vanilla does not.

[[nodiscard]] bool insideUnion(const std::vector<OutlineBox>& boxes, const glm::vec3& point) {
    for (const OutlineBox& box : boxes) {
        if (point.x > box.minimum.x && point.x < box.maximum.x && point.y > box.minimum.y &&
            point.y < box.maximum.y && point.z > box.minimum.z && point.z < box.maximum.z) {
            return true;
        }
    }
    return false;
}

// `along` is the axis the candidate line runs along; `point` is a point on it.
[[nodiscard]] bool isEdgeOfUnion(const std::vector<OutlineBox>& boxes, const glm::vec3& point,
                                 int along) {
    // Smaller than any gap in the roster (everything is n/16) and larger than
    // float noise on values built out of those sixteenths.
    constexpr float kProbe = 1.0F / 256.0F;
    const int u = (along + 1) % 3;
    const int v = (along + 2) % 3;
    int occupied = 0;
    int oddParity = 0;
    for (int du = 0; du <= 1; ++du) {
        for (int dv = 0; dv <= 1; ++dv) {
            glm::vec3 probe = point;
            probe[u] += du == 0 ? -kProbe : kProbe;
            probe[v] += dv == 0 ? -kProbe : kProbe;
            if (insideUnion(boxes, probe)) {
                ++occupied;
                oddParity ^= du ^ dv;
            }
        }
    }
    return occupied == 1 || occupied == 3 || (occupied == 2 && (oddParity & 1) == 0);
}

// The distinct coordinate planes on one axis, the test's own copy — deliberately
// not the header's, so the two can disagree.
[[nodiscard]] std::vector<float> planesOf(const std::vector<OutlineBox>& boxes, int axis) {
    std::vector<float> values;
    for (const OutlineBox& box : boxes) {
        values.push_back(box.minimum[axis]);
        values.push_back(box.maximum[axis]);
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

// Does `segments` cover exactly the grid segments the oracle calls edges?
//
// Covering rather than matching one-to-one, because merging is allowed (and
// required): three collinear grid segments may arrive as one line. What must
// hold is that every oracle edge is inside some emitted segment, and every
// emitted segment is made only of oracle edges.
void assertEdgeSetMatchesOracle(const std::vector<OutlineBox>& boxes, const OutlineEdges& edges,
                                const char* label) {
    const std::array<std::vector<float>, 3> planes{planesOf(boxes, 0), planesOf(boxes, 1),
                                                   planesOf(boxes, 2)};

    // (a) Nothing emitted that the solid does not have an edge along.
    for (std::size_t s = 0; s < edges.count; ++s) {
        const OutlineSegment& segment = edges.segments[s];
        int along = -1;
        int differing = 0;
        for (int axis = 0; axis < 3; ++axis) {
            if (segment.start[axis] != segment.end[axis]) {
                along = axis;
                ++differing;
            }
        }
        if (differing != 1) {
            std::cerr << label << ": a segment runs along " << differing << " axes\n";
        }
        assert(differing == 1 && "an outline segment runs along exactly one axis");
        assert(segment.start[along] < segment.end[along] && "a segment runs low to high");
        // Every grid cell the segment spans must itself be an edge.
        const std::vector<float>& line = planes[static_cast<std::size_t>(along)];
        for (std::size_t k = 0; k + 1 < line.size(); ++k) {
            if (line[k] < segment.start[along] || line[k + 1] > segment.end[along]) {
                continue;
            }
            glm::vec3 midpoint = segment.start;
            midpoint[along] = (line[k] + line[k + 1]) * 0.5F;
            if (!isEdgeOfUnion(boxes, midpoint, along)) {
                std::cerr << label << ": drew a line at (" << midpoint.x << ", " << midpoint.y
                          << ", " << midpoint.z << ") along axis " << along
                          << " where the surface is flat — that is an interior seam\n";
            }
            assert(isEdgeOfUnion(boxes, midpoint, along));
        }
    }

    // (b) Nothing the solid DOES have an edge along is missing. Walks every grid
    // line of the merged arrangement, which is where every edge of a box union
    // has to lie.
    std::size_t oracleCells = 0;
    for (int along = 0; along < 3; ++along) {
        const auto u = static_cast<std::size_t>((along + 1) % 3);
        const auto v = static_cast<std::size_t>((along + 2) % 3);
        for (const float uu : planes[u]) {
            for (const float vv : planes[v]) {
                const std::vector<float>& line = planes[static_cast<std::size_t>(along)];
                for (std::size_t k = 0; k + 1 < line.size(); ++k) {
                    glm::vec3 midpoint{};
                    midpoint[static_cast<int>(u)] = uu;
                    midpoint[static_cast<int>(v)] = vv;
                    midpoint[along] = (line[k] + line[k + 1]) * 0.5F;
                    if (!isEdgeOfUnion(boxes, midpoint, along)) {
                        continue;
                    }
                    ++oracleCells;
                    bool covered = false;
                    for (std::size_t s = 0; s < edges.count && !covered; ++s) {
                        const OutlineSegment& segment = edges.segments[s];
                        covered = segment.start[static_cast<int>(u)] == uu &&
                                  segment.end[static_cast<int>(u)] == uu &&
                                  segment.start[static_cast<int>(v)] == vv &&
                                  segment.end[static_cast<int>(v)] == vv &&
                                  segment.start[along] <= line[k] &&
                                  segment.end[along] >= line[k + 1];
                    }
                    if (!covered) {
                        std::cerr << label << ": no line drawn at (" << midpoint.x << ", "
                                  << midpoint.y << ", " << midpoint.z << ") along axis " << along
                                  << ", where the solid has a real edge\n";
                    }
                    assert(covered);
                }
            }
        }
    }
    assert(oracleCells > 0 && "the oracle found no edges at all — the shape was empty");
}

// Does the set contain this exact line? Used for the four named claims about a
// stair, where "an equivalent pair of half-lines" would not be the same picture.
[[nodiscard]] bool hasSegment(const OutlineEdges& edges, const glm::vec3& start,
                              const glm::vec3& end) {
    for (std::size_t i = 0; i < edges.count; ++i) {
        if (edges.segments[i] == OutlineSegment{start, end}) {
            return true;
        }
    }
    return false;
}

// Does any emitted segment cover this whole span? (A merged run may be longer.)
[[nodiscard]] bool coversSpan(const OutlineEdges& edges, const glm::vec3& start,
                              const glm::vec3& end, int along) {
    for (std::size_t i = 0; i < edges.count; ++i) {
        const OutlineSegment& s = edges.segments[i];
        bool sameLine = true;
        for (int axis = 0; axis < 3; ++axis) {
            if (axis == along) continue;
            sameLine = sameLine && s.start[axis] == start[axis] && s.end[axis] == start[axis];
        }
        if (sameLine && s.start[along] <= start[along] && s.end[along] >= end[along]) {
            return true;
        }
    }
    return false;
}

// The renderer's own adapter, mirrored: `world::blockSelectionBoxes` hands over a
// fixed-capacity box set and the renderer turns it into OutlineBoxes.
[[nodiscard]] std::vector<OutlineBox> boxesOf(mc::world::BlockState state) {
    const mc::world::BlockShape shape = mc::world::blockShape(state);
    std::vector<OutlineBox> boxes;
    switch (shape.kind) {
    case mc::world::ShapeKind::Empty:
        break;
    case mc::world::ShapeKind::Column:
        boxes.push_back({{0.0F, shape.bottom, 0.0F}, {1.0F, shape.top, 1.0F}});
        break;
    case mc::world::ShapeKind::Boxes:
        for (const mc::world::ShapeBox& box : shape.boxes) {
            boxes.push_back({{box.minX, box.minY, box.minZ}, {box.maxX, box.maxY, box.maxZ}});
        }
        break;
    }
    return boxes;
}

[[nodiscard]] OutlineEdges edgesOf(const std::vector<OutlineBox>& boxes) {
    return outlineEdgesOf(std::span<const OutlineBox>{boxes});
}

} // namespace

int main() {
    using mc::render::kOutlineSegmentVertexCount;
    using mc::render::kOutlineViewShrink;

    // --- A single box is still its twelve edges, merged. -----------------------
    //
    // The grid for one box is 1x1x1, so this also says the merge did not eat
    // anything: twelve lines, each a full edge of the box, each running along one
    // axis, no duplicates.
    {
        const std::array<std::pair<glm::vec3, glm::vec3>, 4> shapes{{
            {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}},                    // a full cube
            {{0.0F, 0.0F, 0.0F}, {1.0F, 2.0F / 16.0F, 1.0F}},            // a diode base
            {{0.0F, 13.0F / 16.0F, 0.0F}, {1.0F, 1.0F, 1.0F}},           // a trapdoor leaf
            {{1.0F / 16.0F, 0.0F, 1.0F / 16.0F},
             {15.0F / 16.0F, 1.0F / 16.0F, 15.0F / 16.0F}},              // a pressure plate
        }};
        for (const auto& [minimum, maximum] : shapes) {
            const std::vector<OutlineBox> boxes{{minimum, maximum}};
            const OutlineEdges edges = edgesOf(boxes);
            assert(edges.count == 12U && "one box is twelve lines");
            assert(!edges.truncated);
            std::set<std::pair<std::string, std::string>> seen;
            for (std::size_t i = 0; i < edges.count; ++i) {
                const OutlineSegment& s = edges.segments[i];
                // Every endpoint component is exactly one end of the box's span
                // on that axis — never a hair outside it. This is RN-13-2's
                // claim, restated for the segment form.
                for (int axis = 0; axis < 3; ++axis) {
                    assert(s.start[axis] == minimum[axis] || s.start[axis] == maximum[axis]);
                    assert(s.end[axis] == minimum[axis] || s.end[axis] == maximum[axis]);
                }
                std::ostringstream a;
                std::ostringstream b;
                a << s.start.x << ',' << s.start.y << ',' << s.start.z;
                b << s.end.x << ',' << s.end.y << ',' << s.end.z;
                assert(seen.insert({a.str(), b.str()}).second && "an edge is drawn twice");
            }
            assertEdgeSetMatchesOracle(boxes, edges, "single box");
        }
        // A degenerate box has no volume and therefore no edges — and no NaN.
        const std::vector<OutlineBox> flat{{{0.5F, 0.5F, 0.5F}, {0.5F, 0.5F, 0.5F}}};
        assert(edgesOf(flat).count == 0U);
        assert(outlineEdgesOf(std::span<const OutlineBox>{}).count == 0U);
    }

    // --- The stair, stated as the four claims the field report made. -----------
    //
    // Idealised to two boxes so the coordinates can be written down: the lower
    // slab fills y 0..0.5 across the whole cell, the upper step sits on the back
    // half (z 0.5..1). That is a north-facing stair's shape, and the four lines
    // at y=0.5 are exactly what per-box outlining got wrong.
    {
        const std::vector<OutlineBox> stair{
            {{0.0F, 0.0F, 0.0F}, {1.0F, 0.5F, 1.0F}},
            {{0.0F, 0.5F, 0.5F}, {1.0F, 1.0F, 1.0F}},
        };
        const OutlineEdges edges = edgesOf(stair);
        assert(!edges.truncated);

        // (1) THE FRONT LINE IS THERE. At z=0, y=0.5, running the full width:
        // below it the lower slab's face, above it nothing at all. A real
        // silhouette boundary, and vanilla draws it.
        assert(hasSegment(edges, {0.0F, 0.5F, 0.0F}, {1.0F, 0.5F, 0.0F}));

        // (2) THE BACK SEAM IS GONE. At z=1, y=0.5, both boxes reach the same
        // face and the surface runs straight through. Per-box outlining drew a
        // bar across the back of every stair in the world.
        assert(!coversSpan(edges, {0.0F, 0.5F, 1.0F}, {1.0F, 0.5F, 1.0F}, 0));

        // (3) THE INNER CORNER IS THERE. At y=0.5, z=0.5 — the step's tread
        // meeting its riser. Three of the four cells around it are solid.
        assert(hasSegment(edges, {0.0F, 0.5F, 0.5F}, {1.0F, 0.5F, 0.5F}));

        // (4) THE SIDES KEEP ONLY THE TREAD'S EDGE. On each side face there IS a
        // line at y=0.5 — the top of the step you walk on — but it stops at
        // z=0.5. Per-box outlining ran it the full depth of the block, straight
        // across the flush seam behind it. Both halves are asserted: the tread
        // edge present, the full-depth bar absent.
        for (const float x : {0.0F, 1.0F}) {
            assert(hasSegment(edges, {x, 0.5F, 0.0F}, {x, 0.5F, 0.5F}));
            assert(!coversSpan(edges, {x, 0.5F, 0.0F}, {x, 0.5F, 1.0F}, 2));
        }

        // The whole set, against the independent oracle.
        assertEdgeSetMatchesOracle(stair, edges, "idealised stair");

        // And the count: an L-prism is a hexagon extruded, so six edges at each
        // end plus six struts. Eighteen, not the twenty-four two boxes would
        // give. Stated as a number because "fewer than before" is not a
        // specification.
        assert(edges.count == 18U);
    }

    // --- A shape whose merged outline has an edge NO box has. ------------------
    //
    // Two boxes overlapping in plan view: the reflex line where one box's side
    // face meets the other's front face is an edge of the union and an edge of
    // neither box. A "draw each box's twelve edges, minus the shared ones"
    // shortcut cannot produce it; walking the merged grid does. It is why this
    // is `Shapes.or` and not a subtraction.
    {
        const std::vector<OutlineBox> overlapping{
            {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.5F}},
            {{0.5F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}},
        };
        const OutlineEdges edges = edgesOf(overlapping);
        assert(!edges.truncated);
        // x=0.5, z=0.5, running the full height: the reflex corner.
        assert(hasSegment(edges, {0.5F, 0.0F, 0.5F}, {0.5F, 1.0F, 0.5F}));
        assertEdgeSetMatchesOracle(overlapping, edges, "overlapping boxes");
    }

    // --- Every state of every block in the roster. -----------------------------
    //
    // The named cases above say what a stair looks like; this says nothing in the
    // roster produces a line the solid does not have, or misses one it does —
    // including the walls, whose five boxes make the widest grid, and the fence
    // gates and pressure plates the report also listed.
    {
        std::size_t widest = 0;
        std::size_t withEdges = 0;
        std::size_t multiBox = 0;
        for (std::uint32_t id = 0; id < mc::world::kBlockStateCount; ++id) {
            const auto state = mc::world::BlockState::fromRawId(id);
            const std::vector<OutlineBox> boxes = boxesOf(state);
            if (boxes.empty()) {
                continue;
            }
            const OutlineEdges edges = edgesOf(boxes);
            assert(!edges.truncated &&
                   "a shape outgrew kMaxOutlineSegments — raise the cap deliberately");
            if (edges.count == 0U) {
                continue; // a zero-volume shape (a fully open fence gate)
            }
            ++withEdges;
            if (boxes.size() > 1U) {
                ++multiBox;
            }
            widest = std::max(widest, edges.count);
            assertEdgeSetMatchesOracle(boxes, edges, "roster state");
        }
        // Guards on the guard: an emptied roster would let the sweep pass by
        // doing nothing, and a shape table that had collapsed to single boxes
        // would never exercise the merge at all.
        assert(withEdges > 1000U);
        assert(multiBox > 100U);
        assert(widest > 12U && "the sweep never saw a shape more complex than one box");
        assert(widest < mc::render::kMaxOutlineSegments);
        std::cout << "outline states checked: " << withEdges << " (" << multiBox
                  << " multi-box), widest " << widest << " segments of "
                  << mc::render::kMaxOutlineSegments << "\n";
    }

    // --- Lockstep with the shader that actually places these endpoints. --------
    {
        const std::string source = stripComments(readShader("block_outline.vert"));
        const std::string body = mainBody(source);

        // One draw is one line: the shader picks an endpoint off gl_VertexIndex,
        // so the vertex count and the two-field push block are the same fact.
        assert(kOutlineSegmentVertexCount == 2U);
        assert(body.find("gl_VertexIndex == 0 ? outline.segmentStart.xyz : outline.segmentEnd.xyz") !=
               std::string::npos);
        // The box form is gone in both directions: no corner tables to index and
        // no min/max to interpolate between.
        assert(source.find("boundsMin") == std::string::npos &&
               source.find("boundsMax") == std::string::npos &&
               "block_outline.vert still reads a box; the outline is a line list now");
        assert(source.find("edgeVertices") == std::string::npos);
        // And nothing expands anything about a box centre any more.
        assert(body.find("boxCenter") == std::string::npos &&
               "the outline must not be scaled about its own centre — that is the RN-13-2 bug");

        // main() may contain NO numeric constant except the homogeneous 1.0. Any
        // other literal there is a fudge factor applied to the position, which is
        // exactly what the 1.02 was; the one legitimate constant is named, lives
        // outside main() and is checked against the header below.
        for (const std::string& literal : floatLiterals(body)) {
            assert(literal == "1.0" &&
                   "block_outline.vert's main() grew a magic constant — the position must be the "
                   "segment endpoint, and the only nudge is the named view-space one");
        }
        assert(std::fabs(constantValue(source, "kViewShrink") - kOutlineViewShrink) < 1.0e-9F &&
               "the depth nudge must be JE's 1 - 1/4096, the same value the header states");

        // RN-39：偏移的**量**也是被钉住的，不只是「两处一致」。
        //
        // vanilla 的 1/4096 在这条管线上不够：线的光栅化像素中心与覆盖同一条棱的
        // 三角形采样点差半个像素，而掠射面上半个像素的深度步远大于距离的 1/4096，
        // 于是线被它自己所在的那个面吃掉。实测（导出 `--outline`，石头方块）：
        // 1/4096 只画出 1194 个线像素中的 824 个，正对相机的棱是实的、掠射的成了虚线——
        // 那就是玩家瞄准方块时看到的闪烁。1/1024 处饱和（1187）。
        //
        // 下限写 1/2048 而不是 1/1024：留一档余量给将来调，但把「改回 vanilla 那个
        // 数字」挡在门外。
        assert(kOutlineViewShrink <= 1.0F - 1.0F / 2048.0F &&
               "the outline's depth nudge must be at least 1/2048 of the camera distance; "
               "vanilla's 1/4096 leaves the grazing edges dashed on this pipeline");
        assert(kOutlineViewShrink > 1.0F - 1.0F / 128.0F &&
               "and no more than 1/128, or the line starts floating off the surface");
        // It has to be applied in VIEW space: a world-space offset would push the
        // line into a neighbouring block and lose the shared edge behind its face.
        assert(body.find("camera.view * vec4(worldPosition, 1.0)") != std::string::npos);
        assert(body.find("viewPosition.xyz *=") != std::string::npos);
    }

    std::cout << "block outline geometry: ok\n";
    return 0;
}

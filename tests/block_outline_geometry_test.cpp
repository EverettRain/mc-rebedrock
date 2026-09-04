// RN-13-2: the selection-box wireframe hugs the box exactly.
//
// The defect was a size-proportional expansion in block_outline.vert —
// `local = boxCenter + (local - boxCenter) * 1.02` — dating from the initial
// commit. A full cube's edges floated 0.01 blocks outside the block, a 2px
// diode base 0.00125, so the offset was not even consistent between the boxes
// RN-10f now draws side by side.
//
// Headless cannot look at the wireframe, but it can pin the two claims the fix
// rests on:
//   * a vertex IS a corner of the box (BlockOutlineGeometry.hpp), for boxes of
//     wildly different sizes, so nothing about the offset can depend on size;
//   * the shader computes that and nothing else — its table matches the header
//     and its main() carries no constant except the homogeneous 1.0 and the
//     documented camera-space nudge. That second half is where the defect
//     actually lived, so it is asserted against the shader source directly, the
//     way item_cube_uv_test asserts the item shaders' UV literals.

#include "render/BlockOutlineGeometry.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
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
// the vertex position passes through lives here; the tables above it do not.
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
// array indices (no decimal point) are ignored: the question is what the
// position is multiplied or offset by.
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

// The `int[](...)` table the shader declares for `edgeVertices`.
[[nodiscard]] std::vector<int> parseEdgeTable(const std::string& source) {
    const auto begin = source.find("edgeVertices");
    assert(begin != std::string::npos);
    const auto open = source.find("int[](", begin);
    assert(open != std::string::npos);
    const auto close = source.find(')', open + 6);
    assert(close != std::string::npos);
    std::vector<int> values;
    std::istringstream stream{source.substr(open + 6, close - open - 6)};
    std::string token;
    while (std::getline(stream, token, ',')) {
        values.push_back(std::stoi(token));
    }
    return values;
}

// The `vec3(a, b, c)` entries of the `corners` table.
[[nodiscard]] std::vector<glm::vec3> parseCornerTable(const std::string& source) {
    const auto begin = source.find("const vec3 corners[8]");
    assert(begin != std::string::npos);
    const auto end = source.find(");", begin);
    assert(end != std::string::npos);
    const std::string block = source.substr(begin, end - begin);
    std::vector<glm::vec3> values;
    for (std::size_t cursor = 0;;) {
        const auto call = block.find("vec3(", cursor);
        if (call == std::string::npos) {
            break;
        }
        const auto close = block.find(')', call);
        assert(close != std::string::npos);
        std::istringstream stream{block.substr(call + 5, close - call - 5)};
        std::array<float, 3> component{};
        std::string token;
        for (float& value : component) {
            std::getline(stream, token, ',');
            value = std::stof(token);
        }
        values.push_back({component[0], component[1], component[2]});
        cursor = close + 1;
    }
    return values;
}

[[nodiscard]] bool same(const glm::vec3& a, const glm::vec3& b) {
    return std::fabs(a.x - b.x) < 1.0e-6F && std::fabs(a.y - b.y) < 1.0e-6F &&
           std::fabs(a.z - b.z) < 1.0e-6F;
}

} // namespace

int main() {
    using mc::render::kOutlineCorners;
    using mc::render::kOutlineEdgeVertices;
    using mc::render::kOutlineVertexCount;
    using mc::render::kOutlineViewShrink;
    using mc::render::outlineVertexLocal;

    // --- A vertex is a corner of the box, for every box shape the roster puts on
    //     screen. The full cube and the 2px diode base are the two ends of the
    //     spread the 1.02 expansion behaved differently on; the trapdoor leaf and
    //     the pressure plate are thin in one axis, where a proportional offset
    //     nearly vanishes and an absolute one would not. ---
    {
        struct Box final {
            glm::vec3 minimum;
            glm::vec3 maximum;
        };
        const std::array<Box, 5> boxes{{
            {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}},                  // a full cube
            {{0.0F, 0.0F, 0.0F}, {1.0F, 2.0F / 16.0F, 1.0F}},          // a diode base
            {{0.0F, 13.0F / 16.0F, 0.0F}, {1.0F, 1.0F, 1.0F}},         // a trapdoor leaf
            {{1.0F / 16.0F, 0.0F, 1.0F / 16.0F},
             {15.0F / 16.0F, 1.0F / 16.0F, 15.0F / 16.0F}},            // a pressure plate
            {{0.5F, 0.5F, 0.5F}, {0.5F, 0.5F, 0.5F}},                  // degenerate: no NaN
        }};
        for (const Box& box : boxes) {
            for (std::size_t index = 0; index < kOutlineVertexCount; ++index) {
                const glm::vec3 vertex = outlineVertexLocal(box.minimum, box.maximum, index);
                // Each component is exactly one end of the box's span on that
                // axis — never a hair outside it.
                assert(vertex.x == box.minimum.x || vertex.x == box.maximum.x);
                assert(vertex.y == box.minimum.y || vertex.y == box.maximum.y);
                assert(vertex.z == box.minimum.z || vertex.z == box.maximum.z);
            }
        }
    }

    // --- The 24 indices are the twelve edges of a cube, each once: every listed
    //     pair differs in exactly one axis, and no pair repeats. A scrambled
    //     table would still draw 24 endpoints and still hug the box. ---
    {
        std::set<std::pair<int, int>> edges;
        for (std::size_t i = 0; i < kOutlineVertexCount; i += 2) {
            const glm::vec3& a = kOutlineCorners[kOutlineEdgeVertices[i]];
            const glm::vec3& b = kOutlineCorners[kOutlineEdgeVertices[i + 1]];
            const int differing = static_cast<int>(a.x != b.x) + static_cast<int>(a.y != b.y) +
                                  static_cast<int>(a.z != b.z);
            assert(differing == 1 && "an outline edge must run along one axis");
            const int lo = std::min(kOutlineEdgeVertices[i], kOutlineEdgeVertices[i + 1]);
            const int hi = std::max(kOutlineEdgeVertices[i], kOutlineEdgeVertices[i + 1]);
            assert(edges.insert({lo, hi}).second && "an edge is drawn twice");
        }
        assert(edges.size() == 12U);
    }

    // --- Lockstep with the shader that actually generates these vertices. ---
    {
        const std::string source = stripComments(readShader("block_outline.vert"));

        const auto corners = parseCornerTable(source);
        assert(corners.size() == kOutlineCorners.size());
        for (std::size_t i = 0; i < corners.size(); ++i) {
            assert(same(corners[i], kOutlineCorners[i]));
        }

        const auto edges = parseEdgeTable(source);
        assert(edges.size() == kOutlineVertexCount);
        for (std::size_t i = 0; i < edges.size(); ++i) {
            assert(edges[i] == static_cast<int>(kOutlineEdgeVertices[i]));
        }

        const std::string body = mainBody(source);
        // The endpoint is the box corner, spelled as the mix the header states.
        assert(body.find("mix(outline.boundsMin.xyz, outline.boundsMax.xyz, unit)") !=
               std::string::npos);
        // And nothing expands it about the box centre any more.
        assert(body.find("boxCenter") == std::string::npos &&
               "the outline must not be scaled about its own centre — that is the RN-13-2 bug");

        // main() may contain NO numeric constant except the homogeneous 1.0. Any
        // other literal there is a fudge factor applied to the position, which is
        // exactly what the 1.02 was; the one legitimate constant is named, lives
        // outside main() and is checked against the header below.
        for (const std::string& literal : floatLiterals(body)) {
            assert(literal == "1.0" &&
                   "block_outline.vert's main() grew a magic constant — the position must be the "
                   "box corner, and the only nudge is the named view-space one");
        }
        assert(std::fabs(constantValue(source, "kViewShrink") - kOutlineViewShrink) < 1.0e-9F &&
               "the depth nudge must be JE's 1 - 1/4096, the same value the header states");
        // It has to be applied in VIEW space: a world-space offset would push the
        // line into a neighbouring block and lose the shared edge behind its face.
        assert(body.find("camera.view * vec4(worldPosition, 1.0)") != std::string::npos);
        assert(body.find("viewPosition.xyz *=") != std::string::npos);
    }

    return 0;
}

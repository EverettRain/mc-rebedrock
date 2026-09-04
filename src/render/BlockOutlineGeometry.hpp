#pragma once

// RN-13-2: the selection-box wireframe's geometry contract.
//
// The outline has no vertex buffer — `block_outline.vert` generates all 24
// endpoints from `gl_VertexIndex` and the box in a push constant — so there was
// nowhere on the C++ side that stated what those endpoints are, and the shader
// quietly disagreed with vanilla from the initial commit onwards:
//
//     local = boxCenter + (local - boxCenter) * 1.02;
//
// a 2% expansion **about the box centre**, which means the offset grows with the
// box: a full cube's edges sat 0.01 blocks proud, a 2px diode's base 0.00125.
// Vanilla's ShapeRenderer.renderShape emits `forAllEdges` coordinates untouched
// and separates the line from the surface in the DEPTH domain instead —
// RenderTypes.LINES carries LayeringTransform.VIEW_OFFSET_Z_LAYERING, which for a
// perspective projection is `modelViewStack.scale(1 - 1/4096)`
// (ProjectionType.java:7), i.e. the vertex is pulled a four-thousandth of its
// camera distance toward the eye. The inflation was invisible while every block
// was outlined by one full-cell bounding box; RN-10f's per-box outlining put
// small boxes and large ones on screen at once and made the size dependence
// obvious.
//
// So: the endpoint IS the box corner, and the depth nudge is camera-relative
// (never geometry-relative, which is what would sink a shared edge into the
// neighbouring block's face). This header states both, `block_outline.vert`
// implements them, and `block_outline_geometry_test` holds the two together —
// the same lockstep item_cube_uv_test keeps between CubeUv.hpp and the item
// shaders.

#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace mc::render {

// The unit cube's eight corners, in the order the shader's `corners[8]` table
// declares them.
inline constexpr std::array<glm::vec3, 8> kOutlineCorners{{
    {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.0F}, {0.0F, 1.0F, 0.0F},
    {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 1.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 1.0F, 1.0F},
}};

// The twelve edges as 24 corner indices (a VK_PRIMITIVE_TOPOLOGY_LINE_LIST):
// the z=0 ring, the z=1 ring, then the four struts between them.
inline constexpr std::array<std::uint8_t, 24> kOutlineEdgeVertices{{
    0, 1, 1, 2, 2, 3, 3, 0, //
    4, 5, 5, 6, 6, 7, 7, 4, //
    0, 4, 1, 5, 2, 6, 3, 7, //
}};

inline constexpr std::size_t kOutlineVertexCount = kOutlineEdgeVertices.size();

// JE ProjectionType.PERSPECTIVE's layering transform at bias 1: scale the
// camera-relative position by 1 - 1/4096. Applied in VIEW space, so the offset
// is a fixed fraction of the distance to the eye and does not scale with the
// box — which is the whole difference from the 1.02 it replaces.
inline constexpr float kOutlineViewShrink = 1.0F - 1.0F / 4096.0F;

// The block-local position of outline vertex `index` for a box spanning
// [minimum, maximum]. Exactly a corner of that box: no inflation, no epsilon.
[[nodiscard]] constexpr glm::vec3 outlineVertexLocal(const glm::vec3& minimum,
                                                     const glm::vec3& maximum,
                                                     std::size_t index) {
    const glm::vec3& unit = kOutlineCorners[kOutlineEdgeVertices[index]];
    return {minimum.x + (maximum.x - minimum.x) * unit.x,
            minimum.y + (maximum.y - minimum.y) * unit.y,
            minimum.z + (maximum.z - minimum.z) * unit.z};
}

} // namespace mc::render

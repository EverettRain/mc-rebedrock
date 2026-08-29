#include "world/FaceBakery.hpp"

#include <array>
#include <cassert>
#include <cmath>

// RN-4 N1: the geometry/UV baking primitive (world/FaceBakery.hpp) checked
// against hand-computed JE FaceBakery / CuboidFace / FaceInfo numbers. Nothing is
// wired into the mesher yet; this locks the primitive's values so N2-N4 can port
// each path onto it and diff against the current render output with confidence.

namespace {

using namespace mc::world::bake;

[[nodiscard]] bool near(float a, float b) { return std::fabs(a - b) < 1.0e-4F; }
[[nodiscard]] bool near2(const glm::vec2& a, float x, float y) {
    return near(a.x, x) && near(a.y, y);
}
[[nodiscard]] bool near3(const glm::vec3& a, float x, float y, float z) {
    return near(a.x, x) && near(a.y, y) && near(a.z, z);
}

// The unit-box (from 0, to 1) corners of each face, transcribed straight from JE
// FaceInfo.java. This is the memory-mandated "vertex layout == JE" guard: if the
// primitive's kFaceInfo ever drifts from vanilla, winding and UV corners silently
// rotate, and this fires.
void testFaceInfoMatchesJava() {
    const glm::vec3 zero{0.0F};
    const glm::vec3 one{1.0F};
    const std::array<std::array<glm::vec3, 4>, 6> expected{{
        // Down
        {{{0, 0, 1}, {0, 0, 0}, {1, 0, 0}, {1, 0, 1}}},
        // Up
        {{{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}},
        // North
        {{{1, 1, 0}, {1, 0, 0}, {0, 0, 0}, {0, 1, 0}}},
        // South
        {{{0, 1, 1}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}}},
        // West
        {{{0, 1, 0}, {0, 0, 0}, {0, 0, 1}, {0, 1, 1}}},
        // East
        {{{1, 1, 1}, {1, 0, 1}, {1, 0, 0}, {1, 1, 0}}},
    }};
    for (std::size_t f = 0; f < kFacingCount; ++f) {
        for (std::size_t v = 0; v < 4; ++v) {
            const glm::vec3 got = faceVertex(kFaceInfo[f][v], zero, one);
            const glm::vec3& want = expected[f][v];
            assert(near3(got, want.x, want.y, want.z));
        }
    }
}

// JE FaceBakery.defaultFaceUV on a non-cube box from(2,3,4) to(10,12,14), the
// projection of each face onto its plane. Values hand-computed from the switch.
void testDefaultFaceUv() {
    const glm::vec3 from{2, 3, 4};
    const glm::vec3 to{10, 12, 14};
    const auto d = defaultFaceUv(from, to, Facing::Down);
    assert(!d.absent && near(d.minU, 2) && near(d.minV, 2) && near(d.maxU, 10) && near(d.maxV, 12));
    const auto u = defaultFaceUv(from, to, Facing::Up);
    assert(near(u.minU, 2) && near(u.minV, 4) && near(u.maxU, 10) && near(u.maxV, 14));
    const auto n = defaultFaceUv(from, to, Facing::North);
    assert(near(n.minU, 6) && near(n.minV, 4) && near(n.maxU, 14) && near(n.maxV, 13));
    const auto s = defaultFaceUv(from, to, Facing::South);
    assert(near(s.minU, 2) && near(s.minV, 4) && near(s.maxU, 10) && near(s.maxV, 13));
    const auto w = defaultFaceUv(from, to, Facing::West);
    assert(near(w.minU, 4) && near(w.minV, 4) && near(w.maxU, 14) && near(w.maxV, 13));
    const auto e = defaultFaceUv(from, to, Facing::East);
    assert(near(e.minU, 2) && near(e.minV, 4) && near(e.maxU, 12) && near(e.maxV, 13));
}

// JE CuboidFace.getVertexU/V + Quadrant.rotateVertexIndex: a full 0..16 rect,
// sampled per output vertex for quadrants 0 and 1 (a 90° face rotation is a
// cyclic +1 shift of the sampled corner).
void testUvCornersAndQuadrant() {
    const FaceUv uv{0, 0, 16, 16, false};
    // quadrant 0: (min,min),(min,max),(max,max),(max,min).
    const std::array<glm::vec2, 4> q0{{{0, 0}, {0, 16}, {16, 16}, {16, 0}}};
    for (int i = 0; i < 4; ++i) {
        const int c = rotateVertexIndex(i, 0);
        assert(near(vertexU(uv, c), q0[static_cast<std::size_t>(i)].x));
        assert(near(vertexV(uv, c), q0[static_cast<std::size_t>(i)].y));
    }
    // quadrant 1: shifted one corner.
    const std::array<glm::vec2, 4> q1{{{0, 16}, {16, 16}, {16, 0}, {0, 0}}};
    for (int i = 0; i < 4; ++i) {
        const int c = rotateVertexIndex(i, 1);
        assert(near(vertexU(uv, c), q1[static_cast<std::size_t>(i)].x));
        assert(near(vertexV(uv, c), q1[static_cast<std::size_t>(i)].y));
    }
}

// A plain full-cube UP face: positions are the unit UP corners, default UV covers
// the whole layer, facing stays Up, winding leaves the canonical order untouched.
void testBakeFullCubeUp() {
    ElementFace face;
    face.present = true;
    const auto q = bakeElementFace({0, 0, 0}, {16, 16, 16}, Facing::Up, face, ElementRotation{},
                                   ModelTransform{});
    assert(q.facing == Facing::Up);
    assert(near3(q.normal, 0, 1, 0));
    assert(near3(q.position[0], 0, 1, 0) && near3(q.position[1], 0, 1, 1));
    assert(near3(q.position[2], 1, 1, 1) && near3(q.position[3], 1, 1, 0));
    assert(near2(q.uv[0], 0, 0) && near2(q.uv[1], 0, 1));
    assert(near2(q.uv[2], 1, 1) && near2(q.uv[3], 1, 0));
}

// Element rotation: rotate a full box's UP face 90° about X at the block centre.
// The +Y face turns to face +Z (South). Winding is skipped for element-rotated
// quads (JE), so vertex 0 stays the rotated UP-corner (0,1,0) -> (0,1,1).
void testElementRotation() {
    ElementFace face;
    face.present = true;
    ElementRotation rot;
    rot.present = true;
    rot.origin = {8, 8, 8};
    rot.axis = 'x';
    rot.angleDeg = 90.0F;
    const auto q = bakeElementFace({0, 0, 0}, {16, 16, 16}, Facing::Up, face, rot,
                                   ModelTransform{});
    assert(q.facing == Facing::South);
    assert(near3(q.position[0], 0, 1, 1));
}

// ModelState rigid rotation with an exact integer matrix: Ry(90) maps the North
// face onto the West plane (x==0), and winding (which runs here, no element
// rotation) re-sorts to West's canonical layout.
void testModelStateRotation() {
    ElementFace face;
    face.present = true;
    ModelTransform state;
    // x'=z, y'=y, z'=-x  (column-major mat3(col0,col1,col2)).
    state.rotation = glm::mat3(glm::vec3{0, 0, -1}, glm::vec3{0, 1, 0}, glm::vec3{1, 0, 0});
    const auto q = bakeElementFace({0, 0, 0}, {16, 16, 16}, Facing::North, face, ElementRotation{},
                                   state);
    assert(q.facing == Facing::West);
    for (const auto& p : q.position) {
        assert(near(p.x, 0.0F)); // the whole quad lies on the x==0 plane
    }
    // After winding, vertex order matches West's FaceInfo unit corners.
    assert(near3(q.position[0], 0, 1, 0) && near3(q.position[1], 0, 0, 0));
    assert(near3(q.position[2], 0, 0, 1) && near3(q.position[3], 0, 1, 1));
}

} // namespace

int main() {
    testFaceInfoMatchesJava();
    testDefaultFaceUv();
    testUvCornersAndQuadrant();
    testBakeFullCubeUp();
    testElementRotation();
    testModelStateRotation();
    return 0;
}

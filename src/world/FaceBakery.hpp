#pragma once

// RN-4 N1: the single "model-local element face -> baked quad" primitive, the
// C++ transcription of JE net.minecraft.client.resources.model.cuboid.FaceBakery
// (+ CuboidFace UV rules + FaceInfo vertex layout + Quadrant rotation). This is
// the geometry/UV binding layer rebedrock was missing: today the Cube path
// (kUvs + reverse-engineered cubeFaceUvTurns), the shaped-block path (inline
// position-derived UV) and the ElementModel path (its own literal UV-rect corner
// order) each re-derive UVs independently. This header is the one source they all
// funnel into from N2 onward.
//
// N1 scope: the pure geometry+UV kernel only. It deliberately does NOT resolve a
// texture slot to an atlas layer, sample light/AO/tint, or apply uvlock — those
// are wiring concerns (N2+/N6). It is header-only, glm-based (bake runs once at
// startup, not per frame, so runtime glm is fine — trig is not constexpr), and
// carries no rebedrock world dependencies, so it can be unit-tested in isolation
// against hand-computed JE numbers.
//
// It mirrors JE's FaceInfo vertex order and getVertexU/getVertexV exactly (not
// the mesher's own kFaces/kUvs order), so every value here matches FaceBakery to
// the bit and recalculateWinding stays self-consistent. Reconciling this order
// with the mesher's kFaces is the N2 wiring step, guarded by the "layout == JE"
// assertions in face_bakery_test.

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cmath>
#include <cstdint>

namespace mc::world::bake {

// Facing in JE net.minecraft.core.Direction order (Direction.values() =
// DOWN,UP,NORTH,SOUTH,WEST,EAST) — FaceInfo, defaultFaceUV and the winding table
// are all authored against this order, so the primitive is self-consistent with
// JE. The mesher's own Face{PositiveX..} enum is a separate, N2-bridged thing.
enum class Facing : std::uint8_t { Down, Up, North, South, West, East };
inline constexpr std::size_t kFacingCount = 6;

inline constexpr std::uint8_t kNoCull = 0xFFU;

// RN-8d: a model json face `"rotation"` divided by 90 — the JE Quadrant the
// bakery applies as a cyclic shift of the sampled UV corner. Named rather than
// spelled 1/2/3 at the transcription sites, so a model reads back against the
// json it came from.
inline constexpr std::uint8_t kQuadrant90 = 1;
inline constexpr std::uint8_t kQuadrant180 = 2;
inline constexpr std::uint8_t kQuadrant270 = 3;

// The outward unit vector of a facing (JE Direction.getUnitVec3f).
[[nodiscard]] constexpr glm::vec3 facingUnit(Facing facing) {
    switch (facing) {
    case Facing::Down: return {0.0F, -1.0F, 0.0F};
    case Facing::Up: return {0.0F, 1.0F, 0.0F};
    case Facing::North: return {0.0F, 0.0F, -1.0F};
    case Facing::South: return {0.0F, 0.0F, 1.0F};
    case Facing::West: return {-1.0F, 0.0F, 0.0F};
    case Facing::East: return {1.0F, 0.0F, 0.0F};
    }
    return {0.0F, 1.0F, 0.0F};
}

// --- FaceInfo: the fixed vertex layout per facing, transcribed byte-for-byte
// from JE FaceInfo.java. Each vertex picks, per axis, the box's min (from) or max
// (to) extent. This is what pins texture orientation to a face regardless of
// caller, and what recalculateWinding re-sorts toward.
enum class Extent : std::uint8_t { Min, Max };
struct VertexInfo final {
    Extent x;
    Extent y;
    Extent z;
};

inline constexpr std::array<std::array<VertexInfo, 4>, kFacingCount> kFaceInfo{{
    // Down
    {{{Extent::Min, Extent::Min, Extent::Max}, {Extent::Min, Extent::Min, Extent::Min},
      {Extent::Max, Extent::Min, Extent::Min}, {Extent::Max, Extent::Min, Extent::Max}}},
    // Up
    {{{Extent::Min, Extent::Max, Extent::Min}, {Extent::Min, Extent::Max, Extent::Max},
      {Extent::Max, Extent::Max, Extent::Max}, {Extent::Max, Extent::Max, Extent::Min}}},
    // North
    {{{Extent::Max, Extent::Max, Extent::Min}, {Extent::Max, Extent::Min, Extent::Min},
      {Extent::Min, Extent::Min, Extent::Min}, {Extent::Min, Extent::Max, Extent::Min}}},
    // South
    {{{Extent::Min, Extent::Max, Extent::Max}, {Extent::Min, Extent::Min, Extent::Max},
      {Extent::Max, Extent::Min, Extent::Max}, {Extent::Max, Extent::Max, Extent::Max}}},
    // West
    {{{Extent::Min, Extent::Max, Extent::Min}, {Extent::Min, Extent::Min, Extent::Min},
      {Extent::Min, Extent::Min, Extent::Max}, {Extent::Min, Extent::Max, Extent::Max}}},
    // East
    {{{Extent::Max, Extent::Max, Extent::Max}, {Extent::Max, Extent::Min, Extent::Max},
      {Extent::Max, Extent::Min, Extent::Min}, {Extent::Max, Extent::Max, Extent::Min}}},
}};

// The vertex position for a box [from,to] under a VertexInfo (JE
// VertexInfo.select): each axis takes from (Min) or to (Max).
[[nodiscard]] constexpr glm::vec3 faceVertex(const VertexInfo& info, const glm::vec3& from,
                                             const glm::vec3& to) {
    return {info.x == Extent::Max ? to.x : from.x, info.y == Extent::Max ? to.y : from.y,
            info.z == Extent::Max ? to.z : from.z};
}

// --- UV rect (JE CuboidFace.UVs), in 0..16 model units. `absent` means "derive
// from geometry via defaultFaceUV", mirroring a face json with no `uv`.
struct FaceUv final {
    float minU = 0.0F;
    float minV = 0.0F;
    float maxU = 16.0F;
    float maxV = 16.0F;
    bool absent = true;
};

// JE FaceBakery.defaultFaceUV: project the element's from/to onto the face plane.
// This is the model-local-space -> UV binding that was reimplemented ad hoc in
// each shaped-block mesher.
[[nodiscard]] constexpr FaceUv defaultFaceUv(const glm::vec3& from, const glm::vec3& to,
                                             Facing facing) {
    switch (facing) {
    case Facing::Down:
        return {from.x, 16.0F - to.z, to.x, 16.0F - from.z, false};
    case Facing::Up:
        return {from.x, from.z, to.x, to.z, false};
    case Facing::North:
        return {16.0F - to.x, 16.0F - to.y, 16.0F - from.x, 16.0F - from.y, false};
    case Facing::South:
        return {from.x, 16.0F - to.y, to.x, 16.0F - from.y, false};
    case Facing::West:
        return {from.z, 16.0F - to.y, to.z, 16.0F - from.y, false};
    case Facing::East:
        return {16.0F - to.z, 16.0F - to.y, 16.0F - from.z, 16.0F - from.y, false};
    }
    return {from.x, from.z, to.x, to.z, false};
}

// JE CuboidFace.UVs.getVertexU/getVertexV: which of the rect's corners a rect-
// local vertex index (0..3) samples.
[[nodiscard]] constexpr float vertexU(const FaceUv& uv, int index) {
    return (index != 0 && index != 1) ? uv.maxU : uv.minU;
}
[[nodiscard]] constexpr float vertexV(const FaceUv& uv, int index) {
    return (index != 0 && index != 3) ? uv.maxV : uv.minV;
}

// JE Quadrant.rotateVertexIndex: the per-face `rotation` (quarter-turns 0..3)
// as a cyclic shift of the sampled corner.
[[nodiscard]] constexpr int rotateVertexIndex(int vertex, std::uint8_t quarterTurns) {
    return (vertex + static_cast<int>(quarterTurns)) & 3;
}

// --- A single face of a model element (JE CuboidFace, minus atlas concepts).
// `slot` is passed through untouched; slot->layer resolution is an N3 wiring step.
struct ElementFace final {
    bool present = false;
    std::uint8_t slot = 0;
    FaceUv uv{};
    std::uint8_t quadrant = 0; // JE face `rotation` / 90
    std::int8_t tintIndex = -1;
    std::uint8_t cull = kNoCull;
};

// --- Element rotation (JE CuboidRotation), single-axis form (the only kind the
// current content — the lever's 45° handle — uses; EulerXYZ can be added later).
// `origin` is in 0..16 model units.
struct ElementRotation final {
    bool present = false;
    glm::vec3 origin{8.0F, 8.0F, 8.0F};
    char axis = 'y';
    float angleDeg = 0.0F;
    bool rescale = false;
};

// --- The blockstate-level model transform (JE ModelState.transformation): the
// rigid rotation that turns the base model onto its FACING. A plain rotation
// matrix; N4 supplies the per-orientation table. uvlock (JE
// inverseFaceTransformation) is an N6 field, not yet applied.
struct ModelTransform final {
    glm::mat3 rotation{1.0F};
};

// The baked quad: cell-local 0..1 positions and layer-local 0..1 UVs, both with
// element + model rotation applied, plus the recomputed facing. Slot/tint/cull
// ride along for the wiring layers; light/AO/layer are not this primitive's job.
struct BakedQuad final {
    std::array<glm::vec3, 4> position{};
    std::array<glm::vec2, 4> uv{};
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    Facing facing = Facing::Up;
    std::uint8_t slot = 0;
    std::int8_t tintIndex = -1;
    std::uint8_t cull = kNoCull;
};

namespace detail {

inline constexpr float kPi = 3.14159265358979323846F;

// The facing whose unit vector `v` points most nearly along — JE
// Direction.getApproximateNearest, used to carry a cullface through a model
// rotation.
[[nodiscard]] inline Facing nearestFacing(const glm::vec3& v) {
    Facing best = Facing::Up;
    float bestDot = -2.0F;
    for (std::uint8_t f = 0; f < kFacingCount; ++f) {
        const float dot = glm::dot(v, facingUnit(static_cast<Facing>(f)));
        if (dot > bestDot) {
            bestDot = dot;
            best = static_cast<Facing>(f);
        }
    }
    return best;
}

// The cullface declaration carried through a model-state rotation. kNoCull
// (a face vanilla left undeclared) stays kNoCull.
[[nodiscard]] inline std::uint8_t rotateCull(std::uint8_t cull, const glm::mat3& rotation) {
    if (cull == kNoCull) {
        return cull;
    }
    return static_cast<std::uint8_t>(
        nearestFacing(rotation * facingUnit(static_cast<Facing>(cull))));
}

// A right-handed single-axis rotation as a glm::mat3, matching ChunkMesher's
// rotateAxis convention exactly (so wired content behaves identically).
[[nodiscard]] inline glm::mat3 singleAxisMatrix(char axis, float degrees) {
    const float r = degrees * kPi / 180.0F;
    const float c = std::cos(r);
    const float s = std::sin(r);
    // glm::mat3 is column-major: mat3(col0, col1, col2), out = c0*x + c1*y + c2*z.
    if (axis == 'x') {
        return glm::mat3({1.0F, 0.0F, 0.0F}, {0.0F, c, s}, {0.0F, -s, c});
    }
    if (axis == 'y') {
        return glm::mat3({c, 0.0F, -s}, {0.0F, 1.0F, 0.0F}, {s, 0.0F, c});
    }
    return glm::mat3({c, s, 0.0F}, {-s, c, 0.0F}, {0.0F, 0.0F, 1.0F});
}

// JE CuboidRotation.computeRescale: for a 45°-class rotation, stretch each axis
// so the rotated box fills its original footprint (scale = 1 / max component of
// the transformed axis).
[[nodiscard]] inline glm::mat3 withRescale(const glm::mat3& rotation) {
    glm::vec3 scale{1.0F};
    const std::array<glm::vec3, 3> axes{{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F},
                                         {0.0F, 0.0F, 1.0F}}};
    for (int i = 0; i < 3; ++i) {
        const glm::vec3 t = rotation * axes[static_cast<std::size_t>(i)];
        const float m = std::max(std::max(std::fabs(t.x), std::fabs(t.y)), std::fabs(t.z));
        scale[i] = m > 0.0F ? 1.0F / m : 1.0F;
    }
    // Post-multiply by the anisotropic scale (JE Matrix4f.scale).
    return rotation * glm::mat3(glm::vec3{scale.x, 0.0F, 0.0F}, glm::vec3{0.0F, scale.y, 0.0F},
                                glm::vec3{0.0F, 0.0F, scale.z});
}

[[nodiscard]] inline glm::mat3 elementMatrix(const ElementRotation& rot) {
    glm::mat3 m = singleAxisMatrix(rot.axis, rot.angleDeg);
    return rot.rescale ? withRescale(m) : m;
}

// JE FaceBakery.rotateVertexBy: rotate a point about an origin.
[[nodiscard]] inline glm::vec3 rotateAbout(const glm::vec3& v, const glm::vec3& origin,
                                           const glm::mat3& m) {
    return origin + m * (v - origin);
}

// JE FaceBakery.calculateFacing + findClosestDirection: the face's real normal
// after rotation, snapped to the closest of the six axes. `found` is false when
// the quad is degenerate (JE returns null).
struct FacingResult final {
    bool found = false;
    Facing facing = Facing::Up;
};

[[nodiscard]] inline FacingResult calculateFacing(const std::array<glm::vec3, 4>& p) {
    const glm::vec3 n = glm::cross(p[1] - p[0], p[2] - p[0]);
    if (!(std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z))) {
        return {};
    }
    FacingResult best;
    float bestDot = 0.0F;
    for (std::uint8_t i = 0; i < kFacingCount; ++i) {
        const auto facing = static_cast<Facing>(i);
        const float dot = glm::dot(n, facingUnit(facing));
        if (dot > 0.0F && dot > bestDot) {
            bestDot = dot;
            best = {true, facing};
        }
    }
    return best;
}

[[nodiscard]] inline float extentScalar(Extent e, float lo, float hi) {
    return e == Extent::Max ? hi : lo;
}

// JE FaceBakery.recalculateWinding: after rotation re-sort the four vertices (and
// their UVs) into the canonical order the final facing's FaceInfo dictates, so
// winding/normal stay consistent. Runs only for non-element-rotated quads (JE:
// `elementRotation == null`). Vertices are exact axis-aligned box corners here,
// so a small epsilon match is safe.
inline void recalculateWinding(std::array<glm::vec3, 4>& positions,
                               std::array<glm::vec2, 4>& uvs, Facing facing) {
    glm::vec3 lo{999.0F, 999.0F, 999.0F};
    glm::vec3 hi{-999.0F, -999.0F, -999.0F};
    for (const auto& p : positions) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const auto& info = kFaceInfo[static_cast<std::size_t>(facing)];
    for (int vertex = 0; vertex < 4; ++vertex) {
        const VertexInfo& vi = info[static_cast<std::size_t>(vertex)];
        const glm::vec3 target{extentScalar(vi.x, lo.x, hi.x), extentScalar(vi.y, lo.y, hi.y),
                               extentScalar(vi.z, lo.z, hi.z)};
        int swapWith = -1;
        for (int i = vertex; i < 4; ++i) {
            if (glm::length(positions[static_cast<std::size_t>(i)] - target) < 1.0e-4F) {
                swapWith = i;
                break;
            }
        }
        if (swapWith < 0 || swapWith == vertex) {
            continue;
        }
        std::swap(positions[static_cast<std::size_t>(vertex)],
                  positions[static_cast<std::size_t>(swapWith)]);
        std::swap(uvs[static_cast<std::size_t>(vertex)], uvs[static_cast<std::size_t>(swapWith)]);
    }
}

} // namespace detail

// A right-handed single-axis rotation matrix (degrees), the same convention as
// ChunkMesher's rotateAxis. Public so the wiring layers can build a ModelTransform
// for a block's attachment/FACING rotation.
[[nodiscard]] inline glm::mat3 axisMatrix(char axis, float degrees) {
    return detail::singleAxisMatrix(axis, degrees);
}

// The primitive. `from16`/`to16` are the element box in 0..16 model units.
// Mirrors FaceBakery.bakeQuad step for step; see the header preamble for what is
// intentionally left to the wiring layers.
[[nodiscard]] inline BakedQuad bakeElementFace(const glm::vec3& from16, const glm::vec3& to16,
                                               Facing dir, const ElementFace& face,
                                               const ElementRotation& elementRotation,
                                               const ModelTransform& state) {
    const auto& info = kFaceInfo[static_cast<std::size_t>(dir)];
    const glm::mat3 elementM = elementRotation.present ? detail::elementMatrix(elementRotation)
                                                       : glm::mat3(1.0F);
    const glm::vec3 elementOrigin = elementRotation.origin / 16.0F;
    constexpr glm::vec3 blockMiddle{0.5F, 0.5F, 0.5F};

    BakedQuad quad;
    quad.slot = face.slot;
    quad.tintIndex = face.tintIndex;
    // RN-8b: JE BlockModel.bakeFace rotates the cullface with the model state
    // (`Direction.rotate(state.getRotation().getMatrix(), face.cullface())`), so
    // a lever bolted to the ceiling culls against the block above rather than the
    // one below. Element rotation deliberately does not enter here: JE only
    // carries the *state* rotation into the cullface, and an element-rotated face
    // is no longer axis-aligned anyway.
    quad.cull = detail::rotateCull(face.cull, state.rotation);

    const FaceUv uv = face.uv.absent ? defaultFaceUv(from16, to16, dir) : face.uv;

    for (int i = 0; i < 4; ++i) {
        glm::vec3 v = faceVertex(info[static_cast<std::size_t>(i)], from16, to16) / 16.0F;
        if (elementRotation.present) {
            v = detail::rotateAbout(v, elementOrigin, elementM);
        }
        v = detail::rotateAbout(v, blockMiddle, state.rotation);
        quad.position[static_cast<std::size_t>(i)] = v;

        const int corner = rotateVertexIndex(i, face.quadrant);
        quad.uv[static_cast<std::size_t>(i)] = {vertexU(uv, corner) / 16.0F,
                                                vertexV(uv, corner) / 16.0F};
    }

    const detail::FacingResult facing = detail::calculateFacing(quad.position);
    quad.facing = facing.found ? facing.facing : Facing::Up;
    quad.normal = facing.found ? facingUnit(facing.facing)
                               : glm::normalize(glm::cross(quad.position[1] - quad.position[0],
                                                           quad.position[2] - quad.position[0]));
    // JE bakes winding only when there is no element rotation.
    if (!elementRotation.present && facing.found) {
        detail::recalculateWinding(quad.position, quad.uv, facing.facing);
    }
    return quad;
}

} // namespace mc::world::bake

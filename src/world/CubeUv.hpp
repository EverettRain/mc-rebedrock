#pragma once

// RN-8c: the mesher's face/corner order, and the one bridge from it to the
// bakery's. Everything that draws a block — the chunk mesh, the dropped item, the
// held item and the inventory icon — has to agree about "which corner of the
// sprite does this vertex sample", and this header is where that agreement is
// stated once.
//
// It used to be stated four times and disagree three ways: `kUvs` for cubes,
// a position-derived formula for shaped blocks, literal rect arrays for the
// billboards, and FaceBakery for the element models. The item surfaces made it
// five, because their cubes are generated in a vertex shader and carried their
// own copy — which is why the world could be fixed while every inventory icon
// stayed a quarter turn out.

#include "world/BlockShape.hpp"
#include "world/FaceBakery.hpp"

#include <array>
#include <cstdint>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace mc::world {

struct FaceDefinition final {
    Face face;
    int dx;
    int dy;
    int dz;
    glm::vec3 normal;
    std::array<glm::vec3, 4> corners;
};

constexpr std::array<FaceDefinition, 6> kFaces{{
    {Face::PositiveX, 1, 0, 0, {1, 0, 0}, {{{1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}}}},
    {Face::NegativeX, -1, 0, 0, {-1, 0, 0}, {{{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}}},
    {Face::PositiveY, 0, 1, 0, {0, 1, 0}, {{{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}}},
    {Face::NegativeY, 0, -1, 0, {0, -1, 0}, {{{0, 0, 1}, {0, 0, 0}, {1, 0, 0}, {1, 0, 1}}}},
    {Face::PositiveZ, 0, 0, 1, {0, 0, 1}, {{{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}}},
    {Face::NegativeZ, 0, 0, -1, {0, 0, -1}, {{{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}}}},
}};

// --- RN-8c: the one bridge between the mesher's face/corner order and the
// bakery's. Every UV in this file is derived through it from here on, so
// "which corner of the sprite does this vertex sample" has one answer instead of
// the four conventions that used to disagree (kUvs for cubes, a position-derived
// formula for shaped blocks, literal rect arrays for the billboards, and
// FaceBakery for the element models).

[[nodiscard]] constexpr bake::Facing bakeFacingOf(Face face) {
    switch (face) {
    case Face::PositiveX: return bake::Facing::East;
    case Face::NegativeX: return bake::Facing::West;
    case Face::PositiveY: return bake::Facing::Up;
    case Face::NegativeY: return bake::Facing::Down;
    case Face::PositiveZ: return bake::Facing::South;
    case Face::NegativeZ: return bake::Facing::North;
    }
    return bake::Facing::Up;
}

// For each mesher face and each of its four corners, the JE FaceInfo vertex index
// standing at the same box corner. Derived by matching positions rather than
// written down: the two orders agree on the caps and differ by one step on the
// four sides, and a hand-copied permutation is exactly the table that rots when
// either order is touched. The static_assert below is the guard.
inline constexpr std::array<std::array<std::uint8_t, 4>, 6> kFaceInfoCorner = [] {
    std::array<std::array<std::uint8_t, 4>, 6> table{};
    constexpr glm::vec3 unitFrom{0.0F, 0.0F, 0.0F};
    constexpr glm::vec3 unitTo{1.0F, 1.0F, 1.0F};
    for (std::size_t f = 0; f < kFaces.size(); ++f) {
        const auto dir = bakeFacingOf(kFaces[f].face);
        const auto& info = bake::kFaceInfo[static_cast<std::size_t>(dir)];
        for (std::size_t c = 0; c < 4; ++c) {
            const glm::vec3 want = kFaces[f].corners[c];
            std::uint8_t found = 4U;
            for (std::size_t i = 0; i < 4; ++i) {
                const glm::vec3 got = bake::faceVertex(info[i], unitFrom, unitTo);
                if (got.x == want.x && got.y == want.y && got.z == want.z) {
                    found = static_cast<std::uint8_t>(i);
                }
            }
            table[f][c] = found;
        }
    }
    return table;
}();
// Every mesher corner must have found a FaceInfo twin; a 4 here means the two
// vertex layouts stopped describing the same box.
static_assert([] {
    for (const auto& row : kFaceInfoCorner) {
        for (const auto index : row) {
            if (index > 3U) return false;
        }
    }
    return true;
}());

// The UV of a box face's four corners, in the mesher's corner order: JE's
// `defaultFaceUV` rect for the box (or an explicit rect a model json declares),
// sampled through FaceInfo and rotated by the face's Quadrant. `from16`/`to16`
// are the box in 0..16 model units.
[[nodiscard]] constexpr std::array<glm::vec2, 4> faceUvCorners(const bake::FaceUv& rect,
                                                               Face face,
                                                               std::uint8_t quadrant = 0) {
    std::array<glm::vec2, 4> out{};
    const auto& perm = kFaceInfoCorner[static_cast<std::size_t>(face)];
    for (std::size_t c = 0; c < 4; ++c) {
        const int index = bake::rotateVertexIndex(static_cast<int>(perm[c]), quadrant);
        out[c] = {bake::vertexU(rect, index) / 16.0F, bake::vertexV(rect, index) / 16.0F};
    }
    return out;
}

[[nodiscard]] constexpr std::array<glm::vec2, 4> boxFaceUv(const glm::vec3& from16,
                                                           const glm::vec3& to16, Face face,
                                                           std::uint8_t quadrant = 0) {
    return faceUvCorners(bake::defaultFaceUv(from16, to16, bakeFacingOf(face)), face, quadrant);
}

// The whole-cell box, the one every full cube and every plane-sized sprite uses.
inline constexpr glm::vec3 kCellFrom16{0.0F, 0.0F, 0.0F};
inline constexpr glm::vec3 kCellTo16{16.0F, 16.0F, 16.0F};

// The corner order a quad is emitted in. A box face uses FaceInfo's order, which
// `kFaceInfoCorner` bridges; the billboard emitters in this file wind their own
// quads, and naming the two orders they use is what lets their UVs come from a
// `bake::FaceUv` rect sampled by JE's rules instead of a literal array of four
// pairs. In texture space (v downward) a rect's own four corners are
// i0 = top-left, i1 = bottom-left, i2 = bottom-right, i3 = top-right.
enum class QuadWinding : std::uint8_t {
    BottomLeftFirst, // bl, br, tr, tl — the plane emitters and the torch caps
    ColumnFirst,     // bl, tl, tr, br — the torch's four side quads
};

[[nodiscard]] constexpr std::array<glm::vec2, 4> rectUv(const bake::FaceUv& rect,
                                                        QuadWinding winding,
                                                        std::uint8_t quadrant = 0) {
    constexpr std::array<std::uint8_t, 4> kBottomLeftFirst{1, 2, 3, 0};
    constexpr std::array<std::uint8_t, 4> kColumnFirst{1, 0, 3, 2};
    const auto& order =
        winding == QuadWinding::ColumnFirst ? kColumnFirst : kBottomLeftFirst;
    std::array<glm::vec2, 4> out{};
    for (std::size_t c = 0; c < 4; ++c) {
        const int index = bake::rotateVertexIndex(static_cast<int>(order[c]), quadrant);
        out[c] = {bake::vertexU(rect, index) / 16.0F, bake::vertexV(rect, index) / 16.0F};
    }
    return out;
}

// The whole sprite, in 0..16 model units — the rect a model json face gets when
// it declares `"uv": [0, 0, 16, 16]`.
inline constexpr bake::FaceUv kWholeSpriteRect{0.0F, 0.0F, 16.0F, 16.0F, /*absent=*/false};

// A plane sprite's four corners. This is what `kUvs` was: the same numbers, now
// derived from the rect rules rather than written out as four literals.
inline constexpr std::array<glm::vec2, 4> kPlaneUv =
    rectUv(kWholeSpriteRect, QuadWinding::BottomLeftFirst);
static_assert(kPlaneUv[0].x == 0.0F && kPlaneUv[0].y == 1.0F);
static_assert(kPlaneUv[1].x == 1.0F && kPlaneUv[1].y == 1.0F);
static_assert(kPlaneUv[2].x == 1.0F && kPlaneUv[2].y == 0.0F);
static_assert(kPlaneUv[3].x == 0.0F && kPlaneUv[3].y == 0.0F);

// A cube model json's six faces as it declares them: a `uv` rect and a
// `rotation` quadrant each, in bake::Facing order. Three models cover the whole
// roster, which is the point — this is keyed by model, not by block.
struct CubeUvModelDesc final {
    std::array<bake::FaceUv, bake::kFacingCount> rect{};
    std::array<std::uint8_t, bake::kFacingCount> quadrant{};
};

[[nodiscard]] constexpr CubeUvModelDesc cubeUvModelDesc(CubeUvModel model) {
    constexpr bake::FaceUv kWholeSprite{0.0F, 0.0F, 16.0F, 16.0F, /*absent=*/false};
    CubeUvModelDesc desc;
    desc.rect.fill(kWholeSprite);
    const auto index = [](bake::Facing facing) { return static_cast<std::size_t>(facing); };
    switch (model) {
    case CubeUvModel::Default:
        // block/cube: every face takes the whole sprite, unrotated.
        break;
    case CubeUvModel::PistonTemplate:
        // template_piston.json: "rotation" 180 on down, 270 on west, 90 on east;
        // up/north/south carry none. These wrap piston_side's frame around the
        // platform.
        desc.quadrant[index(bake::Facing::Down)] = bake::kQuadrant180;
        desc.quadrant[index(bake::Facing::West)] = bake::kQuadrant270;
        desc.quadrant[index(bake::Facing::East)] = bake::kQuadrant90;
        break;
    case CubeUvModel::Observer:
        // observer.json: the up face declares "uv": [0,16,16,0] — a rect whose V
        // runs backwards. That is the registered "observer top uv-rect flipped"
        // defect, and it is a property of the model, which is why a per-face
        // quarter-turn count could never express it.
        desc.rect[index(bake::Facing::Up)] = {0.0F, 16.0F, 16.0F, 0.0F, /*absent=*/false};
        break;
    }
    return desc;
}

inline constexpr std::size_t kCubeUvModelCount = 3;

// Every cube model's six faces at the IDENTITY facing, in this file's face order
// and corner order. This is what a block item is: vanilla renders the block's own
// model with no blockstate rotation, so a cube item's UV is exactly the world
// cube's at facing=north. No rotation means no trigonometry, so unlike the
// mesher's FACING-indexed kCubeUv this one is a compile-time constant.
//
// The two vertex shaders that generate item cubes (resources/shaders/src/
// item_entity.vert for the dropped and held item, hud.vert for the inventory
// icon) cannot include this header, so they carry the same numbers as GLSL
// literals. `item_cube_uv_test` parses both shaders and checks them against this
// table, which is what stops the two from drifting apart again.
inline constexpr std::array<std::array<std::array<glm::vec2, 4>, 6>, kCubeUvModelCount>
    kCubeModelFaceUv = [] {
        std::array<std::array<std::array<glm::vec2, 4>, 6>, kCubeUvModelCount> table{};
        for (std::size_t m = 0; m < kCubeUvModelCount; ++m) {
            const CubeUvModelDesc desc = cubeUvModelDesc(static_cast<CubeUvModel>(m));
            for (std::size_t f = 0; f < kFaces.size(); ++f) {
                const Face face = kFaces[f].face;
                const auto dir = static_cast<std::size_t>(bakeFacingOf(face));
                table[m][f] = faceUvCorners(desc.rect[dir], face, desc.quadrant[dir]);
            }
        }
        return table;
    }();

// The plain cube's table, the one every block without its own declared cube model
// uses.
inline constexpr auto& kCubeItemFaceUv =
    kCubeModelFaceUv[static_cast<std::size_t>(CubeUvModel::Default)];

// Which of a block item's five layers a given face of its cube shows — the
// CPU-side statement of what item_entity.vert selects, so the routing can be
// asserted without a GPU. A block item is the block's own model unrotated, so the
// front is on the model's north face and the back on its south one.
//
// This exists because the routing HAD a bug a headless test could not see: the
// dropped cube took top/side/bottom from `textureLayers`, and the atlas baker
// deliberately puts a DirectionalCube's FRONT layer in that triple's `side` slot
// (so the old three-slot item cube would at least be recognisable). Feeding the
// real front in as well then put it on three faces. The fix is to take every
// layer from `cubeItemLayers`; this function is what pins it.
[[nodiscard]] inline float cubeItemFaceLayer(const CubeItemLayers& layers, Face face) {
    switch (face) {
    case Face::PositiveY: return layers.top;
    case Face::NegativeY: return layers.bottom;
    case Face::NegativeZ: return layers.front; // the model's north face
    case Face::PositiveZ: return layers.back;  // its south face
    case Face::PositiveX:
    case Face::NegativeX: return layers.side;
    }
    return layers.side;
}

} // namespace mc::world

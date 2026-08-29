#include "world/ElementModelBaker.hpp"

#include <array>
#include <cassert>
#include <cmath>

// RN-4 N2a: the ElementModel description + baker (world/ElementModelBaker.hpp),
// the shared "model elements -> N1 FaceBakery -> quads" pipeline the diodes and
// lever will be wired onto in N2b. This locks it WITHOUT touching the mesher:
//  - geometry is checked against an INDEPENDENT box-face-corner enumeration (not
//    kFaceInfo), so a drift in the primitive's face binding is caught here;
//  - UV corners, texture slots and quad counts match the transcription;
//  - two hand-computed points lock the attachment transform (identity + yaw 90).

namespace {

using namespace mc::world::bake;
using mc::world::Block;
using mc::world::BlockOrientation;
using mc::world::BlockState;

[[nodiscard]] bool near(float a, float b) { return std::fabs(a - b) < 1.0e-4F; }
[[nodiscard]] bool eqVec3(const glm::vec3& a, const glm::vec3& b) {
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}
[[nodiscard]] bool eqVec2(const glm::vec2& a, const glm::vec2& b) {
    return near(a.x, b.x) && near(a.y, b.y);
}

// Order-independent equality of two 4-corner sets.
template <typename T, typename Eq>
[[nodiscard]] bool setEq(const std::array<T, 4>& a, const std::array<T, 4>& b, Eq eq) {
    std::array<bool, 4> used{};
    for (const T& x : a) {
        bool matched = false;
        for (std::size_t j = 0; j < 4; ++j) {
            if (!used[j] && eq(x, b[j])) {
                used[j] = true;
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool contains(const std::array<glm::vec3, 4>& set, const glm::vec3& p) {
    for (const glm::vec3& q : set) {
        if (eqVec3(q, p)) {
            return true;
        }
    }
    return false;
}

// The four corners of a box face, enumerated directly from min/max — deliberately
// independent of kFaceInfo so it can serve as the oracle for the primitive's face
// binding.
[[nodiscard]] std::array<glm::vec3, 4> boxFaceCorners(const glm::vec3& f, const glm::vec3& t,
                                                      Facing face) {
    switch (face) {
    case Facing::Down:
        return {{{f.x, f.y, f.z}, {t.x, f.y, f.z}, {f.x, f.y, t.z}, {t.x, f.y, t.z}}};
    case Facing::Up:
        return {{{f.x, t.y, f.z}, {t.x, t.y, f.z}, {f.x, t.y, t.z}, {t.x, t.y, t.z}}};
    case Facing::North:
        return {{{f.x, f.y, f.z}, {t.x, f.y, f.z}, {f.x, t.y, f.z}, {t.x, t.y, f.z}}};
    case Facing::South:
        return {{{f.x, f.y, t.z}, {t.x, f.y, t.z}, {f.x, t.y, t.z}, {t.x, t.y, t.z}}};
    case Facing::West:
        return {{{f.x, f.y, f.z}, {f.x, t.y, f.z}, {f.x, f.y, t.z}, {f.x, t.y, t.z}}};
    case Facing::East:
        return {{{t.x, f.y, f.z}, {t.x, t.y, f.z}, {t.x, f.y, t.z}, {t.x, t.y, t.z}}};
    }
    return {};
}

// The oracle transform, mirroring bakeElementFace's order (element rotation in
// 0..16 space about origin16, then /16, then the attachment about the block
// centre) but starting from the independent box corners.
[[nodiscard]] glm::vec3 oracleTransform(glm::vec3 c16, const ModelElement& el,
                                        const ModelTransform& attach) {
    if (el.rotation.present) {
        const glm::mat3 m = axisMatrix(el.rotation.axis, el.rotation.angleDeg);
        c16 = el.rotation.origin + m * (c16 - el.rotation.origin);
    }
    glm::vec3 v = c16 / 16.0F;
    v = glm::vec3(0.5F) + attach.rotation * (v - glm::vec3(0.5F));
    return v;
}

void checkBlock(Block block, BlockState state, std::size_t expectedQuads) {
    const std::vector<BakedElementQuad> quads = bakeElementModel(block, state);
    assert(quads.size() == expectedQuads);

    const std::vector<ModelElement> elements = elementsFor(block, state);
    const ModelTransform attach = attachTransform(block, state);

    std::size_t qi = 0;
    for (const ModelElement& el : elements) {
        for (std::uint8_t f = 0; f < kFacingCount; ++f) {
            const ElementFace& face = el.faces[f];
            if (!face.present) {
                continue;
            }
            const BakedQuad& q = quads[qi++].quad;
            assert(q.slot == face.slot);

            // Geometry: baker positions == independently enumerated face corners
            // under the same transform, as a set.
            const auto corners = boxFaceCorners(el.from16, el.to16, static_cast<Facing>(f));
            std::array<glm::vec3, 4> expected{};
            for (std::size_t i = 0; i < 4; ++i) {
                expected[i] = oracleTransform(corners[i], el, attach);
            }
            assert(setEq(q.position, expected, [](const glm::vec3& a, const glm::vec3& b) {
                return eqVec3(a, b);
            }));

            // UV: baker corners == the rect's four corners, as a set.
            const FaceUv& uv = face.uv;
            const std::array<glm::vec2, 4> uvCorners{{{uv.minU / 16.0F, uv.minV / 16.0F},
                                                      {uv.minU / 16.0F, uv.maxV / 16.0F},
                                                      {uv.maxU / 16.0F, uv.minV / 16.0F},
                                                      {uv.maxU / 16.0F, uv.maxV / 16.0F}}};
            assert(setEq(q.uv, uvCorners, [](const glm::vec2& a, const glm::vec2& b) {
                return eqVec2(a, b);
            }));
        }
    }
    assert(qi == quads.size());
}

// An independent golden transcription of the element geometry/UV, hand-copied
// from ChunkMesher's appendElementModel — NOT read from elementsFor. This is the
// second, independent copy that catches a drift in the description table itself
// (the geometry/UV set checks above share elementsFor with their oracle, so they
// only lock the primitive's logic, not the transcription's numbers).
struct FaceExp final {
    bool present = false;
    std::uint8_t slot = 0;
    float minU = 0, minV = 0, maxU = 0, maxV = 0;
};
struct ElemExp final {
    glm::vec3 from{}, to{};
    std::array<FaceExp, 6> faces{}; // Down,Up,North,South,West,East
    bool rotPresent = false;
    glm::vec3 rotOrigin{};
    char rotAxis = 'y';
    float rotAngle = 0.0F;
};

FaceExp F(std::uint8_t slot, float a, float b, float c, float d) {
    return {true, slot, a, b, c, d};
}

// Diode base golden: up=#top slot1, four sides=#slab slot0, no down.
ElemExp goldenDiodeBase() {
    ElemExp e;
    e.from = {0, 0, 0};
    e.to = {16, 2, 16};
    e.faces[1] = F(1, 0, 0, 16, 16);                          // Up
    for (std::size_t s : {2, 3, 4, 5}) e.faces[s] = F(0, 0, 14, 16, 16); // N/S/W/E
    return e;
}
ElemExp goldenTorch(glm::vec3 from, glm::vec3 to, std::uint8_t slot) {
    ElemExp e;
    e.from = from;
    e.to = to;
    e.faces[1] = F(slot, 7, 6, 9, 8);                          // Up
    for (std::size_t s : {2, 3, 4, 5}) e.faces[s] = F(slot, 7, 6, 9, 11);
    return e;
}

void compareElements(const std::vector<ModelElement>& actual,
                     const std::vector<ElemExp>& expected) {
    assert(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const ModelElement& a = actual[i];
        const ElemExp& e = expected[i];
        assert(eqVec3(a.from16, e.from) && eqVec3(a.to16, e.to));
        assert(a.rotation.present == e.rotPresent);
        if (e.rotPresent) {
            assert(eqVec3(a.rotation.origin, e.rotOrigin) && a.rotation.axis == e.rotAxis &&
                   near(a.rotation.angleDeg, e.rotAngle));
        }
        for (std::size_t f = 0; f < 6; ++f) {
            assert(a.faces[f].present == e.faces[f].present);
            if (!e.faces[f].present) {
                continue;
            }
            assert(a.faces[f].slot == e.faces[f].slot);
            assert(near(a.faces[f].uv.minU, e.faces[f].minU) &&
                   near(a.faces[f].uv.minV, e.faces[f].minV) &&
                   near(a.faces[f].uv.maxU, e.faces[f].maxU) &&
                   near(a.faces[f].uv.maxV, e.faces[f].maxV));
        }
    }
}

void checkTranscription() {
    // Repeater delay 1, unpowered: torch slot 2, second torch at movingZ=6.
    compareElements(
        elementsFor(Block::Repeater, BlockState{Block::Repeater, BlockOrientation::South}),
        {goldenDiodeBase(), goldenTorch({7, 2, 2}, {9, 7, 4}, 2),
         goldenTorch({7, 2, 6}, {9, 7, 8}, 2)});
    // Repeater delay 4 powered: torch slot 3, second torch at movingZ=6+3*2=12.
    compareElements(
        elementsFor(Block::Repeater,
                    BlockState{Block::Repeater, BlockOrientation::South}.withRepeaterDelay(4)
                        .withPowered(true)),
        {goldenDiodeBase(), goldenTorch({7, 2, 2}, {9, 7, 4}, 3),
         goldenTorch({7, 2, 12}, {9, 7, 14}, 3)});
    // Comparator, normal: front torch top=5.
    compareElements(
        elementsFor(Block::Comparator, BlockState{Block::Comparator, BlockOrientation::South}),
        {goldenDiodeBase(), goldenTorch({4, 2, 11}, {6, 7, 13}, 2),
         goldenTorch({10, 2, 11}, {12, 7, 13}, 2), goldenTorch({7, 2, 2}, {9, 5, 4}, 2)});
    // Comparator, subtract: front torch top rises to 6.
    compareElements(
        elementsFor(Block::Comparator,
                    BlockState{Block::Comparator, BlockOrientation::South}.withComparatorSubtract(
                        true)),
        {goldenDiodeBase(), goldenTorch({4, 2, 11}, {6, 7, 13}, 2),
         goldenTorch({10, 2, 11}, {12, 7, 13}, 2), goldenTorch({7, 2, 2}, {9, 6, 4}, 2)});
    // Lever, unpowered: base #base slot0, handle #lever slot1 tilted +45 about x.
    ElemExp base;
    base.from = {5, -0.02F, 4};
    base.to = {11, 2.98F, 12};
    base.faces[0] = F(0, 5, 4, 11, 12); // Down
    base.faces[1] = F(0, 5, 4, 11, 12); // Up
    for (std::size_t s : {2, 3, 4, 5}) base.faces[s] = F(0, 4, 0, 12, 3);
    ElemExp handle;
    handle.from = {7, 1, 7};
    handle.to = {9, 11, 9};
    handle.rotPresent = true;
    handle.rotOrigin = {8, 1, 8};
    handle.rotAxis = 'x';
    handle.rotAngle = 45.0F;
    handle.faces[1] = F(1, 7, 6, 9, 8);
    for (std::size_t s : {2, 3, 4, 5}) handle.faces[s] = F(1, 7, 6, 9, 16);
    compareElements(elementsFor(Block::Lever, BlockState{Block::Lever, BlockOrientation::Up}),
                    {base, handle});
}

} // namespace

int main() {
    checkTranscription();

    // Repeater: base(5) + two torches(5+5) = 15; torch sprite unlit=2 / lit=3.
    checkBlock(Block::Repeater, BlockState{Block::Repeater, BlockOrientation::South}, 15);
    checkBlock(Block::Repeater,
               BlockState{Block::Repeater, BlockOrientation::East}.withRepeaterDelay(4).withPowered(
                   true),
               15);
    // Comparator: base(5) + three torches = 20.
    checkBlock(Block::Comparator, BlockState{Block::Comparator, BlockOrientation::North}, 20);
    checkBlock(
        Block::Comparator,
        BlockState{Block::Comparator, BlockOrientation::East}.withComparatorSubtract(true), 20);
    // Lever: base(6) + handle(5) = 11.
    checkBlock(Block::Lever, BlockState{Block::Lever, BlockOrientation::Up}, 11);
    checkBlock(Block::Lever, BlockState{Block::Lever, BlockOrientation::West}.withPowered(true), 11);

    // Torch slot flips with power (repeater output torch).
    {
        const auto off = bakeElementModel(Block::Repeater,
                                          BlockState{Block::Repeater, BlockOrientation::South});
        const auto on = bakeElementModel(
            Block::Repeater,
            BlockState{Block::Repeater, BlockOrientation::South}.withPowered(true));
        assert(off[5].quad.slot == 2 && on[5].quad.slot == 3); // first torch's up face (after base's 5)
    }

    // Attachment lock #1 (identity, South): the diode base up face carries the
    // raw box corners /16 — a fully independent numeric check.
    {
        const auto quads = bakeElementModel(Block::Repeater,
                                            BlockState{Block::Repeater, BlockOrientation::South});
        const BakedQuad& baseUp = quads[0].quad; // base, Up is first present face
        assert(contains(baseUp.position, glm::vec3{0.0F, 0.125F, 0.0F}));
        assert(contains(baseUp.position, glm::vec3{1.0F, 0.125F, 1.0F}));
    }

    // Attachment lock #2 (yaw 90, East): (0,2,0)/16 maps to (0, 0.125, 1) under
    // Ry(90) about the block centre — hand-computed, independent of axisMatrix.
    {
        const auto quads = bakeElementModel(Block::Repeater,
                                            BlockState{Block::Repeater, BlockOrientation::East});
        const BakedQuad& baseUp = quads[0].quad;
        assert(contains(baseUp.position, glm::vec3{0.0F, 0.125F, 1.0F}));
    }

    return 0;
}

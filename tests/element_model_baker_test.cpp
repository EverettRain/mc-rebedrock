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
using mc::world::DoorHinge;

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

// An independent golden transcription of the element geometry/UV, taken face by
// face from the vanilla model jsons (template_anvil / enchanting_table /
// repeater_Ntick / comparator / lever) — NOT read from elementsFor. This is the
// second, independent copy that catches a drift in the description table itself
// (the geometry/UV set checks above share elementsFor with their oracle, so they
// only lock the primitive's logic, not the transcription's numbers).
struct FaceExp final {
    bool present = false;
    std::uint8_t slot = 0;
    float minU = 0, minV = 0, maxU = 0, maxV = 0;
    std::uint8_t quadrant = 0; // the json face `"rotation"` / 90 (RN-8d)
};
struct ElemExp final {
    glm::vec3 from{}, to{};
    std::array<FaceExp, 6> faces{}; // Down,Up,North,South,West,East
    bool rotPresent = false;
    glm::vec3 rotOrigin{};
    char rotAxis = 'y';
    float rotAngle = 0.0F;
};

FaceExp F(std::uint8_t slot, float a, float b, float c, float d, std::uint8_t quadrant = 0) {
    return {true, slot, a, b, c, d, quadrant};
}

// Diode base golden: down=#slab slot0 (RN-10d / R15 — it was omitted, so a
// repeater on glass or a slab showed the world through its own plate),
// up=#top slot1, four sides=#slab slot0.
// RN-13-1: `lit` picks the `#top` binding, the ONE texture an `_on` diode model
// changes (repeater_1tick_on.json / comparator_on.json swap `#top` and nothing
// else). Slot 1 is block/repeater|comparator, slot 5 block/*_on.
ElemExp goldenDiodeBase(bool lit = false) {
    ElemExp e;
    e.from = {0, 0, 0};
    e.to = {16, 2, 16};
    e.faces[0] = F(0, 0, 0, 16, 16);                          // Down
    e.faces[1] = F(lit ? 5 : 1, 0, 0, 16, 16);                // Up
    for (std::size_t s : {2, 3, 4, 5}) e.faces[s] = F(0, 0, 14, 16, 16); // N/S/W/E
    return e;
}

// RN-10d: one halo billboard — a 3x3 box showing a single face, textured with
// one pixel of the lit sprite. `faceIndex` is Down..East. The six boxes per lit
// torch are written out LITERALLY at the call sites from the vanilla json, not
// derived, because the production side derives them from a rule and an oracle
// that shared the rule would prove nothing.
ElemExp goldenHalo(std::size_t faceIndex, glm::vec3 from, glm::vec3 to, float u0, float v0,
                   float u1, float v1) {
    ElemExp e;
    e.from = from;
    e.to = to;
    e.faces[faceIndex] = F(3, u0, v0, u1, v1);
    return e;
}
// RN-8d: a nub's side rect starts at v=6 and runs as many rows as the box is
// tall — [7,6,9,11] for the 5-tall repeater torches (repeater_1tick.json),
// [7,6,9,9] for the 3-tall comparator front torch (comparator.json). The golden
// derives it the same way the model json states it, so a nub of a new height
// cannot silently take the wrong rect.
ElemExp goldenTorch(glm::vec3 from, glm::vec3 to, std::uint8_t slot) {
    ElemExp e;
    e.from = from;
    e.to = to;
    e.faces[1] = F(slot, 7, 6, 9, 8);                          // Up
    for (std::size_t s : {2, 3, 4, 5}) e.faces[s] = F(slot, 7, 6, 9, 6.0F + (to.y - from.y));
    return e;
}

// RN-10e golden: the locked repeater's bar, transcribed from
// repeater_*tick_locked.json. Slot 4 is `#lock` (block/bedrock). Five faces, no
// down, and the up face carries `"rotation": 90`.
ElemExp goldenLockBar(float z) {
    ElemExp e;
    e.from = {2, 2, z};
    e.to = {14, 4, z + 2.0F};
    e.faces[1] = F(4, 7, 2, 9, 14, 1); // Up, quadrant 1 == rotation 90
    e.faces[2] = F(4, 2, 7, 14, 9);    // North
    e.faces[3] = F(4, 2, 7, 14, 9);    // South
    e.faces[4] = F(4, 6, 7, 8, 9);     // West
    e.faces[5] = F(4, 6, 7, 8, 9);     // East
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
            // RN-8d: the face's own json `"rotation"` / 90. Dropping these is
            // invisible on a symmetric sprite and glaring on the anvil's body.
            assert(a.faces[f].quadrant == e.faces[f].quadrant);
            assert(near(a.faces[f].uv.minU, e.faces[f].minU) &&
                   near(a.faces[f].uv.minV, e.faces[f].minV) &&
                   near(a.faces[f].uv.maxU, e.faces[f].maxU) &&
                   near(a.faces[f].uv.maxV, e.faces[f].maxV));
        }
    }
}

void checkTranscription() {
    // Repeater delay 1, unpowered: torch slot 2, second torch at movingZ=6, and
    // no halo — repeater_1tick.json has three elements.
    compareElements(
        elementsFor(Block::Repeater, BlockState{Block::Repeater, BlockOrientation::South}),
        {goldenDiodeBase(), goldenTorch({7, 2, 2}, {9, 7, 4}, 2),
         goldenTorch({7, 2, 6}, {9, 7, 8}, 2)});
    // Repeater delay 1, powered: repeater_1tick_on.json — both torches lit and
    // SIX halo boxes each, transcribed literally from the file.
    compareElements(
        elementsFor(Block::Repeater,
                    BlockState{Block::Repeater, BlockOrientation::South}.withPowered(true)),
        {goldenDiodeBase(/*lit=*/true), goldenTorch({7, 2, 2}, {9, 7, 4}, 3),
         goldenTorch({7, 2, 6}, {9, 7, 8}, 3),
         // halos of the fixed torch [7,2,2]-[9,7,4]
         goldenHalo(1, {6.5F, 1.5F, 1.5F}, {9.5F, 4.5F, 4.5F}, 8, 5, 9, 6),
         goldenHalo(0, {6.5F, 7.5F, 1.5F}, {9.5F, 10.5F, 4.5F}, 7, 5, 8, 6),
         goldenHalo(3, {6.5F, 4.5F, -1.5F}, {9.5F, 7.5F, 1.5F}, 9, 6, 10, 7),
         goldenHalo(2, {6.5F, 4.5F, 4.5F}, {9.5F, 7.5F, 7.5F}, 6, 6, 7, 7),
         goldenHalo(5, {3.5F, 4.5F, 1.5F}, {6.5F, 7.5F, 4.5F}, 9, 7, 10, 8),
         goldenHalo(4, {9.5F, 4.5F, 1.5F}, {12.5F, 7.5F, 4.5F}, 6, 7, 7, 8),
         // halos of the moving torch [7,2,6]-[9,7,8]
         goldenHalo(1, {6.5F, 1.5F, 5.5F}, {9.5F, 4.5F, 8.5F}, 8, 5, 9, 6),
         goldenHalo(0, {6.5F, 7.5F, 5.5F}, {9.5F, 10.5F, 8.5F}, 7, 5, 8, 6),
         goldenHalo(3, {6.5F, 4.5F, 2.5F}, {9.5F, 7.5F, 5.5F}, 9, 6, 10, 7),
         goldenHalo(2, {6.5F, 4.5F, 8.5F}, {9.5F, 7.5F, 11.5F}, 6, 6, 7, 7),
         goldenHalo(5, {3.5F, 4.5F, 5.5F}, {6.5F, 7.5F, 8.5F}, 9, 7, 10, 8),
         goldenHalo(4, {9.5F, 4.5F, 5.5F}, {12.5F, 7.5F, 8.5F}, 6, 7, 7, 8)});
    // Repeater delay 4 powered: torch slot 3, second torch at movingZ=6+3*2=12.
    // Only the element count is checked here; the halo geometry is pinned above.
    assert(elementsFor(Block::Repeater,
                       BlockState{Block::Repeater, BlockOrientation::South}
                           .withRepeaterDelay(4)
                           .withPowered(true))
               .size() == 15U);

    // RN-10e: a LOCKED repeater. repeater_2tick_locked.json has THREE elements —
    // base, the bar at the delay torch's own z, and the fixed output torch. The
    // moving torch is not in the model at all: locked replaces it, it does not
    // decorate it.
    compareElements(elementsFor(Block::Repeater,
                                BlockState{Block::Repeater, BlockOrientation::South}
                                    .withRepeaterDelay(2)
                                    .withRepeaterLocked(true)),
                    {goldenDiodeBase(), goldenLockBar(8.0F),
                     goldenTorch({7, 2, 2}, {9, 7, 4}, 2)});
    // repeater_4tick_locked.json: the bar tracks DELAY, at 6 + (delay-1)*2.
    compareElements(elementsFor(Block::Repeater,
                                BlockState{Block::Repeater, BlockOrientation::South}
                                    .withRepeaterDelay(4)
                                    .withRepeaterLocked(true)),
                    {goldenDiodeBase(), goldenLockBar(12.0F),
                     goldenTorch({7, 2, 2}, {9, 7, 4}, 2)});
    // repeater_1tick_on_locked.json: nine elements — the fixed torch lit with its
    // six halos, and still no moving torch (so six halos, not twelve).
    assert(elementsFor(Block::Repeater, BlockState{Block::Repeater, BlockOrientation::South}
                                            .withRepeaterLocked(true)
                                            .withPowered(true))
               .size() == 9U);

    // The defence assertion RN-10e exists for: the bar's presence comes from the
    // LOCKED state bit and from nothing else. `elementsFor` takes a BlockState
    // and no world — it *cannot* be given one — so a future "save the state axis,
    // just look at the neighbouring diode" would have to open a neighbour-state
    // channel into the mesher's hot path to do it, which is what AR-B4-2c's D1
    // decided against. Two repeaters differing only in that bit must differ here.
    {
        const BlockState base =
            BlockState{Block::Repeater, BlockOrientation::East}.withRepeaterDelay(3);
        const auto unlocked = elementsFor(Block::Repeater, base);
        const auto locked = elementsFor(Block::Repeater, base.withRepeaterLocked(true));
        assert(unlocked.size() == 3U && locked.size() == 3U);
        // Same count, different content: the bar stands where the torch stood.
        assert(unlocked[1].faces[1].slot != locked[1].faces[1].slot);
        assert(!eqVec3(unlocked[1].from16, locked[1].from16));
    }

    // --- The comparator truth table (RN-10d / audit R11+R12), one case per
    // vanilla model file. The rear pair follows POWERED, the front torch follows
    // MODE, and the front torch's box never moves. ---
    const auto comparator = [](bool subtract, bool powered) {
        return elementsFor(Block::Comparator,
                           BlockState{Block::Comparator, BlockOrientation::South}
                               .withComparatorSubtract(subtract)
                               .withPowered(powered));
    };
    // comparator.json: 4 elements, everything unlit (slot 2).
    compareElements(comparator(false, false),
                    {goldenDiodeBase(), goldenTorch({4, 2, 11}, {6, 7, 13}, 2),
                     goldenTorch({10, 2, 11}, {12, 7, 13}, 2),
                     goldenTorch({7, 2, 2}, {9, 5, 4}, 2)});
    // comparator_subtract.json: 10 elements — front torch LIT, rear pair unlit,
    // and the front torch's six halos. Same box as unlit: [7,2,2]-[9,5,4].
    compareElements(comparator(true, false),
                    {goldenDiodeBase(), goldenTorch({4, 2, 11}, {6, 7, 13}, 2),
                     goldenTorch({10, 2, 11}, {12, 7, 13}, 2),
                     goldenTorch({7, 2, 2}, {9, 5, 4}, 3),
                     goldenHalo(1, {6.5F, -0.5F, 1.5F}, {9.5F, 2.5F, 4.5F}, 6, 5, 7, 6),
                     goldenHalo(0, {6.5F, 5.5F, 1.5F}, {9.5F, 8.5F, 4.5F}, 6, 5, 7, 6),
                     goldenHalo(3, {6.5F, 2.5F, -1.5F}, {9.5F, 5.5F, 1.5F}, 6, 5, 7, 6),
                     goldenHalo(2, {6.5F, 2.5F, 4.5F}, {9.5F, 5.5F, 7.5F}, 6, 5, 7, 6),
                     goldenHalo(5, {3.5F, 2.5F, 1.5F}, {6.5F, 5.5F, 4.5F}, 6, 5, 7, 6),
                     goldenHalo(4, {9.5F, 2.5F, 1.5F}, {12.5F, 5.5F, 4.5F}, 6, 5, 7, 6)});
    // comparator_on.json: 16 elements — rear pair LIT with twelve halos, front
    // torch unlit.
    compareElements(comparator(false, true),
                    {goldenDiodeBase(/*lit=*/true), goldenTorch({4, 2, 11}, {6, 7, 13}, 3),
                     goldenTorch({10, 2, 11}, {12, 7, 13}, 3),
                     goldenTorch({7, 2, 2}, {9, 5, 4}, 2),
                     goldenHalo(1, {3.5F, 1.5F, 10.5F}, {6.5F, 4.5F, 13.5F}, 6, 5, 7, 6),
                     goldenHalo(0, {3.5F, 7.5F, 10.5F}, {6.5F, 10.5F, 13.5F}, 6, 5, 7, 6),
                     goldenHalo(3, {3.5F, 4.5F, 7.5F}, {6.5F, 7.5F, 10.5F}, 6, 5, 7, 6),
                     goldenHalo(2, {3.5F, 4.5F, 13.5F}, {6.5F, 7.5F, 16.5F}, 6, 5, 7, 6),
                     goldenHalo(5, {0.5F, 4.5F, 10.5F}, {3.5F, 7.5F, 13.5F}, 6, 5, 7, 6),
                     goldenHalo(4, {6.5F, 4.5F, 10.5F}, {9.5F, 7.5F, 13.5F}, 6, 5, 7, 6),
                     goldenHalo(1, {9.5F, 1.5F, 10.5F}, {12.5F, 4.5F, 13.5F}, 6, 5, 7, 6),
                     goldenHalo(0, {9.5F, 7.5F, 10.5F}, {12.5F, 10.5F, 13.5F}, 6, 5, 7, 6),
                     goldenHalo(3, {9.5F, 4.5F, 7.5F}, {12.5F, 7.5F, 10.5F}, 6, 5, 7, 6),
                     goldenHalo(2, {9.5F, 4.5F, 13.5F}, {12.5F, 7.5F, 16.5F}, 6, 5, 7, 6),
                     goldenHalo(5, {6.5F, 4.5F, 10.5F}, {9.5F, 7.5F, 13.5F}, 6, 5, 7, 6),
                     goldenHalo(4, {12.5F, 4.5F, 10.5F}, {15.5F, 7.5F, 13.5F}, 6, 5, 7, 6)});
    // comparator_on_subtract.json: 22 elements — all three torches lit.
    assert(comparator(true, true).size() == 22U);
    // RN-13-1: the top plate follows POWERED and ONLY powered. All four vanilla
    // model files agree — comparator.json and comparator_subtract.json bind the
    // unlit `#top`, comparator_on.json and comparator_on_subtract.json the lit
    // one — so a mode flip must leave slot 1 alone while a power flip must move
    // it. Driving it off MODE too would pass every count above.
    {
        const auto topSlot = [&](bool subtract, bool powered) {
            return comparator(subtract, powered)[0].faces[1].slot;
        };
        assert(topSlot(false, false) == topSlot(true, false));
        assert(topSlot(false, true) == topSlot(true, true));
        assert(topSlot(false, false) != topSlot(false, true));
    }
    // The whole point, stated as a difference rather than as a count: switching
    // MODE alone must change the front torch's sprite. Before RN-10d it did not,
    // and the only thing that moved was a 1px height this build invented.
    {
        const auto compare = comparator(false, false);
        const auto subtract = comparator(true, false);
        // element 3 is the front torch in both
        assert(compare[3].faces[1].slot != subtract[3].faces[1].slot);
        assert(eqVec3(compare[3].to16, subtract[3].to16)); // and its box does NOT move
        assert(near(compare[3].to16.y, 5.0F));
    }
    // ...and switching POWERED alone must change the rear pair, not the front.
    {
        const auto off = comparator(true, false);
        const auto on = comparator(true, true);
        assert(off[1].faces[1].slot != on[1].faces[1].slot);
        assert(off[2].faces[1].slot != on[2].faces[1].slot);
        assert(off[3].faces[1].slot == on[3].faces[1].slot);
    }
    // Vanilla marks a lit torch element `"shade": false` and leaves an unlit one
    // shaded; every halo is unshaded.
    {
        const auto on = comparator(true, true);
        assert(!on[1].shade && !on[2].shade && !on[3].shade);
        for (std::size_t i = 4; i < on.size(); ++i) {
            assert(!on[i].shade);
        }
        assert(comparator(false, false)[1].shade);
        assert(on[0].shade); // the slab base is shaded in every variant
    }

    // Lever, unpowered: base #base slot0, handle #lever slot1 tilted +45 about x.
    ElemExp base;
    base.from = {5, -0.02F, 4};
    base.to = {11, 2.98F, 12};
    base.faces[0] = F(0, 5, 4, 11, 12); // Down
    base.faces[1] = F(0, 5, 4, 11, 12); // Up
    // lever.json's side rects are not all equal: north/south [5,0,11,3] (the base
    // is 6 wide on X), west/east [4,0,12,3] (8 deep on Z).
    base.faces[2] = F(0, 5, 0, 11, 3); // North
    base.faces[3] = F(0, 5, 0, 11, 3); // South
    base.faces[4] = F(0, 4, 0, 12, 3); // West
    base.faces[5] = F(0, 4, 0, 12, 3); // East
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

    // RN-8d: the enchanting table declares no face rotation at all
    // (enchanting_table.json), so every quadrant here must stay 0 — the control
    // for the anvil golden below.
    {
        ElemExp table;
        table.from = {0, 0, 0};
        table.to = {16, 12, 16};
        table.faces[0] = F(2, 0, 0, 16, 16);                          // Down  #bottom
        table.faces[1] = F(0, 0, 0, 16, 16);                          // Up    #top
        for (std::size_t s : {2, 3, 4, 5}) table.faces[s] = F(1, 0, 4, 16, 16); // #side
        compareElements(elementsFor(Block::EnchantingTable, BlockState{Block::EnchantingTable}),
                        {table});
    }

    // RN-8d: template_anvil.json, transcribed face for face INCLUDING the
    // `"rotation"` field — 13 of its 21 faces carry one (down/up 180, west 90,
    // east 270), and all 13 were previously dropped. The uv rects that run
    // backwards (east [4,2,0,14]) are vanilla's own mirrored rects, kept as-is.
    {
        ElemExp anvilBase;
        anvilBase.from = {2, 0, 2};
        anvilBase.to = {14, 4, 14};
        anvilBase.faces[0] = F(0, 2, 2, 14, 14, 2);   // Down  rotation 180
        anvilBase.faces[1] = F(0, 2, 2, 14, 14, 2);   // Up    rotation 180
        anvilBase.faces[2] = F(0, 2, 12, 14, 16);     // North rotation 0
        anvilBase.faces[3] = F(0, 2, 12, 14, 16);     // South rotation 0
        anvilBase.faces[4] = F(0, 0, 2, 4, 14, 1);    // West  rotation 90
        anvilBase.faces[5] = F(0, 4, 2, 0, 14, 3);    // East  rotation 270

        ElemExp waist;
        waist.from = {4, 4, 3};
        waist.to = {12, 5, 13};
        waist.faces[1] = F(0, 4, 3, 12, 13, 2);       // Up    180
        waist.faces[2] = F(0, 4, 11, 12, 12);
        waist.faces[3] = F(0, 4, 11, 12, 12);
        waist.faces[4] = F(0, 4, 3, 5, 13, 1);        // West  90
        waist.faces[5] = F(0, 5, 3, 4, 13, 3);        // East  270

        ElemExp neck;
        neck.from = {6, 5, 4};
        neck.to = {10, 10, 12};
        neck.faces[2] = F(0, 6, 6, 10, 11);
        neck.faces[3] = F(0, 6, 6, 10, 11);
        neck.faces[4] = F(0, 5, 4, 10, 12, 1);        // West  90
        neck.faces[5] = F(0, 10, 4, 5, 12, 3);        // East  270

        ElemExp top;
        top.from = {3, 10, 0};
        top.to = {13, 16, 16};
        top.faces[0] = F(0, 3, 0, 13, 16, 2);         // Down  180
        top.faces[1] = F(1, 3, 0, 13, 16, 2);         // Up    180  #top slot
        top.faces[2] = F(0, 3, 0, 13, 6);
        top.faces[3] = F(0, 3, 0, 13, 6);
        top.faces[4] = F(0, 10, 0, 16, 16, 1);        // West  90
        top.faces[5] = F(0, 16, 0, 10, 16, 3);        // East  270

        for (const Block anvil : {Block::Anvil, Block::ChippedAnvil, Block::DamagedAnvil}) {
            compareElements(elementsFor(anvil, BlockState{anvil}),
                            {anvilBase, waist, neck, top});
        }

        // 13 rotated faces, no more and no less — a count, so a future
        // transcription that drops one or invents one is caught even if it lands
        // on a face this golden happens to spell the same way.
        int rotated = 0;
        for (const ModelElement& element : elementsFor(Block::Anvil, BlockState{Block::Anvil})) {
            for (const ElementFace& face : element.faces) {
                if (face.present && face.quadrant != 0) ++rotated;
            }
        }
        assert(rotated == 13);
    }
}

} // namespace

// RN-8c-0: every state the mesher can hand the store must land on the variant
// that bakes that exact state, and the store's quads must equal what
// bakeElementModel would have produced per cell. This is the whole contract of
// moving the bake to startup: same numbers, computed once.
void checkStore() {
    const ElementModelStore& store = elementModelStore();

    struct Case final {
        Block block;
        ElementModelKind kind;
    };
    const std::array<Case, 10> blocks{{
        {Block::Repeater, ElementModelKind::Repeater},
        {Block::Comparator, ElementModelKind::Comparator},
        {Block::Lever, ElementModelKind::Lever},
        {Block::EnchantingTable, ElementModelKind::EnchantingTable},
        {Block::Anvil, ElementModelKind::Anvil},
        {Block::ChippedAnvil, ElementModelKind::Anvil},
        {Block::DamagedAnvil, ElementModelKind::Anvil},
        // RN-10a/10b/10c: the three model families the store gained.
        {Block::OakDoor, ElementModelKind::Door},
        {Block::OakTrapdoor, ElementModelKind::TrapDoor},
        {Block::OakFenceGate, ElementModelKind::FenceGate},
    }};

    for (const Case& c : blocks) {
        assert(elementModelKind(c.block) == c.kind);
        for (const auto facing :
             {BlockOrientation::North, BlockOrientation::East, BlockOrientation::South,
              BlockOrientation::West, BlockOrientation::Up, BlockOrientation::Down}) {
            for (int delay = 1; delay <= 4; ++delay) {
                for (const bool powered : {false, true}) {
                    for (const bool subtract : {false, true}) {
                        // The axes the newer kinds select on ride along: a
                        // setter for a property the block does not declare is a
                        // no-op, so one loop covers every kind's whole variant
                        // space (LOCKED included — RN-10e added it to the
                        // repeater's, and a variant index that disagreed with
                        // the state decode would show up right here).
                        const BlockState state = BlockState{c.block, facing}
                                                     .withRepeaterDelay(delay)
                                                     .withPowered(powered)
                                                     .withComparatorSubtract(subtract)
                                                     .withRepeaterLocked(subtract)
                                                     .withDoorUpperHalf(powered)
                                                     .withOpen(subtract)
                                                     .withInWall(powered)
                                                     .withHinge(subtract ? DoorHinge::Right
                                                                         : DoorHinge::Left);
                        const auto stored = bakedElementModel(c.block, state);
                        const auto fresh = bakeElementModel(c.block, state);
                        assert(stored.size() == fresh.size());
                        for (std::size_t q = 0; q < fresh.size(); ++q) {
                            assert(stored[q].quad.facing == fresh[q].quad.facing);
                            assert(stored[q].quad.slot == fresh[q].quad.slot);
                            assert(stored[q].quad.cull == fresh[q].quad.cull);
                            assert(stored[q].shade == fresh[q].shade);
                            for (std::size_t i = 0; i < 4; ++i) {
                                assert(eqVec3(stored[q].quad.position[i], fresh[q].quad.position[i]));
                                assert(eqVec2(stored[q].quad.uv[i], fresh[q].quad.uv[i]));
                            }
                        }
                    }
                }
            }
        }
    }

    // A block with no ElementModel gets an empty span, never a stray range.
    assert(elementModelKind(Block::Stone) == ElementModelKind::None);
    assert(bakedElementModel(Block::Stone, BlockState{Block::Stone}).empty());

    // The three anvil wear states share one geometry table: the store is keyed by
    // model kind, and only the texture slot (resolved per block by the mesher)
    // separates them. If this ever fails, the store was keyed per block, which is
    // the ~2 MB mistake RN-8's performance section names.
    {
        const BlockState facing{Block::Anvil, BlockOrientation::East};
        const auto plain = bakedElementModel(Block::Anvil, facing);
        const auto chipped =
            bakedElementModel(Block::ChippedAnvil, BlockState{Block::ChippedAnvil,
                                                              BlockOrientation::East});
        assert(plain.data() == chipped.data() && plain.size() == chipped.size());
    }

    // Size guardrail: geometry per model, texture layer per block. Keyed per
    // block instead, this table would be megabytes.
    assert(store.byteSize() < 500U * 1024U);
    assert(store.rangeCount() ==
           elementModelVariantCount(ElementModelKind::Repeater) +
               elementModelVariantCount(ElementModelKind::Comparator) +
               elementModelVariantCount(ElementModelKind::Lever) +
               elementModelVariantCount(ElementModelKind::EnchantingTable) +
               elementModelVariantCount(ElementModelKind::Anvil) +
               elementModelVariantCount(ElementModelKind::Door) +
               elementModelVariantCount(ElementModelKind::TrapDoor) +
               elementModelVariantCount(ElementModelKind::FenceGate));
}

int main() {
    checkStore();
    checkTranscription();

    // Repeater unpowered: base(6, RN-10d added the down face) + two torches(5+5)
    // = 16; torch sprite unlit=2 / lit=3. Powered adds twelve one-face halo
    // billboards: 16 + 12 = 28.
    checkBlock(Block::Repeater, BlockState{Block::Repeater, BlockOrientation::South}, 16);
    checkBlock(Block::Repeater,
               BlockState{Block::Repeater, BlockOrientation::East}.withRepeaterDelay(4).withPowered(
                   true),
               28);
    // Comparator: base(6) + three torches(5 each) = 21, plus six halo quads per
    // LIT torch — and which torches are lit is the RN-10d truth table: rear pair
    // = POWERED, front = MODE.
    checkBlock(Block::Comparator, BlockState{Block::Comparator, BlockOrientation::North}, 21);
    checkBlock(
        Block::Comparator,
        BlockState{Block::Comparator, BlockOrientation::East}.withComparatorSubtract(true),
        21 + 6);
    checkBlock(Block::Comparator,
               BlockState{Block::Comparator, BlockOrientation::East}.withPowered(true), 21 + 12);
    checkBlock(Block::Comparator,
               BlockState{Block::Comparator, BlockOrientation::East}
                   .withComparatorSubtract(true)
                   .withPowered(true),
               21 + 18);
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
        // the first torch's up face, after the base's six.
        assert(off[6].quad.slot == 2 && on[6].quad.slot == 3);
    }

    // Attachment lock #1 (identity, South): the diode base up face carries the
    // raw box corners /16 — a fully independent numeric check.
    {
        const auto quads = bakeElementModel(Block::Repeater,
                                            BlockState{Block::Repeater, BlockOrientation::South});
        // RN-10d gave the base a down face, so Up is now the second present one.
        const BakedQuad& baseUp = quads[1].quad;
        assert(contains(baseUp.position, glm::vec3{0.0F, 0.125F, 0.0F}));
        assert(contains(baseUp.position, glm::vec3{1.0F, 0.125F, 1.0F}));
    }

    // Attachment lock #2 (yaw 90, East): (0,2,0)/16 maps to (0, 0.125, 1) under
    // Ry(90) about the block centre — hand-computed, independent of axisMatrix.
    {
        const auto quads = bakeElementModel(Block::Repeater,
                                            BlockState{Block::Repeater, BlockOrientation::East});
        const BakedQuad& baseUp = quads[1].quad;
        assert(contains(baseUp.position, glm::vec3{0.0F, 0.125F, 1.0F}));
    }

    return 0;
}

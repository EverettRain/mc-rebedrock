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

// Diode base golden: up=#top slot1, four sides=#slab slot0, no down.
ElemExp goldenDiodeBase() {
    ElemExp e;
    e.from = {0, 0, 0};
    e.to = {16, 2, 16};
    e.faces[1] = F(1, 0, 0, 16, 16);                          // Up
    for (std::size_t s : {2, 3, 4, 5}) e.faces[s] = F(0, 0, 14, 16, 16); // N/S/W/E
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
    const std::array<Case, 7> blocks{{
        {Block::Repeater, ElementModelKind::Repeater},
        {Block::Comparator, ElementModelKind::Comparator},
        {Block::Lever, ElementModelKind::Lever},
        {Block::EnchantingTable, ElementModelKind::EnchantingTable},
        {Block::Anvil, ElementModelKind::Anvil},
        {Block::ChippedAnvil, ElementModelKind::Anvil},
        {Block::DamagedAnvil, ElementModelKind::Anvil},
    }};

    for (const Case& c : blocks) {
        assert(elementModelKind(c.block) == c.kind);
        for (const auto facing :
             {BlockOrientation::North, BlockOrientation::East, BlockOrientation::South,
              BlockOrientation::West, BlockOrientation::Up, BlockOrientation::Down}) {
            for (int delay = 1; delay <= 4; ++delay) {
                for (const bool powered : {false, true}) {
                    for (const bool subtract : {false, true}) {
                        const BlockState state = BlockState{c.block, facing}
                                                     .withRepeaterDelay(delay)
                                                     .withPowered(powered)
                                                     .withComparatorSubtract(subtract);
                        const auto stored = bakedElementModel(c.block, state);
                        const auto fresh = bakeElementModel(c.block, state);
                        assert(stored.size() == fresh.size());
                        for (std::size_t q = 0; q < fresh.size(); ++q) {
                            assert(stored[q].quad.facing == fresh[q].quad.facing);
                            assert(stored[q].quad.slot == fresh[q].quad.slot);
                            assert(stored[q].quad.cull == fresh[q].quad.cull);
                            assert(near(stored[q].glow, fresh[q].glow));
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

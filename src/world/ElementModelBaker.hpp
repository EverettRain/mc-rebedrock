#pragma once

// RN-4 N2a: the data pipeline that turns an ElementModel block's transcribed
// vanilla-model elements into baked quads through the N1 FaceBakery primitive.
// This is the shared "model description -> bakeElementFace" path the diodes and
// the lever currently each open-code inside ChunkMesher's appendElementModel
// (with their own UV-corner convention). N2a introduces the description + baker
// and locks it with a test; N2b then points appendElementModel at it (a wiring
// step that also changes UV orientation to the faithful-vanilla convention, so it
// is Mac-visually verified separately). Nothing here is wired into the mesher yet.
//
// Scope: geometry + UV + texture slot only, exactly like the N1 primitive. Light,
// AO, the emissive glow of a lit redstone torch, and slot->atlas-layer resolution
// remain the mesher's job (N2b), so this header carries no render dependency and
// is unit-testable in isolation.

#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/FaceBakery.hpp"

#include <vector>

namespace mc::world::bake {

// A single box element of a model: a [from,to] box in 0..16 model units, its six
// faces (present flag on each), and an optional element rotation. The direct C++
// analogue of a vanilla model json `elements[]` entry.
struct ModelElement final {
    glm::vec3 from16{0.0F, 0.0F, 0.0F};
    glm::vec3 to16{16.0F, 16.0F, 16.0F};
    std::array<ElementFace, kFacingCount> faces{}; // indexed by Facing
    ElementRotation rotation{};
    // The extra block light this element emits (a lit redstone torch glows). The
    // JE-model analogue is CuboidModelElement.lightEmission; the mesher folds it
    // into the cell block light, so it stays a wiring concern the baker just
    // carries per element.
    float glow = 0.0F;
};

// A baked quad plus the wiring-layer light hint (glow) its element carries. The
// baker returns these; the mesher resolves slot->layer and folds glow into the
// cell light when it emits.
struct BakedElementQuad final {
    BakedQuad quad{};
    float glow = 0.0F;
};

namespace detail {

// Set one face of an element by facing. `cull` is the model json's `cullface`
// declaration (RN-8b) — the direction whose neighbour, if it seals that whole
// cell wall, hides this quad. Faces vanilla leaves undeclared stay kNoCull and
// are drawn unconditionally, which is JE's getQuads(null) half of the split.
inline void putFace(ModelElement& element, Facing facing, std::uint8_t slot, const FaceUv& uv,
                    std::uint8_t cull = kNoCull) {
    ElementFace& f = element.faces[static_cast<std::size_t>(facing)];
    f.present = true;
    f.slot = slot;
    f.uv = uv;
    f.cull = cull;
}

// `cullface: "<that same side>"`, the shape every declaration in the models this
// file transcribes happens to take.
[[nodiscard]] inline std::uint8_t cullToward(Facing facing) {
    return static_cast<std::uint8_t>(facing);
}

inline FaceUv rect(float minU, float minV, float maxU, float maxV) {
    return FaceUv{minU, minV, maxU, maxV, false};
}

// The diode slab base (0,0,0)-(16,2,16): #top on the up face, #slab on the four
// sides, never a down face (it sits on its support). Transcribed from
// appendElementModel's emitDiodeBase.
inline ModelElement diodeBaseElement() {
    ModelElement e;
    e.from16 = {0.0F, 0.0F, 0.0F};
    e.to16 = {16.0F, 2.0F, 16.0F};
    putFace(e, Facing::Up, 1, rect(0, 0, 16, 16));
    for (const Facing side : {Facing::North, Facing::South, Facing::West, Facing::East}) {
        // repeater_*.json / comparator.json: each side face carries
        // `"cullface": "<that side>"`. (Their down face, which carries
        // `"cullface": "down"`, is not transcribed here at all — the slab sits on
        // its support, so the quad would never be visible; that omission predates
        // RN-8b and is left as it was.)
        putFace(e, side, 0, rect(0, 14, 16, 16), cullToward(side));
    }
    return e;
}

// A redstone-torch nub: up face plus four sides, no down (it stands on the slab).
// `slot` is the lit or unlit sprite. Transcribed from emitTorch.
inline ModelElement torchElement(const glm::vec3& from16, const glm::vec3& to16,
                                 std::uint8_t slot, float glow) {
    ModelElement e;
    e.from16 = from16;
    e.to16 = to16;
    e.glow = glow;
    putFace(e, Facing::Up, slot, rect(7, 6, 9, 8));
    for (const Facing side : {Facing::North, Facing::South, Facing::West, Facing::East}) {
        putFace(e, side, slot, rect(7, 6, 9, 11));
    }
    return e;
}

} // namespace detail

// The elements of a repeater, delay 1..4 and powered decide torch position/sprite
// (repeater_Ntick.json). Mirrors appendElementModel's repeater branch.
[[nodiscard]] inline std::vector<ModelElement> repeaterElements(BlockState state) {
    const std::uint8_t torchSlot = state.powered() ? 3U : 2U;
    const float glow = state.powered() ? 0.5F : 0.0F;
    std::vector<ModelElement> elements;
    elements.push_back(detail::diodeBaseElement());
    elements.push_back(detail::torchElement({7, 2, 2}, {9, 7, 4}, torchSlot, glow)); // fixed output
    const float movingZ = 6.0F + static_cast<float>(state.repeaterDelay() - 1) * 2.0F;
    elements.push_back(
        detail::torchElement({7, 2, movingZ}, {9, 7, movingZ + 2.0F}, torchSlot, glow));
    return elements;
}

// The elements of a comparator: two rear torches flank the input, the front torch
// rises in SUBTRACT mode. Mirrors appendElementModel's comparator branch.
[[nodiscard]] inline std::vector<ModelElement> comparatorElements(BlockState state) {
    const std::uint8_t torchSlot = state.powered() ? 3U : 2U;
    const float glow = state.powered() ? 0.5F : 0.0F;
    std::vector<ModelElement> elements;
    elements.push_back(detail::diodeBaseElement());
    elements.push_back(detail::torchElement({4, 2, 11}, {6, 7, 13}, torchSlot, glow));
    elements.push_back(detail::torchElement({10, 2, 11}, {12, 7, 13}, torchSlot, glow));
    const float frontTop = state.comparatorSubtract() ? 6.0F : 5.0F;
    elements.push_back(detail::torchElement({7, 2, 2}, {9, frontTop, 4}, torchSlot, glow));
    return elements;
}

// The elements of a lever: a cobblestone base (#base, slot 0) plus a handle
// (#lever, slot 1) tilted 45° about its bottom (powered flips the tilt). Mirrors
// appendElementModel's lever branch. The whole model is then attached to its
// FACING face by attachTransform.
[[nodiscard]] inline std::vector<ModelElement> leverElements(BlockState state) {
    std::vector<ModelElement> elements;

    ModelElement base;
    base.from16 = {5.0F, -0.02F, 4.0F};
    base.to16 = {11.0F, 2.98F, 12.0F};
    // lever.json: the base's down face carries `"cullface": "down"`, its up face
    // none. The cullface rides through attachTransform with the model, so a
    // ceiling lever culls against the block above it.
    detail::putFace(base, Facing::Down, 0, detail::rect(5, 4, 11, 12),
                    detail::cullToward(Facing::Down));
    detail::putFace(base, Facing::Up, 0, detail::rect(5, 4, 11, 12));
    for (const Facing f : {Facing::North, Facing::South, Facing::West, Facing::East}) {
        detail::putFace(base, f, 0, detail::rect(4, 0, 12, 3));
    }
    elements.push_back(base);

    ModelElement handle;
    handle.from16 = {7.0F, 1.0F, 7.0F};
    handle.to16 = {9.0F, 11.0F, 9.0F};
    handle.rotation.present = true;
    handle.rotation.origin = {8.0F, 1.0F, 8.0F};
    handle.rotation.axis = 'x';
    handle.rotation.angleDeg = state.powered() ? -45.0F : 45.0F;
    detail::putFace(handle, Facing::Up, 1, detail::rect(7, 6, 9, 8));
    for (const Facing f : {Facing::North, Facing::South, Facing::West, Facing::East}) {
        detail::putFace(handle, f, 1, detail::rect(7, 6, 9, 16));
    }
    elements.push_back(handle);

    return elements;
}

// The FACING yaw for a horizontally-attached diode (south is vanilla identity).
// Mirrors ChunkMesher::yawForHorizontalFacing.
[[nodiscard]] inline float diodeYaw(BlockOrientation facing) {
    switch (facing) {
    case BlockOrientation::East: return 90.0F;
    case BlockOrientation::North: return 180.0F;
    case BlockOrientation::West: return 270.0F;
    default: return 0.0F; // South
    }
}

// The whole-model attachment transform (JE ModelState): the diodes yaw about Y to
// their FACING; the lever tilts its floor-authored model onto whichever of the
// six faces its FACING attaches it to. Mirrors appendElementModel's postAxis/
// postDeg selection.
[[nodiscard]] inline ModelTransform attachTransform(Block block, BlockState state) {
    // ENCH-2: the table has no FACING at all, so it must not fall through to the
    // diode yaw below — which reads `state.orientation()` and would rotate the
    // model by whatever the default orientation happens to be. Visually the box
    // is 4-way symmetric so a yaw is invisible today, but "invisible today"
    // is exactly how a model bug survives until someone gives a face its own
    // sprite.
    if (block == Block::EnchantingTable) {
        return {axisMatrix('y', 0.0F)};
    }
    if (block == Block::Lever) {
        switch (state.orientation()) {
        case BlockOrientation::Up: return {axisMatrix('x', 0.0F)};      // floor
        case BlockOrientation::Down: return {axisMatrix('x', 180.0F)};  // ceiling
        case BlockOrientation::North: return {axisMatrix('x', -90.0F)};
        case BlockOrientation::South: return {axisMatrix('x', 90.0F)};
        case BlockOrientation::East: return {axisMatrix('z', -90.0F)};
        case BlockOrientation::West: return {axisMatrix('z', 90.0F)};
        }
    }
    return {axisMatrix('y', diodeYaw(state.orientation()))};
}

// ENCH-2: the enchanting table, transcribed verbatim from vanilla
// models/block/enchanting_table.json — ONE box (0,0,0)-(16,12,16) whose down
// face takes #bottom over the full 16x16 sprite, whose up face takes #top, and
// whose four sides take #side over the sprite's LOWER 12 rows (uv 0,4..16,16):
// the model is 12 units tall, so it samples the bottom three quarters of the
// side texture, not a stretched full sprite. Texture slots follow the block's
// .elementModel() order: 0 = top, 1 = side, 2 = bottom.
inline ModelElement enchantingTableElement() {
    ModelElement e;
    e.from16 = {0.0F, 0.0F, 0.0F};
    e.to16 = {16.0F, 12.0F, 16.0F};
    // enchanting_table.json declares `cullface` on the down face and all four
    // sides; the up face (the table is only 12 units tall) has none.
    detail::putFace(e, Facing::Down, 2, detail::rect(0, 0, 16, 16),
                    detail::cullToward(Facing::Down));
    detail::putFace(e, Facing::Up, 0, detail::rect(0, 0, 16, 16));
    for (const Facing side : {Facing::North, Facing::South, Facing::West, Facing::East}) {
        detail::putFace(e, side, 1, detail::rect(0, 4, 16, 16), detail::cullToward(side));
    }
    return e;
}

// ENCH-3: the anvil, transcribed from vanilla models/block/template_anvil.json —
// four stacked boxes (base, waist, neck, top plate) with the literal per-face uv
// rects the model carries. Slot 0 is #body (block/anvil), slot 1 is #top, which
// is what the three wear states differ in. The `west`/`east` faces whose uv runs
// backwards (e.g. [4,2,0,14]) are vanilla's own mirrored rects and are kept
// as-is: flipping them "to look right" is how a model stops matching vanilla.
inline std::vector<ModelElement> anvilElements() {
    std::vector<ModelElement> elements;
    {
        ModelElement base;
        base.from16 = {2.0F, 0.0F, 2.0F};
        base.to16 = {14.0F, 4.0F, 14.0F};
        // template_anvil.json: the base's down face is the model's only
        // `"cullface"` declaration.
        detail::putFace(base, Facing::Down, 0, detail::rect(2, 2, 14, 14),
                        detail::cullToward(Facing::Down));
        detail::putFace(base, Facing::Up, 0, detail::rect(2, 2, 14, 14));
        detail::putFace(base, Facing::North, 0, detail::rect(2, 12, 14, 16));
        detail::putFace(base, Facing::South, 0, detail::rect(2, 12, 14, 16));
        detail::putFace(base, Facing::West, 0, detail::rect(0, 2, 4, 14));
        detail::putFace(base, Facing::East, 0, detail::rect(4, 2, 0, 14));
        elements.push_back(base);
    }
    {
        ModelElement waist;
        waist.from16 = {4.0F, 4.0F, 3.0F};
        waist.to16 = {12.0F, 5.0F, 13.0F};
        detail::putFace(waist, Facing::Up, 0, detail::rect(4, 3, 12, 13));
        detail::putFace(waist, Facing::North, 0, detail::rect(4, 11, 12, 12));
        detail::putFace(waist, Facing::South, 0, detail::rect(4, 11, 12, 12));
        detail::putFace(waist, Facing::West, 0, detail::rect(4, 3, 5, 13));
        detail::putFace(waist, Facing::East, 0, detail::rect(5, 3, 4, 13));
        elements.push_back(waist);
    }
    {
        ModelElement neck;
        neck.from16 = {6.0F, 5.0F, 4.0F};
        neck.to16 = {10.0F, 10.0F, 12.0F};
        detail::putFace(neck, Facing::North, 0, detail::rect(6, 6, 10, 11));
        detail::putFace(neck, Facing::South, 0, detail::rect(6, 6, 10, 11));
        detail::putFace(neck, Facing::West, 0, detail::rect(5, 4, 10, 12));
        detail::putFace(neck, Facing::East, 0, detail::rect(10, 4, 5, 12));
        elements.push_back(neck);
    }
    {
        ModelElement top;
        top.from16 = {3.0F, 10.0F, 0.0F};
        top.to16 = {13.0F, 16.0F, 16.0F};
        detail::putFace(top, Facing::Down, 0, detail::rect(3, 0, 13, 16));
        detail::putFace(top, Facing::Up, 1, detail::rect(3, 0, 13, 16));
        detail::putFace(top, Facing::North, 0, detail::rect(3, 0, 13, 6));
        detail::putFace(top, Facing::South, 0, detail::rect(3, 0, 13, 6));
        detail::putFace(top, Facing::West, 0, detail::rect(10, 0, 16, 16));
        detail::putFace(top, Facing::East, 0, detail::rect(16, 0, 10, 16));
        elements.push_back(top);
    }
    return elements;
}

[[nodiscard]] constexpr bool isAnvil(Block block) {
    return block == Block::Anvil || block == Block::ChippedAnvil ||
           block == Block::DamagedAnvil;
}

// The elements of an ElementModel block by kind (empty for a block that is not
// one of the transcribed models).
[[nodiscard]] inline std::vector<ModelElement> elementsFor(Block block, BlockState state) {
    switch (block) {
    case Block::Repeater: return repeaterElements(state);
    case Block::Comparator: return comparatorElements(state);
    case Block::Lever: return leverElements(state);
    case Block::EnchantingTable: return {enchantingTableElement()};
    case Block::Anvil:
    case Block::ChippedAnvil:
    case Block::DamagedAnvil: return anvilElements();
    default: return {};
    }
}

// Bake an ElementModel block to its quads: every present face of every element,
// through the N1 primitive, with the block's attachment transform applied. Quad
// order is elements in build order, faces in Facing order (Down..East), so a
// consumer/test can pair quads by index with the element description.
[[nodiscard]] inline std::vector<BakedElementQuad> bakeElementModel(Block block, BlockState state) {
    const std::vector<ModelElement> elements = elementsFor(block, state);
    const ModelTransform attach = attachTransform(block, state);
    std::vector<BakedElementQuad> quads;
    for (const ModelElement& element : elements) {
        for (std::uint8_t f = 0; f < kFacingCount; ++f) {
            const ElementFace& face = element.faces[f];
            if (!face.present) {
                continue;
            }
            quads.push_back({bakeElementFace(element.from16, element.to16, static_cast<Facing>(f),
                                             face, element.rotation, attach),
                             element.glow});
        }
    }
    return quads;
}

} // namespace mc::world::bake

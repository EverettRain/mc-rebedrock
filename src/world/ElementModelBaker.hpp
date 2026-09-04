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
// AO and slot->atlas-layer resolution remain the mesher's job (N2b), so this
// header carries no render dependency and is unit-testable in isolation.

#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/FaceBakery.hpp"

#include <array>
#include <cstdint>
#include <span>
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
    // RN-10a: the model json's per-element `"shade"`. False means the element
    // does not take the directional light falloff a face's normal would give it —
    // JE's own use is a lit redstone torch's glow halo, whose six billboards
    // would otherwise be four different brightnesses. The baker carries it, the
    // mesher hands it to the shader (RN-13).
    //
    // RN-13 removed the sibling `glow` field this used to sit beside — a float
    // the mesher folded into the cell's block light for a lit diode torch. It had
    // no vanilla counterpart: neither Blocks.REPEATER (Blocks.java:2089) nor
    // Blocks.COMPARATOR (:2762) declares a `lightLevel`, and their models declare
    // no `light_emission` either (26.1 does have per-element `light_emission`,
    // CuboidModelElement.java:36-72, but in the whole vanilla model set only
    // cross_emissive/flower_pot_cross_emissive use it, and this roster has
    // neither). Two mechanisms for "this element is bright" would have coexisted;
    // the one vanilla actually uses here is `shade`.
    bool shade = true;
};

// A baked quad plus the `shade` flag its element carries. The baker returns
// these; the mesher resolves slot->layer and passes shade to the vertex.
struct BakedElementQuad final {
    BakedQuad quad{};
    bool shade = true; // RN-10a, from the element
};

namespace detail {

// Set one face of an element by facing. `cull` is the model json's `cullface`
// declaration (RN-8b) — the direction whose neighbour, if it seals that whole
// cell wall, hides this quad. Faces vanilla leaves undeclared stay kNoCull and
// are drawn unconditionally, which is JE's getQuads(null) half of the split.
inline void putFace(ModelElement& element, Facing facing, std::uint8_t slot, const FaceUv& uv,
                    std::uint8_t cull = kNoCull, std::uint8_t quadrant = 0) {
    ElementFace& f = element.faces[static_cast<std::size_t>(facing)];
    f.present = true;
    f.slot = slot;
    f.uv = uv;
    f.cull = cull;
    f.quadrant = quadrant;
}

// `cullface: "<that same side>"`, the shape every declaration in the models this
// file transcribes happens to take.
[[nodiscard]] inline std::uint8_t cullToward(Facing facing) {
    return static_cast<std::uint8_t>(facing);
}

} // namespace detail

namespace detail {

inline FaceUv rect(float minU, float minV, float maxU, float maxV) {
    return FaceUv{minU, minV, maxU, maxV, false};
}

// The diode slab base (0,0,0)-(16,2,16): #top on the up face, #slab on the four
// sides, never a down face (it sits on its support). Transcribed from
// appendElementModel's emitDiodeBase.
inline ModelElement diodeBaseElement(bool lit = false) {
    ModelElement e;
    e.from16 = {0.0F, 0.0F, 0.0F};
    e.to16 = {16.0F, 2.0F, 16.0F};
    // RN-13-1: `#top` is the ONLY texture binding vanilla's `_on` diode models
    // change (repeater_1tick_on.json / comparator_on.json both swap it and
    // nothing else), and it is what turns the red line between the torches from
    // dark to bright. This build showed the unlit plate in every state, which is
    // a large part of "a powered repeater looks no different".
    putFace(e, Facing::Up, lit ? 5 : 1, rect(0, 0, 16, 16));
    // RN-10d / audit R15: the down face, `[0,0,16,16]` with `"cullface": "down"`.
    // It used to be left out on the grounds that a diode sits on its support so
    // the quad is never seen — but that is what the cullface declaration is for,
    // and it is only true while the support is opaque. A repeater on a slab, a
    // fence, a hopper or glass showed the world through its own base.
    putFace(e, Facing::Down, 0, rect(0, 0, 16, 16), cullToward(Facing::Down));
    for (const Facing side : {Facing::North, Facing::South, Facing::West, Facing::East}) {
        // repeater_*.json / comparator.json: each side face carries
        // `"cullface": "<that side>"`.
        putFace(e, side, 0, rect(0, 14, 16, 16), cullToward(side));
    }
    return e;
}

// A redstone-torch nub: up face plus four sides, no down (it stands on the slab).
// `slot` is the lit or unlit sprite. Transcribed from emitTorch.
inline ModelElement torchElement(const glm::vec3& from16, const glm::vec3& to16,
                                 std::uint8_t slot, bool shade = true) {
    ModelElement e;
    e.from16 = from16;
    e.to16 = to16;
    // RN-10d: vanilla marks a *lit* torch element `"shade": false` (see any
    // repeater_*_on.json), and leaves an unlit one shaded.
    e.shade = shade;
    putFace(e, Facing::Up, slot, rect(7, 6, 9, 8));
    // RN-8d: vanilla gives each nub a side rect that starts at v=6 and runs as
    // many rows as the box is tall — the 5-tall repeater torches take [7,6,9,11]
    // (repeater_1tick.json), the 3-tall comparator front torch [7,6,9,9]
    // (comparator.json). This used to be a fixed 11 for every nub, which
    // stretched five rows of the sprite over the short one.
    const float sideBottomV = 6.0F + (to16.y - from16.y);
    for (const Facing side : {Facing::North, Facing::South, Facing::West, Facing::East}) {
        putFace(e, side, slot, rect(7, 6, 9, sideBottomV));
    }
    return e;
}

// RN-10d: the six billboards vanilla hangs around a LIT redstone torch — the
// glow this build had none of at all (audit R13). Each is a 3x3x3 box offset
// three pixels off the torch along one axis, showing only the single face that
// points back at the torch, textured with one pixel of the lit sprite
// (`uv [6,5,7,6]`), and marked `"shade": false`.
//
// The BOX rule is regular across all four comparator models and all eight lit
// repeater ones, so it is written once rather than as ninety transcribed boxes:
// centre the core box on the torch's own x/z and on `to.y - 1`, then push a copy
// three pixels along each of the six axes and keep the one face pointing back.
//
// The RECTS are not regular and are passed in. The comparator's six halos all
// take `[6,5,7,6]`; the repeater's take six *different* single-pixel rects (up
// `[8,5,9,6]`, down `[7,5,8,6]`, ...). Assuming one rule for both is the mistake
// this comment exists to stop — a halo is one pixel of the torch sprite, and
// vanilla picks a different pixel per direction on the repeater.
//
// The element order (up, down, south, north, east, west) is vanilla's own within
// each torch's group. Order is not observable in a mesh; it is kept so a reader
// can diff this against the json line by line.
using HaloRects = std::array<FaceUv, kFacingCount>; // indexed by Facing

inline void appendTorchHalo(std::vector<ModelElement>& out, const glm::vec3& from16,
                            const glm::vec3& to16, std::uint8_t litSlot,
                            const HaloRects& rects) {
    const glm::vec3 centre{(from16.x + to16.x) * 0.5F, to16.y - 1.0F,
                           (from16.z + to16.z) * 0.5F};
    constexpr float kHalfExtent = 1.5F;
    constexpr float kOffset = 3.0F;
    for (const Facing facing : {Facing::Up, Facing::Down, Facing::South, Facing::North,
                                Facing::East, Facing::West}) {
        const glm::vec3 shift = facingUnit(facing) * -kOffset;
        ModelElement e;
        e.from16 = centre - glm::vec3{kHalfExtent} + shift;
        e.to16 = centre + glm::vec3{kHalfExtent} + shift;
        e.shade = false;
        putFace(e, facing, litSlot, rects[static_cast<std::size_t>(facing)]);
        out.push_back(e);
    }
}

// repeater_*tick_on.json, in Facing order (Down, Up, North, South, West, East).
inline HaloRects repeaterHaloRects() {
    return {rect(7, 5, 8, 6),  rect(8, 5, 9, 6), rect(6, 6, 7, 7),
            rect(9, 6, 10, 7), rect(6, 7, 7, 8), rect(9, 7, 10, 8)};
}

// comparator_on*.json: one rect for all six.
inline HaloRects comparatorHaloRects() {
    HaloRects rects{};
    rects.fill(rect(6, 5, 7, 6));
    return rects;
}

} // namespace detail

// The elements of a repeater, delay 1..4 and powered decide torch position/sprite
// (repeater_Ntick.json). Mirrors appendElementModel's repeater branch.
// RN-10e: the bar a LOCKED repeater shows in place of its delay torch
// (repeater_*tick_locked.json). It sits exactly where that torch would have been
// — `[2,2,z]-[14,4,z+2]` for the same z the torch takes — and vanilla skins it
// with `#lock` = block/bedrock, which is slot 4. Five faces; no down.
//
// Note what the locked models do NOT contain: the moving torch. Locked is not
// "the same model plus a bar", it is "the bar instead of the torch", so a locked
// repeater has three elements where an unlocked one has three of its own.
[[nodiscard]] inline ModelElement repeaterLockElement(float z) {
    ModelElement e;
    e.from16 = {2.0F, 2.0F, z};
    e.to16 = {14.0F, 4.0F, z + 2.0F};
    detail::putFace(e, Facing::Up, 4, detail::rect(7, 2, 9, 14), kNoCull, kQuadrant90);
    detail::putFace(e, Facing::North, 4, detail::rect(2, 7, 14, 9));
    detail::putFace(e, Facing::South, 4, detail::rect(2, 7, 14, 9));
    detail::putFace(e, Facing::West, 4, detail::rect(6, 7, 8, 9));
    detail::putFace(e, Facing::East, 4, detail::rect(6, 7, 8, 9));
    return e;
}

[[nodiscard]] inline std::vector<ModelElement> repeaterElements(BlockState state) {
    // Both of a repeater's torches follow POWERED together (repeater_Ntick_on.json
    // marks both `#on` and `"shade": false`), so unlike the comparator below there
    // is one switch here, not two.
    const bool lit = state.powered();
    const std::uint8_t torchSlot = lit ? 3U : 2U;
    const glm::vec3 fixedFrom{7.0F, 2.0F, 2.0F};
    const glm::vec3 fixedTo{9.0F, 7.0F, 4.0F};
    const float movingZ = 6.0F + static_cast<float>(state.repeaterDelay() - 1) * 2.0F;
    const glm::vec3 movingFrom{7.0F, 2.0F, movingZ};
    const glm::vec3 movingTo{9.0F, 7.0F, movingZ + 2.0F};

    // RN-10e: LOCKED is read off the STATE, never derived here. It has to be:
    // the mesher has no neighbour-state channel at all (it sees a `Block`, not a
    // `BlockState`, for the cells around the one it is drawing), so a locked bar
    // that asked "is a diode beside me powered" would need one opened for it —
    // a runtime graph walk on the remesh path, per repeater. AR-B4-2c put LOCKED
    // in the state and AR-B4-4 writes it; this reads the bit. The bar is checked
    // against that in element_model_baker_test.
    const bool locked = state.repeaterLocked();

    std::vector<ModelElement> elements;
    elements.push_back(detail::diodeBaseElement(lit));
    if (locked) {
        elements.push_back(repeaterLockElement(movingZ));
    }
    elements.push_back(detail::torchElement(fixedFrom, fixedTo, torchSlot, !lit));
    if (!locked) {
        elements.push_back(detail::torchElement(movingFrom, movingTo, torchSlot, !lit));
    }
    if (lit) {
        const detail::HaloRects rects = detail::repeaterHaloRects();
        detail::appendTorchHalo(elements, fixedFrom, fixedTo, torchSlot, rects);
        if (!locked) {
            detail::appendTorchHalo(elements, movingFrom, movingTo, torchSlot, rects);
        }
    }
    return elements;
}

// The elements of a comparator. RN-10d / audit R11+R12: a comparator has TWO
// independent lamps, and this build had one.
//
// Read off the four models (comparator / _on / _subtract / _on_subtract):
//
//   variant                  rear pair (z 11..13)   front torch (z 2..4)
//   compare,  unpowered      unlit                  unlit
//   compare,  powered        LIT                    unlit
//   subtract, unpowered      unlit                  LIT
//   subtract, powered        LIT                    LIT
//
// i.e. the rear pair is POWERED and the front one is MODE. Driving all three off
// POWERED — which is what happened here — meant right-clicking a comparator
// changed nothing a player could see, because the only other difference was a
// 1-pixel height this build invented: the front torch's box is `[7,2,2]` to
// `[9,5,4]` in all four models, subtract included. Its geometry never moves; the
// sprite under it does.
[[nodiscard]] inline std::vector<ModelElement> comparatorElements(BlockState state) {
    const bool rearLit = state.powered();
    const bool frontLit = state.comparatorSubtract();
    const auto slotOf = [](bool lit) { return lit ? 3U : 2U; };

    const glm::vec3 rearLeftFrom{4.0F, 2.0F, 11.0F};
    const glm::vec3 rearLeftTo{6.0F, 7.0F, 13.0F};
    const glm::vec3 rearRightFrom{10.0F, 2.0F, 11.0F};
    const glm::vec3 rearRightTo{12.0F, 7.0F, 13.0F};
    const glm::vec3 frontFrom{7.0F, 2.0F, 2.0F};
    const glm::vec3 frontTo{9.0F, 5.0F, 4.0F};

    std::vector<ModelElement> elements;
    // The top plate follows POWERED alone: comparator_subtract.json binds the
    // unlit `#top` and comparator_on_subtract.json the lit one, so MODE never
    // touches it (only the front torch, below).
    elements.push_back(detail::diodeBaseElement(rearLit));
    elements.push_back(detail::torchElement(rearLeftFrom, rearLeftTo,
                                            static_cast<std::uint8_t>(slotOf(rearLit)), !rearLit));
    elements.push_back(detail::torchElement(rearRightFrom, rearRightTo,
                                            static_cast<std::uint8_t>(slotOf(rearLit)), !rearLit));
    elements.push_back(detail::torchElement(frontFrom, frontTo,
                                            static_cast<std::uint8_t>(slotOf(frontLit)),
                                            !frontLit));
    const detail::HaloRects rects = detail::comparatorHaloRects();
    if (rearLit) {
        detail::appendTorchHalo(elements, rearLeftFrom, rearLeftTo, 3U, rects);
        detail::appendTorchHalo(elements, rearRightFrom, rearRightTo, 3U, rects);
    }
    if (frontLit) {
        detail::appendTorchHalo(elements, frontFrom, frontTo, 3U, rects);
    }
    return elements;
}

// --- RN-10a: door / trapdoor / fence gate, transcribed rect for rect ---------
//
// These three are still meshed by ChunkMesher's appendBoxes/appendFenceGate;
// RN-10a only brings their model *descriptions* in, so they can be locked
// against the json before anything renders from them (RN-10b/10c wire them).
// Every `uv` below is the literal rect from the vanilla model, including the
// ones that run backwards — `[3,0,0,16]`, `[0,16,16,13]` and friends are
// vanilla's own mirrored/flipped rects, and "fixing" them is precisely how a
// model stops matching vanilla. The projected `defaultFaceUV` these blocks get
// today disagrees with most of them (audit R1/R2/R4/R6/R8).

// A door leaf: one box, [0,0,0]-[3,16,16], its identity variant standing against
// the cell's west wall — which is `facing=east` in the blockstate (oak_door.json
// gives facing=east,hinge=left,open=false no `"y"`), and `rotatedBy(
// kDoorClosedBox, East)` in BlockShape's terms. Slot 0 is the half's sprite:
// vanilla splits it as two models over `#bottom`/`#top` rather than as two faces
// of one, so the mesher picks the layer per HALF, not per face.
//
// Sources: door_bottom_{left,right}{,_open}.json, door_top_{left,right}{,_open}.json.
[[nodiscard]] inline ModelElement doorElement(bool upperHalf, bool rightHinge, bool open) {
    ModelElement e;
    e.from16 = {0.0F, 0.0F, 0.0F};
    e.to16 = {3.0F, 16.0F, 16.0F};

    // The horizontal four. Both halves carry the same rects for a given
    // (hinge, open) — the two models differ only in their cap face and their
    // texture — so they are written once here and the cap is added below.
    const FaceUv narrowForward = detail::rect(3, 0, 0, 16);  // [3,0,0,16]
    const FaceUv narrowBackward = detail::rect(0, 0, 3, 16); // [0,0,3,16]
    const FaceUv wideForward = detail::rect(0, 0, 16, 16);   // [0,0,16,16]
    const FaceUv wideBackward = detail::rect(16, 0, 0, 16);  // [16,0,0,16]

    // north/south: closed is (narrowForward, narrowBackward) for both hinges;
    // open mirrors one of the two — left-open takes narrowForward on *south*
    // too, right-open takes narrowBackward's mirror on both. Read straight off
    // the four json files rather than derived.
    FaceUv north = narrowForward;
    FaceUv south = narrowBackward;
    if (open) {
        north = rightHinge ? narrowForward : narrowBackward;
        south = rightHinge ? narrowForward : narrowBackward;
    }
    // west/east: this is R1 — the hinge mirror. A right-hinged door reverses the
    // U of its two wide faces against a left-hinged one, which is what puts the
    // handle on the correct side; sharing one projected rect (what happens today)
    // draws both hinges identically and is most obvious on a double door.
    const bool wideReversed = rightHinge != open;
    const FaceUv west = wideReversed ? wideBackward : wideForward;
    const FaceUv east = wideReversed ? wideForward : wideBackward;

    detail::putFace(e, Facing::North, 0, north, detail::cullToward(Facing::North));
    detail::putFace(e, Facing::South, 0, south, detail::cullToward(Facing::South));
    detail::putFace(e, Facing::West, 0, west, detail::cullToward(Facing::West));
    // The east face is the only one of the six with no `cullface` in any of the
    // eight models: it is the leaf's inward face, never flush with a cell wall.
    detail::putFace(e, Facing::East, 0, east);

    // The cap. The lower half caps down, the upper half up, and each carries a
    // `"rotation"` — R5, which the appendBoxes path cannot express at all.
    if (upperHalf) {
        const FaceUv up = rightHinge ? detail::rect(0, 0, 16, 3) : detail::rect(0, 3, 16, 0);
        const std::uint8_t quadrant = (rightHinge != open) ? kQuadrant270 : kQuadrant90;
        detail::putFace(e, Facing::Up, 0, up, detail::cullToward(Facing::Up), quadrant);
    } else {
        FaceUv down = detail::rect(16, 13, 0, 16); // left, closed
        if (rightHinge && !open) {
            down = detail::rect(0, 13, 16, 16);
        } else if (!rightHinge && open) {
            down = detail::rect(0, 16, 16, 13);
        } else if (rightHinge && open) {
            down = detail::rect(16, 16, 0, 13);
        }
        detail::putFace(e, Facing::Down, 0, down, detail::cullToward(Facing::Down), kQuadrant90);
    }
    return e;
}

// The swing side a door leaf actually stands on — DoorBlock#getShape's
// `doorDirection`, the same derivation kDoorBoxTable uses (right hinge turns
// counter-clockwise, left hinge clockwise). Both the shape and the model rotate
// by this, which is why the two agree box for box.
[[nodiscard]] constexpr BlockOrientation doorSwingOrientation(BlockOrientation facing, bool open,
                                                              bool rightHinge) {
    if (!open) {
        return facing;
    }
    return rightHinge ? counterClockwiseOrientation(facing) : clockwiseOrientation(facing);
}

[[nodiscard]] inline std::vector<ModelElement> doorElements(BlockState state) {
    return {doorElement(state.isDoorUpperHalf(), state.hinge() == DoorHinge::Right, state.open())};
}

// A trapdoor leaf: one box, from template_trapdoor_{bottom,top,open}.json.
// Closed it lies flat against the floor or ceiling and its model carries no
// rotation at all (every `open=false` row of oak_trapdoor.json has no `"y"`);
// open it stands against the cell's south wall — the identity `facing=north`
// variant — which is the same `kDoorClosedBox` the door's shape is built from.
//
// The side rects are `[0,16,16,13]`: V runs *backwards*, which is audit R6. The
// projected UV this block gets today is `[0,13,16,16]`, i.e. the plank grain
// upside down on all four sides and on the open leaf's cap.
[[nodiscard]] inline ModelElement trapdoorElement(bool topHalf, bool open) {
    ModelElement e;
    const FaceUv flat = detail::rect(0, 0, 16, 16);
    if (open) {
        e.from16 = {0.0F, 0.0F, 13.0F};
        e.to16 = {16.0F, 16.0F, 16.0F};
        detail::putFace(e, Facing::Down, 0, detail::rect(0, 13, 16, 16),
                        detail::cullToward(Facing::Down));
        detail::putFace(e, Facing::Up, 0, detail::rect(0, 16, 16, 13),
                        detail::cullToward(Facing::Up));
        detail::putFace(e, Facing::North, 0, flat); // the inward face: no cullface
        detail::putFace(e, Facing::South, 0, flat, detail::cullToward(Facing::South));
        detail::putFace(e, Facing::West, 0, detail::rect(16, 0, 13, 16),
                        detail::cullToward(Facing::West));
        detail::putFace(e, Facing::East, 0, detail::rect(13, 0, 16, 16),
                        detail::cullToward(Facing::East));
        return e;
    }
    const FaceUv side = detail::rect(0, 16, 16, 13);
    if (topHalf) {
        e.from16 = {0.0F, 13.0F, 0.0F};
        e.to16 = {16.0F, 16.0F, 16.0F};
        detail::putFace(e, Facing::Down, 0, flat); // template_trapdoor_top: no cullface
        detail::putFace(e, Facing::Up, 0, flat, detail::cullToward(Facing::Up));
    } else {
        e.from16 = {0.0F, 0.0F, 0.0F};
        e.to16 = {16.0F, 3.0F, 16.0F};
        detail::putFace(e, Facing::Down, 0, flat, detail::cullToward(Facing::Down));
        detail::putFace(e, Facing::Up, 0, flat); // template_trapdoor_bottom: no cullface
    }
    for (const Facing f : {Facing::North, Facing::South, Facing::West, Facing::East}) {
        detail::putFace(e, f, 0, side, detail::cullToward(f));
    }
    return e;
}

[[nodiscard]] inline std::vector<ModelElement> trapdoorElements(BlockState state) {
    return {trapdoorElement(state.isDoorUpperHalf(), state.open())};
}

// A fence gate: eight boxes from template_fence_gate{,_open,_wall,_wall_open}.json.
//
// Three things here that the hand-written appendFenceGate does not do:
//  * the four horizontal bars declare FOUR faces, not six (audit R7). Closed they
//    are down/up/north/south; open they are down/up/west/east, because the whole
//    leaf has swung a quarter turn. The two faces vanilla omits are the ones
//    buried inside the neighbouring post — the bar's east face at x=6 and the
//    inner post's west face at x=6 are the same plane, drawn twice today.
//  * every rect is explicit (audit R8). The left post's north face is `[0,0,2,11]`;
//    the projection this block uses today gives `[14,0,16,11]`, mirrored.
//  * `in_wall` is the same model lowered three pixels (posts 5..16 -> 2..13, inner
//    posts 6..15 -> 3..12, bars 6..9 -> 3..6 and 12..15 -> 9..12) with every uv
//    byte unchanged — that is literally what the _wall jsons are, and the golden
//    test transcribes their boxes independently to keep this shortcut honest.
[[nodiscard]] inline std::vector<ModelElement> fenceGateElements(bool open, bool inWall) {
    const float drop = inWall ? 3.0F : 0.0F;
    std::vector<ModelElement> elements;
    const auto box = [&](float x0, float y0, float z0, float x1, float y1, float z1) {
        ModelElement e;
        e.from16 = {x0, y0 - drop, z0};
        e.to16 = {x1, y1 - drop, z1};
        return e;
    };

    // Left-hand post and right-hand post: identical in both models, and the only
    // two elements with a `cullface` (west on the left post, east on the right).
    {
        ModelElement e = box(0, 5, 7, 2, 16, 9);
        detail::putFace(e, Facing::Down, 0, detail::rect(0, 7, 2, 9));
        detail::putFace(e, Facing::Up, 0, detail::rect(0, 7, 2, 9));
        detail::putFace(e, Facing::North, 0, detail::rect(0, 0, 2, 11));
        detail::putFace(e, Facing::South, 0, detail::rect(0, 0, 2, 11));
        detail::putFace(e, Facing::West, 0, detail::rect(7, 0, 9, 11),
                        detail::cullToward(Facing::West));
        detail::putFace(e, Facing::East, 0, detail::rect(7, 0, 9, 11));
        elements.push_back(e);
    }
    {
        ModelElement e = box(14, 5, 7, 16, 16, 9);
        detail::putFace(e, Facing::Down, 0, detail::rect(14, 7, 16, 9));
        detail::putFace(e, Facing::Up, 0, detail::rect(14, 7, 16, 9));
        detail::putFace(e, Facing::North, 0, detail::rect(14, 0, 16, 11));
        detail::putFace(e, Facing::South, 0, detail::rect(14, 0, 16, 11));
        detail::putFace(e, Facing::West, 0, detail::rect(7, 0, 9, 11));
        detail::putFace(e, Facing::East, 0, detail::rect(7, 0, 9, 11),
                        detail::cullToward(Facing::East));
        elements.push_back(e);
    }

    if (!open) {
        // Inner vertical posts of the two leaves, meeting in the middle.
        {
            ModelElement e = box(6, 6, 7, 8, 15, 9);
            detail::putFace(e, Facing::Down, 0, detail::rect(6, 7, 8, 9));
            detail::putFace(e, Facing::Up, 0, detail::rect(6, 7, 8, 9));
            detail::putFace(e, Facing::North, 0, detail::rect(6, 1, 8, 10));
            detail::putFace(e, Facing::South, 0, detail::rect(6, 1, 8, 10));
            detail::putFace(e, Facing::West, 0, detail::rect(7, 1, 9, 10));
            detail::putFace(e, Facing::East, 0, detail::rect(7, 1, 9, 10));
            elements.push_back(e);
        }
        {
            ModelElement e = box(8, 6, 7, 10, 15, 9);
            detail::putFace(e, Facing::Down, 0, detail::rect(8, 7, 10, 9));
            detail::putFace(e, Facing::Up, 0, detail::rect(8, 7, 10, 9));
            detail::putFace(e, Facing::North, 0, detail::rect(8, 1, 10, 10));
            detail::putFace(e, Facing::South, 0, detail::rect(8, 1, 10, 10));
            detail::putFace(e, Facing::West, 0, detail::rect(7, 1, 9, 10));
            detail::putFace(e, Facing::East, 0, detail::rect(7, 1, 9, 10));
            elements.push_back(e);
        }
        // The four horizontal bars: down/up/north/south only.
        struct Bar final {
            float x0, y0, x1, y1;
            FaceUv cap;
            FaceUv side;
        };
        const std::array<Bar, 4> bars{{
            {2, 6, 6, 9, detail::rect(2, 7, 6, 9), detail::rect(2, 7, 6, 10)},
            {2, 12, 6, 15, detail::rect(2, 7, 6, 9), detail::rect(2, 1, 6, 4)},
            {10, 6, 14, 9, detail::rect(10, 7, 14, 9), detail::rect(10, 7, 14, 10)},
            {10, 12, 14, 15, detail::rect(10, 7, 14, 9), detail::rect(10, 1, 14, 4)},
        }};
        for (const Bar& bar : bars) {
            ModelElement e = box(bar.x0, bar.y0, 7, bar.x1, bar.y1, 9);
            detail::putFace(e, Facing::Down, 0, bar.cap);
            detail::putFace(e, Facing::Up, 0, bar.cap);
            detail::putFace(e, Facing::North, 0, bar.side);
            detail::putFace(e, Facing::South, 0, bar.side);
            elements.push_back(e);
        }
        return elements;
    }

    // Open: the two leaves have swung back against the cell's north-east and
    // north-west corners, so the inner posts move to z 13..15 and the bars turn a
    // quarter turn — their two drawn side faces are now west/east.
    {
        ModelElement e = box(0, 6, 13, 2, 15, 15);
        detail::putFace(e, Facing::Down, 0, detail::rect(0, 13, 2, 15));
        detail::putFace(e, Facing::Up, 0, detail::rect(0, 13, 2, 15));
        detail::putFace(e, Facing::North, 0, detail::rect(0, 1, 2, 10));
        detail::putFace(e, Facing::South, 0, detail::rect(0, 1, 2, 10));
        detail::putFace(e, Facing::West, 0, detail::rect(13, 1, 15, 10));
        detail::putFace(e, Facing::East, 0, detail::rect(13, 1, 15, 10));
        elements.push_back(e);
    }
    {
        ModelElement e = box(14, 6, 13, 16, 15, 15);
        detail::putFace(e, Facing::Down, 0, detail::rect(14, 13, 16, 15));
        detail::putFace(e, Facing::Up, 0, detail::rect(14, 13, 16, 15));
        detail::putFace(e, Facing::North, 0, detail::rect(14, 1, 16, 10));
        detail::putFace(e, Facing::South, 0, detail::rect(14, 1, 16, 10));
        detail::putFace(e, Facing::West, 0, detail::rect(13, 1, 15, 10));
        detail::putFace(e, Facing::East, 0, detail::rect(13, 1, 15, 10));
        elements.push_back(e);
    }
    struct OpenBar final {
        float x0, y0, z0, x1, y1, z1;
        FaceUv cap;
        FaceUv side;
    };
    const std::array<OpenBar, 4> bars{{
        {0, 6, 9, 2, 9, 13, detail::rect(0, 9, 2, 13), detail::rect(13, 7, 15, 10)},
        {0, 12, 9, 2, 15, 13, detail::rect(0, 9, 2, 13), detail::rect(13, 1, 15, 4)},
        {14, 6, 9, 16, 9, 13, detail::rect(14, 9, 16, 13), detail::rect(13, 7, 15, 10)},
        {14, 12, 9, 16, 15, 13, detail::rect(14, 9, 16, 13), detail::rect(13, 1, 15, 4)},
    }};
    for (const OpenBar& bar : bars) {
        ModelElement e = box(bar.x0, bar.y0, bar.z0, bar.x1, bar.y1, bar.z1);
        detail::putFace(e, Facing::Down, 0, bar.cap);
        detail::putFace(e, Facing::Up, 0, bar.cap);
        detail::putFace(e, Facing::West, 0, bar.side);
        detail::putFace(e, Facing::East, 0, bar.side);
        elements.push_back(e);
    }
    return elements;
}

[[nodiscard]] inline std::vector<ModelElement> fenceGateElements(BlockState state) {
    return fenceGateElements(state.open(), state.inWall());
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
    // RN-8d: lever.json's four side rects are not all the same — north/south take
    // [5,0,11,3] (the base is 6 wide on X) and west/east [4,0,12,3] (8 deep on Z).
    // Transcribing one rect for all four stretched the narrow pair.
    for (const Facing f : {Facing::North, Facing::South}) {
        detail::putFace(base, f, 0, detail::rect(5, 0, 11, 3));
    }
    for (const Facing f : {Facing::West, Facing::East}) {
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

// --- RN-10a: the yaw convention, stated once ---------------------------------
//
// Two rotation conventions meet in this file and they run opposite ways, which
// is the single easiest thing here to get backwards:
//
//   * a vanilla blockstate's `"y"` turns the model CLOCKWISE seen from above;
//   * `axisMatrix('y', d)` — and ChunkMesher's rotateAxis, which it matches
//     term for term — turns COUNTER-clockwise (at +90 it takes +Z to +X, i.e.
//     south to east).
//
// So `engine yaw = 360 - vanilla y`, and every number below is in engine
// degrees. `yawFromModelBase` says it structurally instead: a model authored
// with its identity variant at `base` is carried onto `target`.
//
// This is not taken on faith. BlockShape's `rotatedBy` is the same quarter-turn
// family (`rotatedClockwise` is (x,z) -> (1-z,x)), so a baked model's bounding
// box has to equal the block's BlockShape box state for state — and for the door
// and the trapdoor it does, exactly, because their model box *is* their shape
// box. shaped_block_model_test asserts that for every variant, which pins this
// table without needing a screenshot. (The fence gate cannot be pinned that way:
// its model is a lattice inside the shape box and is 180-degree symmetric when
// closed, so only an open gate can show a wrong half-turn. See the note on
// `fenceGateYaw`.)
[[nodiscard]] constexpr std::size_t horizontalQuarterTurns(BlockOrientation facing) {
    switch (facing) {
    case BlockOrientation::North: return 0;
    case BlockOrientation::East: return 1;
    case BlockOrientation::South: return 2;
    case BlockOrientation::West: return 3;
    default: return 2; // the vertical pair, folded onto South as diodeYaw does
    }
}

[[nodiscard]] constexpr float yawFromModelBase(BlockOrientation base, BlockOrientation target) {
    const std::size_t turns =
        (horizontalQuarterTurns(target) + 4U - horizontalQuarterTurns(base)) % 4U;
    // clockwise^turns == engine yaw (4 - turns) * 90.
    return static_cast<float>((4U - turns) % 4U) * 90.0F;
}

// The FACING yaw for a horizontally-attached diode (south is vanilla identity).
// Mirrors ChunkMesher::yawForHorizontalFacing.
[[nodiscard]] constexpr float diodeYaw(BlockOrientation facing) {
    // A diode's json identity variant is `facing=south` (repeater.json /
    // comparator.json give south no `"y"` at all), so it is the base-South case
    // of the rule above. Kept as its own name because three call sites read it,
    // and static_asserted against the general form so the two cannot drift.
    return yawFromModelBase(BlockOrientation::South, facing);
}
static_assert(diodeYaw(BlockOrientation::South) == 0.0F);
static_assert(diodeYaw(BlockOrientation::East) == 90.0F);
static_assert(diodeYaw(BlockOrientation::North) == 180.0F);
static_assert(diodeYaw(BlockOrientation::West) == 270.0F);

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
    // RN-10a: the three model families whose identity variant is not the diode's
    // `facing=south`. Each cites the blockstate json it was read off.
    switch (blockDefinition(block).model) {
    case BlockModel::Door:
        // oak_door.json: facing=east,hinge=left,open=false carries no `"y"`, so
        // the door's identity variant stands against the west wall — East in
        // BlockShape's terms. Its rotation follows DoorBlock#getShape's
        // doorDirection, so model and shape turn together by construction.
        return {axisMatrix('y', yawFromModelBase(BlockOrientation::East,
                                                 doorSwingOrientation(
                                                     state.orientation(), state.open(),
                                                     state.hinge() == DoorHinge::Right)))};
    case BlockModel::TrapDoor:
        // oak_trapdoor.json: every open=false row has no `"y"` at all — a closed
        // trapdoor lies flat and its facing is invisible — and the open rows are
        // keyed off facing=north, the identity.
        return {axisMatrix('y', state.open()
                                    ? yawFromModelBase(BlockOrientation::North,
                                                       state.orientation())
                                    : 0.0F)};
    case BlockModel::FenceGate:
        // oak_fence_gate.json: facing=south carries no `"y"`.
        return {axisMatrix('y',
                           yawFromModelBase(BlockOrientation::South, state.orientation()))};
    default:
        break;
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
        // `"cullface"` declaration. RN-8d: the trailing argument is the face's
        // own `"rotation"` / 90 — 13 of this model's 21 faces carry one, and all
        // 13 were being dropped, which is why the anvil's body texture ran the
        // wrong way round on its sides and caps.
        detail::putFace(base, Facing::Down, 0, detail::rect(2, 2, 14, 14),
                        detail::cullToward(Facing::Down), kQuadrant180);
        detail::putFace(base, Facing::Up, 0, detail::rect(2, 2, 14, 14), kNoCull, kQuadrant180);
        detail::putFace(base, Facing::North, 0, detail::rect(2, 12, 14, 16));
        detail::putFace(base, Facing::South, 0, detail::rect(2, 12, 14, 16));
        detail::putFace(base, Facing::West, 0, detail::rect(0, 2, 4, 14), kNoCull, kQuadrant90);
        detail::putFace(base, Facing::East, 0, detail::rect(4, 2, 0, 14), kNoCull, kQuadrant270);
        elements.push_back(base);
    }
    {
        ModelElement waist;
        waist.from16 = {4.0F, 4.0F, 3.0F};
        waist.to16 = {12.0F, 5.0F, 13.0F};
        detail::putFace(waist, Facing::Up, 0, detail::rect(4, 3, 12, 13), kNoCull, kQuadrant180);
        detail::putFace(waist, Facing::North, 0, detail::rect(4, 11, 12, 12));
        detail::putFace(waist, Facing::South, 0, detail::rect(4, 11, 12, 12));
        detail::putFace(waist, Facing::West, 0, detail::rect(4, 3, 5, 13), kNoCull, kQuadrant90);
        detail::putFace(waist, Facing::East, 0, detail::rect(5, 3, 4, 13), kNoCull, kQuadrant270);
        elements.push_back(waist);
    }
    {
        ModelElement neck;
        neck.from16 = {6.0F, 5.0F, 4.0F};
        neck.to16 = {10.0F, 10.0F, 12.0F};
        detail::putFace(neck, Facing::North, 0, detail::rect(6, 6, 10, 11));
        detail::putFace(neck, Facing::South, 0, detail::rect(6, 6, 10, 11));
        detail::putFace(neck, Facing::West, 0, detail::rect(5, 4, 10, 12), kNoCull, kQuadrant90);
        detail::putFace(neck, Facing::East, 0, detail::rect(10, 4, 5, 12), kNoCull, kQuadrant270);
        elements.push_back(neck);
    }
    {
        ModelElement top;
        top.from16 = {3.0F, 10.0F, 0.0F};
        top.to16 = {13.0F, 16.0F, 16.0F};
        detail::putFace(top, Facing::Down, 0, detail::rect(3, 0, 13, 16), kNoCull, kQuadrant180);
        detail::putFace(top, Facing::Up, 1, detail::rect(3, 0, 13, 16), kNoCull, kQuadrant180);
        detail::putFace(top, Facing::North, 0, detail::rect(3, 0, 13, 6));
        detail::putFace(top, Facing::South, 0, detail::rect(3, 0, 13, 6));
        detail::putFace(top, Facing::West, 0, detail::rect(10, 0, 16, 16), kNoCull, kQuadrant90);
        detail::putFace(top, Facing::East, 0, detail::rect(16, 0, 10, 16), kNoCull, kQuadrant270);
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
    default:
        break;
    }
    // RN-10a: door / trapdoor / fence gate are whole model *families* — eight
    // door species, six trapdoors — that share one geometry and differ only in
    // the sprite, so they dispatch on the model rather than being listed block by
    // block the way the five singletons above are.
    switch (blockDefinition(block).model) {
    case BlockModel::Door: return doorElements(state);
    case BlockModel::TrapDoor: return trapdoorElements(state);
    case BlockModel::FenceGate: return fenceGateElements(state);
    default: return {};
    }
}

// Bake an ElementModel block to its quads: every present face of every element,
// through the N1 primitive, with the block's attachment transform applied. Quad
// order is elements in build order, faces in Facing order (Down..East), so a
// consumer/test can pair quads by index with the element description.
[[nodiscard]] inline std::vector<BakedElementQuad> bakeElements(
    std::span<const ModelElement> elements, const ModelTransform& attach) {
    std::vector<BakedElementQuad> quads;
    for (const ModelElement& element : elements) {
        for (std::uint8_t f = 0; f < kFacingCount; ++f) {
            const ElementFace& face = element.faces[f];
            if (!face.present) {
                continue;
            }
            quads.push_back({bakeElementFace(element.from16, element.to16, static_cast<Facing>(f),
                                             face, element.rotation, attach),
                             element.shade});
        }
    }
    return quads;
}

[[nodiscard]] inline std::vector<BakedElementQuad> bakeElementModel(Block block, BlockState state) {
    const std::vector<ModelElement> elements = elementsFor(block, state);
    return bakeElements(elements, attachTransform(block, state));
}

// --- RN-8c-0: the baked store -------------------------------------------------
//
// `bakeElementModel` above allocates twice (elementsFor's vector, then its own)
// and runs cos/sin per rotated quad. That is JE's "bake once into a BakedModel"
// transcribed into "bake per cell": every remesh re-baked every repeater, lever
// and anvil cell in the section, which is exactly the kind of cost that hides
// until a player breaks one block and the whole section rebuilds.
//
// This is the bake-once half. The store is one flat `std::vector<BakedElementQuad>`
// plus a `{offset, count}` index — not a `vector<vector>` and not JE's
// `List<BakedQuad>` object graph, which is a reference in Java and a pointer
// chase in C++. The hot path becomes "index the range, walk contiguous memory,
// translate into the cell": zero allocation, zero trig.
//
// Keyed by (model kind, state variant), never by block: the three anvil wear
// states share one geometry table and differ only in the texture slot the mesher
// resolves per block, which is the "geometry per model, texture layer per block"
// split RN-8's performance section pins down.

enum class ElementModelKind : std::uint8_t {
    Repeater,
    Comparator,
    Lever,
    EnchantingTable,
    Anvil,
    // RN-10a: keyed by model family, not by block — the eight door species and
    // six trapdoors share one geometry table and differ only in the atlas layer
    // the mesher resolves, which is the same "geometry per model, texture per
    // block" split the three anvil wear states already take.
    Door,
    TrapDoor,
    FenceGate,
    None,
};

[[nodiscard]] constexpr ElementModelKind elementModelKind(Block block) {
    switch (block) {
    case Block::Repeater: return ElementModelKind::Repeater;
    case Block::Comparator: return ElementModelKind::Comparator;
    case Block::Lever: return ElementModelKind::Lever;
    case Block::EnchantingTable: return ElementModelKind::EnchantingTable;
    case Block::Anvil:
    case Block::ChippedAnvil:
    case Block::DamagedAnvil: return ElementModelKind::Anvil;
    default:
        break;
    }
    switch (blockDefinition(block).model) {
    case BlockModel::Door: return ElementModelKind::Door;
    case BlockModel::TrapDoor: return ElementModelKind::TrapDoor;
    case BlockModel::FenceGate: return ElementModelKind::FenceGate;
    default: return ElementModelKind::None;
    }
}

namespace detail {

// The four horizontal facings, with Up/Down folded onto South exactly as
// `diodeYaw`'s default arm does — so a diode or anvil that somehow holds a
// vertical orientation bakes the same model it renders.
[[nodiscard]] constexpr std::size_t horizontalIndex(BlockOrientation facing) {
    switch (facing) {
    case BlockOrientation::North: return 0;
    case BlockOrientation::East: return 1;
    case BlockOrientation::West: return 3;
    default: return 2; // South, and the vertical pair diodeYaw treats as South
    }
}
[[nodiscard]] constexpr BlockOrientation horizontalOf(std::size_t index) {
    switch (index) {
    case 0: return BlockOrientation::North;
    case 1: return BlockOrientation::East;
    case 3: return BlockOrientation::West;
    default: return BlockOrientation::South;
    }
}

} // namespace detail

// How many baked variants a kind has, and which one a state selects. The two
// must agree; `elementModelStore()` asserts the round trip when it fills.
[[nodiscard]] constexpr std::size_t elementModelVariantCount(ElementModelKind kind) {
    switch (kind) {
    case ElementModelKind::Repeater: return 4U * 4U * 2U * 2U; // facing x delay x powered x locked
    case ElementModelKind::Comparator: return 4U * 2U * 2U; // facing x mode x powered
    case ElementModelKind::Lever: return 6U * 2U;           // facing (all six) x powered
    case ElementModelKind::EnchantingTable: return 1U;
    case ElementModelKind::Anvil: return 4U; // facing
    case ElementModelKind::Door: return 4U * 2U * 2U * 2U;  // facing x half x hinge x open
    case ElementModelKind::TrapDoor: return 4U * 2U * 2U;   // facing x half x open
    case ElementModelKind::FenceGate: return 4U * 2U * 2U;  // facing x open x in_wall
    case ElementModelKind::None: return 0U;
    }
    return 0U;
}

[[nodiscard]] constexpr std::size_t elementModelVariant(ElementModelKind kind, BlockState state) {
    switch (kind) {
    case ElementModelKind::Repeater:
        return ((detail::horizontalIndex(state.orientation()) * 4U +
                 static_cast<std::size_t>(state.repeaterDelay() - 1)) *
                    2U +
                (state.powered() ? 1U : 0U)) *
                   2U +
               (state.repeaterLocked() ? 1U : 0U);
    case ElementModelKind::Comparator:
        return (detail::horizontalIndex(state.orientation()) * 2U +
                (state.comparatorSubtract() ? 1U : 0U)) *
                   2U +
               (state.powered() ? 1U : 0U);
    case ElementModelKind::Lever:
        return static_cast<std::size_t>(state.orientation()) * 2U + (state.powered() ? 1U : 0U);
    case ElementModelKind::EnchantingTable:
        return 0U;
    case ElementModelKind::Anvil:
        return detail::horizontalIndex(state.orientation());
    case ElementModelKind::Door:
        return ((detail::horizontalIndex(state.orientation()) * 2U +
                 (state.isDoorUpperHalf() ? 1U : 0U)) *
                    2U +
                (state.hinge() == DoorHinge::Right ? 1U : 0U)) *
                   2U +
               (state.open() ? 1U : 0U);
    case ElementModelKind::TrapDoor:
        return (detail::horizontalIndex(state.orientation()) * 2U +
                (state.isDoorUpperHalf() ? 1U : 0U)) *
                   2U +
               (state.open() ? 1U : 0U);
    case ElementModelKind::FenceGate:
        return (detail::horizontalIndex(state.orientation()) * 2U + (state.open() ? 1U : 0U)) *
                   2U +
               (state.inWall() ? 1U : 0U);
    case ElementModelKind::None:
        return 0U;
    }
    return 0U;
}

namespace detail {

// The state a variant index stands for — the inverse of `elementModelVariant`,
// used only when filling the store.
[[nodiscard]] inline BlockState elementModelVariantState(ElementModelKind kind, Block block,
                                                         std::size_t variant) {
    switch (kind) {
    case ElementModelKind::Repeater:
        return BlockState{block, horizontalOf(variant / 16U)}
            .withRepeaterDelay(static_cast<int>((variant / 4U) % 4U) + 1)
            .withPowered(((variant / 2U) & 1U) != 0U)
            .withRepeaterLocked((variant & 1U) != 0U);
    case ElementModelKind::Comparator:
        return BlockState{block, horizontalOf(variant / 4U)}
            .withComparatorSubtract(((variant / 2U) & 1U) != 0U)
            .withPowered((variant & 1U) != 0U);
    case ElementModelKind::Lever:
        return BlockState{block, static_cast<BlockOrientation>(variant / 2U)}.withPowered(
            (variant & 1U) != 0U);
    case ElementModelKind::EnchantingTable:
        return BlockState{block};
    case ElementModelKind::Anvil:
        return BlockState{block, horizontalOf(variant)};
    case ElementModelKind::Door:
        return BlockState{block, horizontalOf(variant / 8U)}
            .withDoorUpperHalf(((variant / 4U) & 1U) != 0U)
            .withHinge(((variant / 2U) & 1U) != 0U ? DoorHinge::Right : DoorHinge::Left)
            .withOpen((variant & 1U) != 0U);
    case ElementModelKind::TrapDoor:
        return BlockState{block, horizontalOf(variant / 4U)}
            .withDoorUpperHalf(((variant / 2U) & 1U) != 0U)
            .withOpen((variant & 1U) != 0U);
    case ElementModelKind::FenceGate:
        return BlockState{block, horizontalOf(variant / 4U)}
            .withOpen(((variant / 2U) & 1U) != 0U)
            .withInWall((variant & 1U) != 0U);
    case ElementModelKind::None:
        return BlockState{block};
    }
    return BlockState{block};
}

// One kind's representative block — the one whose geometry the whole kind shares.
[[nodiscard]] constexpr Block elementModelKindBlock(ElementModelKind kind) {
    switch (kind) {
    case ElementModelKind::Repeater: return Block::Repeater;
    case ElementModelKind::Comparator: return Block::Comparator;
    case ElementModelKind::Lever: return Block::Lever;
    case ElementModelKind::EnchantingTable: return Block::EnchantingTable;
    case ElementModelKind::Anvil: return Block::Anvil;
    case ElementModelKind::Door: return Block::OakDoor;
    case ElementModelKind::TrapDoor: return Block::OakTrapdoor;
    case ElementModelKind::FenceGate: return Block::OakFenceGate;
    case ElementModelKind::None: return Block::Air;
    }
    return Block::Air;
}

} // namespace detail

struct QuadRange final {
    std::uint32_t offset = 0;
    std::uint32_t count = 0;
};

class ElementModelStore final {
  public:
    ElementModelStore() {
        constexpr std::size_t kKindCount = static_cast<std::size_t>(ElementModelKind::None);
        std::size_t base = 0;
        for (std::size_t k = 0; k < kKindCount; ++k) {
            const auto kind = static_cast<ElementModelKind>(k);
            kindBase_[k] = static_cast<std::uint32_t>(base);
            const std::size_t variants = elementModelVariantCount(kind);
            const Block block = detail::elementModelKindBlock(kind);
            for (std::size_t variant = 0; variant < variants; ++variant) {
                const BlockState state =
                    detail::elementModelVariantState(kind, block, variant);
                const auto baked = bakeElementModel(block, state);
                ranges_.push_back({static_cast<std::uint32_t>(quads_.size()),
                                   static_cast<std::uint32_t>(baked.size())});
                quads_.insert(quads_.end(), baked.begin(), baked.end());
            }
            base += variants;
        }
    }

    [[nodiscard]] std::span<const BakedElementQuad> quads(Block block, BlockState state) const {
        const auto kind = elementModelKind(block);
        if (kind == ElementModelKind::None) {
            return {};
        }
        const std::size_t index = kindBase_[static_cast<std::size_t>(kind)] +
                                  elementModelVariant(kind, state);
        const QuadRange range = ranges_[index];
        return {quads_.data() + range.offset, range.count};
    }

    [[nodiscard]] std::size_t quadCount() const { return quads_.size(); }
    [[nodiscard]] std::size_t rangeCount() const { return ranges_.size(); }
    // Bytes the whole store occupies, for the size guardrail: a table that grows
    // per block rather than per model would show up here.
    [[nodiscard]] std::size_t byteSize() const {
        return quads_.size() * sizeof(BakedElementQuad) + ranges_.size() * sizeof(QuadRange) +
               sizeof(kindBase_);
    }

  private:
    std::vector<BakedElementQuad> quads_;
    std::vector<QuadRange> ranges_;
    std::array<std::uint32_t, static_cast<std::size_t>(ElementModelKind::None)> kindBase_{};
};

// The one store. A function-local static so it is built once, on first use, with
// the thread-safe initialisation the meshing workers need — the mesher is the
// only caller and it runs off the main thread.
[[nodiscard]] inline const ElementModelStore& elementModelStore() {
    static const ElementModelStore store;
    return store;
}

// The hot-path entry point: a view into contiguous baked quads, no allocation.
[[nodiscard]] inline std::span<const BakedElementQuad> bakedElementModel(Block block,
                                                                        BlockState state) {
    return elementModelStore().quads(block, state);
}

} // namespace mc::world::bake

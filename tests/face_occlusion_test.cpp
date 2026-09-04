#include "world/Block.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"

#include <array>
#include <cassert>
#include <cstdint>

// RN-8a: the face-occlusion criterion's truth table.
//
// This is the judgement the mesher's `shouldRenderFace` now rests on, in place
// of the renderLayer three-way guess plus its two hand-written exemptions
// (`modelHeight < 1` for farmland, `isSlab` for the state the old signature
// could not see). It is 26.1's `getFaceOcclusionShape(dir) == Shapes.block()`
// (Block.shouldRenderFace, Block.java:304) reduced to what a closed roster of
// fixed AABB sets needs: a face is sealed when one box covers that whole cell
// wall. See faceOccludesFully's comment for the single known deviation from the
// union answer (a straight stair's back face) and why it is inert today.
namespace {

using namespace mc::world;

constexpr std::array<Face, 6> kAllFaces{Face::PositiveX, Face::NegativeX, Face::PositiveY,
                                        Face::NegativeY, Face::PositiveZ, Face::NegativeZ};

// The six faces as a bitmask, so a whole row of the truth table is one literal.
constexpr std::uint8_t kNone = 0U;
constexpr std::uint8_t kAll = 0b111111U;
// Single-face masks, named so an expectation reads as the face it means.
constexpr std::uint8_t kUp = 1U << static_cast<std::uint8_t>(Face::PositiveY);
constexpr std::uint8_t kDown = 1U << static_cast<std::uint8_t>(Face::NegativeY);
constexpr std::uint8_t kNorth = 1U << static_cast<std::uint8_t>(Face::NegativeZ);
constexpr std::uint8_t bit(Face face) {
    return static_cast<std::uint8_t>(1U << static_cast<unsigned>(face));
}

// What `faceOccludesFully` answers for all six faces of a shape, as a mask.
[[nodiscard]] std::uint8_t sealedFaces(const BlockShape& shape) {
    std::uint8_t mask = 0U;
    for (const Face face : kAllFaces) {
        if (faceOccludesFully(shape, face)) {
            mask |= bit(face);
        }
    }
    return mask;
}

[[nodiscard]] std::uint8_t sealedFaces(BlockState state) {
    return sealedFaces(blockShape(state));
}

} // namespace

int main() {
    // --- Empty: nothing seals anything. ---
    assert(sealedFaces(BlockShape{ShapeKind::Empty, 0.0F, 0.0F, {}}) == kNone);
    assert(sealedFaces(BlockState{Block::Air}) == kNone);

    // --- Column: a cap only needs its own end; a side needs the full height. ---
    assert(sealedFaces(BlockShape{ShapeKind::Column, 0.0F, 1.0F, {}}) == kAll);
    assert(sealedFaces(BlockShape{ShapeKind::Column, 0.0F, 0.5F, {}}) == bit(Face::NegativeY));
    assert(sealedFaces(BlockShape{ShapeKind::Column, 0.5F, 1.0F, {}}) == bit(Face::PositiveY));
    assert(sealedFaces(BlockShape{ShapeKind::Column, 0.25F, 0.75F, {}}) == kNone);

    // A full cube is the all-six row this whole criterion has to keep answering:
    // stone against stone must go on being culled exactly as it was.
    assert(sealedFaces(BlockState{Block::Stone}) == kAll);

    // --- Slab: the state the old `isSlab` exemption could not see. ---
    {
        const BlockState slab{Block::StoneSlab};
        assert(sealedFaces(slab.with(StateProperty::SlabType,
                                     static_cast<std::uint8_t>(SlabPortion::Bottom))) ==
               bit(Face::NegativeY));
        assert(sealedFaces(slab.with(StateProperty::SlabType,
                                     static_cast<std::uint8_t>(SlabPortion::Top))) ==
               bit(Face::PositiveY));
        // The row RN-8a tightens: a double slab is geometrically a full cube, so
        // it seals all six and the neighbour's facing quads stop being drawn.
        assert(sealedFaces(slab.with(StateProperty::SlabType,
                                     static_cast<std::uint8_t>(SlabPortion::Double))) == kAll);
    }

    // --- Farmland and dirt path: both declare `.height()`, both are truncated
    // columns, and both must seal only their own floor. Dirt path only answers
    // this correctly because shapeCube reads the declared modelHeight instead of
    // asking "is this Farmland" — with the identity check it was a full cube,
    // which after RN-8a would have culled its neighbours' side faces and opened a
    // 1/16 see-through gap above it. ---
    for (const Block block : {Block::Farmland, Block::DirtPath}) {
        const auto shape = blockShape(BlockState{block});
        assert(shape.kind == ShapeKind::Column);
        assert(shape.top < 1.0F);
        assert(sealedFaces(BlockState{block}) == bit(Face::NegativeY));
    }

    // --- Stairs, all four facings: the lower slab box seals the floor on its
    // own; no single box seals any other face. The back face IS sealed by the
    // union of slab + step, and this criterion deliberately answers false for it
    // (see faceOccludesFully). That costs nothing while stairs are Cutout: the
    // mask assertion further down pins that the block never occludes at all. ---
    for (const auto facing : {BlockOrientation::North, BlockOrientation::East,
                              BlockOrientation::South, BlockOrientation::West}) {
        const BlockState bottom = BlockState{Block::OakStairs}.with(facing);
        assert(sealedFaces(bottom) == bit(Face::NegativeY));
        const BlockState top = bottom.withStairHalf(SlabPortion::Top);
        assert(sealedFaces(top) == bit(Face::PositiveY));
    }

    // --- Anvil, all four facings: four stacked boxes, none of which covers a
    // whole cell wall (the base is 12x12, the top plate 10 wide). This is defect
    // B: the anvil is an Opaque-bucket block, so the old criterion culled the top
    // face of whatever it stood on and left a 2px ring of holes. ---
    for (const auto facing : {BlockOrientation::North, BlockOrientation::East,
                              BlockOrientation::South, BlockOrientation::West}) {
        for (const Block block : {Block::Anvil, Block::ChippedAnvil, Block::DamagedAnvil}) {
            const BlockState state = BlockState{block}.with(facing);
            assert(blockShape(state).kind == ShapeKind::Boxes);
            assert(sealedFaces(state) == kNone);
            assert(faceOcclusionMask(state) == kNone);
        }
    }

    // --- Enchanting table: Column{0, 12/16}. Its floor is sealed (vanilla culls
    // the top face of the block under it too), its sides are not — which is the
    // seam half of defect B. ---
    assert(sealedFaces(BlockState{Block::EnchantingTable}) == bit(Face::NegativeY));

    // --- Torch: the slim interaction box seals nothing, on the floor or on a
    // wall. Both halves of the guardrail are checked here: the shape is empty of
    // any full face, AND the block cannot occlude at all — a torch must never
    // cull the water face beside it (defect A) whichever of the two answers
    // first. ---
    for (const auto facing : {BlockOrientation::Up, BlockOrientation::North,
                              BlockOrientation::East, BlockOrientation::South,
                              BlockOrientation::West}) {
        const BlockState torch = BlockState{Block::Torch}.with(facing);
        assert(sealedFaces(torch) == kNone);
        assert(faceOcclusionMask(torch) == kNone);
    }
    assert(sealedFaces(BlockState{Block::Chest}) == kNone);

    // --- The canOcclude gate, on top of the shape. ---
    // Glass is a Cube model: shape-wise it seals all six, and if the criterion
    // were `isFullCube` it would start occluding its neighbours. canOcclude keeps
    // its mask empty, which is the guardrail RN-8's design doc names by name.
    assert(sealedFaces(BlockState{Block::Glass}) == kAll);
    assert(faceOcclusionMask(BlockState{Block::Glass}) == kNone);
    assert(faceOcclusionMask(BlockState{Block::Stone}) == kAll);
    // RN-8e: geometrically solid Cutout blocks now declare `.occludes()`, so the
    // shape criterion actually runs for them. A bottom-half stair's lower slab
    // fills the cell's whole footprint from y=0 to y=0.5 and therefore seals the
    // cell's DOWN face; flip it to the top half and it seals UP instead. That
    // state dependence is why its table entry below is the sentinel.
    assert(faceOcclusionMask(BlockState{Block::OakStairs}) == kDown);
    assert(faceOcclusionMask(BlockState{Block::OakStairs}.withStairHalf(SlabPortion::Top)) == kUp);
    // And no more than that: the stair's BACK face is sealed by its slab plus its
    // step together, which the single-box criterion deliberately does not see
    // (this header's comment calls that deviation out). It used to be inert
    // because the mask was zero; now it is a real, registered under-report —
    // safe, because under-reporting occlusion only ever draws a face nobody can
    // see, never removes one.
    assert((faceOcclusionMask(BlockState{Block::OakStairs}) & kNorth) == 0U);
    // Leaves say `noOcclusion()` out loud now instead of inheriting it from the
    // bucket: a full cube whose model is full of holes must never cull.
    assert(faceOcclusionMask(BlockState{Block::OakLeaves}) == kNone);
    // A wall, a gate and a button all occlude in vanilla and seal nothing here —
    // the flag and the shape are separate answers and this is what that looks
    // like when they disagree.
    assert(canOcclude(Block::CobblestoneWall));
    assert(faceOcclusionMask(BlockState{Block::CobblestoneWall}) == kNone);
    assert(canOcclude(Block::OakFenceGate));
    assert(faceOcclusionMask(BlockState{Block::OakFenceGate}) == kNone);
    assert(canOcclude(Block::StoneButton));
    assert(faceOcclusionMask(BlockState{Block::StoneButton}) == kNone);
    // Doors and trapdoors are `noOcclusion()` in vanilla (Blocks.java:1403,
    // 2115): a closed door's 3px leaf spans a whole cell wall, so without the
    // flag the shape would happily seal the face behind it.
    assert(!canOcclude(Block::OakDoor));
    assert(faceOcclusionMask(BlockState{Block::OakDoor}) == kNone);
    assert(!canOcclude(Block::OakTrapdoor));
    assert(faceOcclusionMask(BlockState{Block::OakTrapdoor}) == kNone);
    // The pressure plate is the one eligible block deliberately left out: vanilla
    // occludes it, but vanilla's shape is `Block.column(14, 0, 1)` — inset a
    // pixel on each side — while this build's is a full-footprint Column, so
    // `.occludes()` would seal a face vanilla does not. Fix the shape first.
    assert(!canOcclude(Block::StonePressurePlate));

    // --- The per-block table and its sentinel. ---
    {
        const auto entry = [](Block block) {
            return kOcclusionMaskByBlock[static_cast<std::size_t>(block)];
        };
        assert(entry(Block::Stone) == kAll);
        assert(entry(Block::Air) == kNone);
        assert(entry(Block::Glass) == kNone);
        assert(entry(Block::Torch) == kNone);
        // RN-8e: a stair occludes and its sealed face moves with HALF, which is
        // exactly the pair of conditions the sentinel exists for.
        assert(entry(Block::OakStairs) == kOcclusionStateDependent);
        // Only a block that both occludes and reshapes with its state is worth a
        // `chunk->state()` at snapshot-fill time.
        assert((entry(Block::StoneSlab) & kOcclusionStateDependent) != 0U);
        assert((entry(Block::Anvil) & kOcclusionStateDependent) != 0U);
        assert((entry(Block::Stone) & kOcclusionStateDependent) == 0U);
        // Whatever a sentinel resolves to still fits the six bits the mesher's
        // flags_ byte has spare above bit0/bit1.
        for (std::size_t index = 0; index < kOcclusionMaskByBlock.size(); ++index) {
            const std::uint8_t value = kOcclusionMaskByBlock[index];
            assert(value == kOcclusionStateDependent || (value & ~kAll) == 0U);
            assert(faceOcclusionMask(BlockState{static_cast<Block>(index)}) <= kAll);
        }
    }

    return 0;
}

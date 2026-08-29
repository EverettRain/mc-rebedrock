#pragma once

// STRUCT-2: the rigid rotation applied to a structure template at placement.
//
// The DOD replacement for Java's per-block `BlockState.rotate(Rotation)` +
// `StructureTemplate.calculateRelativePosition` reflection: a structure is placed
// once, so its four rotations are a constexpr integer coordinate transform plus a
// small `(orientation, rotation)` lookup for directional blocks. No `Rotation`
// object per block, no reflection — the transform is a switch over four cases,
// resolved at compile time.
//
// Mirroring is not modelled here yet (the shipped structures a build reaches first
// are rotation-only); it slots in the same way when a structure needs it.

#include "world/Block.hpp"

#include <cstdint>

namespace mc::world {

// Clockwise viewed from above (+Y up), matching vanilla's Rotation enum order.
enum class StructureRotation : std::uint8_t {
    None,
    Clockwise90,
    Clockwise180,
    Counterclockwise90,
};

// A template-local coordinate.
struct LocalPos final {
    int x = 0;
    int y = 0;
    int z = 0;
    [[nodiscard]] constexpr bool operator==(const LocalPos&) const = default;
};

// The template's footprint after rotation: a 90°/270° turn swaps X and Z.
[[nodiscard]] constexpr int rotatedSizeX(int sizeX, int sizeZ, StructureRotation rotation) {
    return (rotation == StructureRotation::Clockwise90 ||
            rotation == StructureRotation::Counterclockwise90)
               ? sizeZ
               : sizeX;
}
[[nodiscard]] constexpr int rotatedSizeZ(int sizeX, int sizeZ, StructureRotation rotation) {
    return (rotation == StructureRotation::Clockwise90 ||
            rotation == StructureRotation::Counterclockwise90)
               ? sizeX
               : sizeZ;
}

// Rotates a local coordinate within a (sizeX × sizeZ) footprint, keeping every
// result inside the rotated box [0, rotatedSize). Y is unchanged.
[[nodiscard]] constexpr LocalPos rotateLocal(LocalPos pos, int sizeX, int sizeZ,
                                             StructureRotation rotation) {
    switch (rotation) {
    case StructureRotation::None:
        return pos;
    case StructureRotation::Clockwise90:
        return {sizeZ - 1 - pos.z, pos.y, pos.x};
    case StructureRotation::Clockwise180:
        return {sizeX - 1 - pos.x, pos.y, sizeZ - 1 - pos.z};
    case StructureRotation::Counterclockwise90:
        return {pos.z, pos.y, sizeX - 1 - pos.x};
    }
    return pos;
}

// Rotates a horizontal facing; vertical (Up/Down) is unchanged. Clockwise90
// sends North -> East -> South -> West -> North.
[[nodiscard]] constexpr BlockOrientation rotateOrientation(BlockOrientation orientation,
                                                           StructureRotation rotation) {
    if (orientation == BlockOrientation::Up || orientation == BlockOrientation::Down) {
        return orientation;
    }
    // North=0, East=1, South=2, West=3 (BlockOrientation's enum order), so a
    // clockwise quarter turn is +1 mod 4.
    const auto steps = static_cast<int>(rotation == StructureRotation::Clockwise90       ? 1
                                        : rotation == StructureRotation::Clockwise180     ? 2
                                        : rotation == StructureRotation::Counterclockwise90 ? 3
                                                                                          : 0);
    const int turned = (static_cast<int>(orientation) + steps) & 0x3;
    return static_cast<BlockOrientation>(turned);
}

// Applies the rotation to a packed state: rebuilds it with its facing turned. A
// block with no horizontal facing is unaffected (rotateOrientation is identity on
// its default, and reapplying the orientation is a no-op for it).
[[nodiscard]] inline BlockState rotateState(BlockState state, StructureRotation rotation) {
    if (rotation == StructureRotation::None) {
        return state;
    }
    return state.with(rotateOrientation(state.orientation(), rotation));
}

} // namespace mc::world

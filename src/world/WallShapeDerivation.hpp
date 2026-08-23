#pragma once

// AR-B3's wall connection derivation: world-layer logic both the placement
// path (BlockPlacement.cpp, which needs a wall's four connection bits the
// instant it is placed, before any neighbour notification runs) and the
// gameplay updateShape slot (gameplay/BlockBehavior.hpp) call — the same split
// StairShapeDerivation.hpp already established for the stair join shape.
// Lives in world/ for the identical reason: BlockPlacement.cpp (world layer)
// needs wallConnectionsFor too, and world/ never depends on gameplay/.
//
// Simplified from vanilla's WallBlock (WallBlock.java): no LOW/TALL distinction
// (a fence-style plain connected/not-connected bool per side, per this task's
// StateSchema — see StateSchema.hpp's WallNorth/East/South/West comment) and
// no UP-post raise/lower logic (the post is unconditional here). The
// connection *rule itself* — which neighbours a wall joins to — is ported
// faithfully from WallBlock#connectsTo: a sturdy face, another wall, or a
// fence gate whose FACING runs along the connecting axis.

#include "world/Block.hpp"
#include "world/BlockPlacement.hpp" // orientationOffset
#include "world/BlockPos.hpp"
#include "world/BlockState.hpp"
#include "world/World.hpp"

#include <array>

namespace mc::world {

namespace detail {

// The four horizontal directions a wall's connection mask iterates, in a
// fixed order shared by every caller (placement, updateShape, the shape
// table's own North/East/South/West axis reads) so nothing has to remember a
// different order per call site.
inline constexpr std::array<BlockOrientation, 4> kWallHorizontals{
    BlockOrientation::North,
    BlockOrientation::East,
    BlockOrientation::South,
    BlockOrientation::West,
};

// WallBlock#connectsTo, ported: a wall joins a neighbour that is itself a wall
// (any wall block, matching vanilla's BlockTags.WALLS), a fence gate whose
// FACING axis runs *across* the connection (FenceGateBlock#connectsToDirection:
// the gate's axis equals the connecting direction's clockwise axis — a gate
// facing north/south connects to a wall approaching from east/west, and vice
// versa, since the gate's opening faces along its FACING and its solid posts
// face across it), or any other block presenting a sturdy face on that side.
[[nodiscard]] inline bool wallConnectsTo(const World& world, BlockPos neighborPos,
                                        BlockOrientation towardWall) {
    const auto neighborBlock = world.block(neighborPos.x, neighborPos.y, neighborPos.z);
    if (blockDefinition(neighborBlock).model == BlockModel::Wall) {
        return true;
    }
    if (blockDefinition(neighborBlock).model == BlockModel::FenceGate) {
        const auto neighborState = world.state(neighborPos.x, neighborPos.y, neighborPos.z);
        const auto gateFacing = neighborState.orientation();
        // sameHorizontalAxis(gateFacing, clockwise(towardWall)) — the gate's own
        // axis matches the connecting direction's perpendicular, i.e. the gate's
        // axis is *not* the same axis as the direction pointing at the wall.
        return !sameHorizontalAxis(gateFacing, towardWall);
    }
    return isFaceSturdy(neighborBlock);
}

} // namespace detail

// WallBlock's four connection bits, re-derived fresh from the world (the same
// "read every relevant neighbour, not just the one that changed" shape
// stairShapeFor already takes, so repeated notification converges to the same
// answer regardless of which single neighbour triggered it).
[[nodiscard]] inline BlockState wallConnectionsFor(const World& world, BlockPos pos,
                                                   BlockState state) {
    BlockState result = state;
    for (const auto side : detail::kWallHorizontals) {
        const auto offset = orientationOffset(side);
        const BlockPos neighborPos{pos.x + offset.x, pos.y + offset.y, pos.z + offset.z};
        result = result.withWallConnected(side, detail::wallConnectsTo(world, neighborPos, side));
    }
    return result;
}

// The updateShape slot for BlockModel::Wall. Only a horizontal neighbour (or
// the cell the wall itself sits on/against) can change its connections; a
// vertical neighbour above is ignored here the same way a stair's shape
// ignores a vertical notification — this task does not carry the UP-post
// raise/lower logic that would make the cell above matter (see the file
// comment's "known simplification").
[[nodiscard]] inline BlockState wallUpdateShape(const World& world, BlockPos pos, BlockState state,
                                                BlockPos fromOffset) {
    const glm::ivec3 offset{fromOffset.x, fromOffset.y, fromOffset.z};
    if (isHorizontal(orientationFromOffset(offset))) {
        return wallConnectionsFor(world, pos, state);
    }
    return state; // vertical neighbour: unchanged (fixed point)
}

} // namespace mc::world

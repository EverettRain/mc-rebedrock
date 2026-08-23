#pragma once

// AR-B2's stair/door shape derivations: world-layer logic both the placement
// path (BlockPlacement.cpp, which needs a stair's join shape the instant it is
// placed, before any neighbour notification runs) and the gameplay updateShape
// slot (gameplay/BlockBehavior.hpp, wired there since that is where the
// behaviour table lives) call. Lives in world/ — not gameplay/ — because
// BlockPlacement.cpp (world layer) needs stairShapeFor too, and world/ never
// depends on gameplay/ (the reverse is fine, and is exactly how
// BlockBehavior.hpp reaches this header).
//
// The updateShape wrappers below are pure property rewrites of the same cell
// under the A3b contract dispatchUpdateShape enforces (gameplay/BlockBehavior.
// hpp's applyUpdateShapeContract): neither one ever changes the block or
// breaks it — a door's "the other half vanished, so I must too" case is
// intentionally *not* handled here (that would violate A3b's "same block"
// rule); it belongs to the ordinary onNeighborChanged/support pass, the
// ordinary channel a block already leaves the world through, not this one.

#include "world/Block.hpp"
#include "world/BlockPlacement.hpp" // orientationOffset, orientationFromOffset
#include "world/BlockPos.hpp"
#include "world/BlockState.hpp"
#include "world/World.hpp"

#include <optional>

namespace mc::world {

namespace detail {

// StairBlock#canTakeShape: the candidate neighbour (the cell *beyond* the
// stair being checked, in the direction that would make it join) must not
// itself be a matching stair — joining two stairs whose far side is a third,
// identical stair would make an ambiguous corner, so vanilla refuses to shape
// into one.
[[nodiscard]] inline bool stairCanTakeShape(const World& world, BlockPos pos, BlockState state,
                                            BlockOrientation neighbourDirection) {
    const auto offset = orientationOffset(neighbourDirection);
    const BlockState neighbour = world.state(pos.x + offset.x, pos.y + offset.y, pos.z + offset.z);
    if (neighbour.block() != state.block()) {
        return true; // not a stair (or a different species): nothing to refuse
    }
    return neighbour.orientation() != state.orientation() || neighbour.stairHalf() != state.stairHalf();
}

} // namespace detail

// StairBlock#getStairsShape, read fresh off `world` rather than a single
// changed neighbour: a stair's shape depends on both of its facing-axis
// neighbours (behind and in front), and re-deriving from scratch is what makes
// repeated notification idempotent (the fixed-point guard in
// applyUpdateShapeContract already assumes the derivation always converges to
// the same answer for the same world) and lets placement compute the join
// immediately (StairBlock#getStateForPlacement calls this the moment the stair
// is placed, not waiting for the first neighbour notification).
[[nodiscard]] inline StairShape stairShapeFor(const World& world, BlockPos pos, BlockState state) {
    const auto facing = state.orientation();

    // The cell "behind" the stair (the direction it faces away from, i.e. the
    // low step's side): a matching stair there whose own facing turns off-axis
    // makes an OUTER corner, unless the far side already took that shape.
    const auto behindOffset = orientationOffset(facing);
    const BlockState behind =
        world.state(pos.x + behindOffset.x, pos.y + behindOffset.y, pos.z + behindOffset.z);
    if (behind.block() == state.block() && behind.stairHalf() == state.stairHalf()) {
        const auto behindFacing = behind.orientation();
        if (!sameHorizontalAxis(behindFacing, facing) &&
            detail::stairCanTakeShape(world, pos, state, oppositeOrientation(behindFacing))) {
            return behindFacing == counterClockwiseOrientation(facing) ? StairShape::OuterLeft
                                                                        : StairShape::OuterRight;
        }
    }

    // The cell "in front" (the high step's side): a matching stair there whose
    // facing turns off-axis makes an INNER corner.
    const auto frontOffset = orientationOffset(oppositeOrientation(facing));
    const BlockState front =
        world.state(pos.x + frontOffset.x, pos.y + frontOffset.y, pos.z + frontOffset.z);
    if (front.block() == state.block() && front.stairHalf() == state.stairHalf()) {
        const auto frontFacing = front.orientation();
        if (!sameHorizontalAxis(frontFacing, facing) &&
            detail::stairCanTakeShape(world, pos, state, frontFacing)) {
            return frontFacing == counterClockwiseOrientation(facing) ? StairShape::InnerLeft
                                                                       : StairShape::InnerRight;
        }
    }

    return StairShape::Straight;
}

// The updateShape slot for BlockModel::Stairs. Only a horizontal neighbour can
// change a stair's join (a block placed above/below never affects it), which
// is the vertical-axis short-circuit StairBlock#updateShape itself makes
// (`directionToNeighbour.getAxis().isHorizontal()`), ported here as the same
// early "unchanged" answer. Takes plain fields rather than gameplay's
// NeighborUpdateContext, so this header stays independent of the gameplay
// layer that adapts it (see the file comment).
[[nodiscard]] inline BlockState stairUpdateShape(const World& world, BlockPos pos, BlockState state,
                                                 BlockPos fromOffset) {
    const glm::ivec3 offset{fromOffset.x, fromOffset.y, fromOffset.z};
    if (isHorizontal(orientationFromOffset(offset))) {
        return state.withStairShape(stairShapeFor(world, pos, state));
    }
    return state; // vertical neighbour: unchanged (fixed point)
}

// DoorBlock#updateShape's *same-block* half. A door's two cells share one
// Facing/Open/Hinge and differ only in Half; when the vertical neighbour is
// the door's other half and that half's own Half differs from this cell's
// (the ordinary, healthy case), this cell copies whichever property the other
// half might have drifted on — in this build the two halves are always written
// together (see the atomic two-cell write in PlayerInteraction.cpp), so the
// derivation is a no-op convergence check, not a live sync path; it exists so
// A3b's contract still holds if a future edit ever touches one half alone
// (a command, a structure paste) — the *other* half self-heals to match on its
// own next shape pass rather than silently drifting.
[[nodiscard]] inline BlockState doorUpdateShape(BlockState state, BlockPos fromOffset,
                                                BlockState neighborState) {
    const glm::ivec3 offset{fromOffset.x, fromOffset.y, fromOffset.z};
    const auto direction = orientationFromOffset(offset);
    const bool verticalNeighbour = !isHorizontal(direction);
    const bool upperHalf = state.isDoorUpperHalf();
    // JE: `directionToNeighbour.getAxis() != Y || half==LOWER != (dir==UP)`
    // guards the *other* branch (a horizontal neighbour, or the vertical
    // neighbour that is NOT this half's own other-half direction) — that
    // branch is support/break territory (A3a), not a property rewrite, so it
    // is deliberately left to the ordinary onNeighborChanged/support pass
    // rather than answered here (A3b forbids this slot from destroying the
    // block). Only the true "my other half" direction reaches the sync below.
    const bool towardOtherHalf =
        verticalNeighbour && (upperHalf == (direction == BlockOrientation::Down));
    if (!towardOtherHalf) {
        return state;
    }
    if (neighborState.block() != state.block() || neighborState.isDoorUpperHalf() == upperHalf) {
        return state; // the other half is gone or malformed: not this slot's job
    }
    // Copy the shared axes from whichever half is asked; Half itself never
    // changes (each cell keeps its own).
    return state.with(neighborState.orientation())
        .withOpen(neighborState.open())
        .withHinge(neighborState.hinge());
}

} // namespace mc::world

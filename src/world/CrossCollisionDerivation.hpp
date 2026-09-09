#pragma once

// MDL-1: the CrossCollision family's four connection bits — the fence / iron
// bars / glass pane counterpart of WallShapeDerivation.hpp, and deliberately
// the same shape as it: one derivation function both the placement path
// (BlockPlacement.cpp, which needs the four bits the instant the block is
// placed, before any neighbour notification runs) and the gameplay updateShape
// slot (gameplay/BlockBehavior.hpp) call.
//
// Lives in world/ for the same reason WallShapeDerivation.hpp does:
// BlockPlacement.cpp is world-layer and world/ never depends on gameplay/.
// That is also why "which family does this block join" is a BlockDefinition
// field (ConnectFamily) rather than a BlockTags query — BlockTags is
// gameplay-layer, and a tag bit test would be both a layering violation and
// more work than one enum compare.
//
// Ported from 26.1:
//   FenceBlock#connectsTo:
//       !isExceptionForConnection(state) && faceSolid || sameFence || gate
//   FenceBlock#isSameFence:
//       state.is(FENCES) && state.is(WOODEN_FENCES) == this.is(WOODEN_FENCES)
//   IronBarsBlock#attachsTo:
//       !isExceptionForConnection(state) && faceSolid
//       || state.getBlock() instanceof IronBarsBlock || state.is(BlockTags.WALLS)
//   Block#isExceptionForConnection:
//       leaves | barrier | carved_pumpkin | jack_o_lantern | melon | pumpkin
// The asymmetry is vanilla's own: a pane attaches to a wall, a wall does not
// attach back to a pane, and a fence attaches to neither.

#include "world/Block.hpp"
#include "world/BlockPlacement.hpp" // orientationOffset
#include "world/BlockPos.hpp"
#include "world/BlockState.hpp"
#include "world/WallShapeDerivation.hpp" // detail::kWallHorizontals
#include "world/World.hpp"

namespace mc::world {

namespace detail {

// One side's connection answer. `towardNeighbor` is the direction pointing from
// the block being derived at its neighbour, which is what the fence-gate axis
// test needs (a gate facing north/south connects to a fence approaching from
// east/west).
[[nodiscard]] inline bool crossConnectsTo(const World& world, const BlockDefinition& self,
                                          BlockPos neighborPos,
                                          BlockOrientation towardNeighbor) {
    const auto neighborBlock = world.block(neighborPos.x, neighborPos.y, neighborPos.z);
    const auto& neighbor = blockDefinition(neighborBlock);
    // Same family — a wooden fence joins wooden fences, a pane joins panes and
    // iron bars. This is `isSameFence` / `instanceof IronBarsBlock`.
    if (neighbor.connectFamily != ConnectFamily::None &&
        neighbor.connectFamily == self.connectFamily) {
        return true;
    }
    // `attachsTo`'s third clause: a pane (and iron bars) also joins a wall.
    // A fence does not, and a wall does not join back — both are vanilla's.
    if (self.connectFamily == ConnectFamily::PaneOrBars && neighbor.model == BlockModel::Wall) {
        return true;
    }
    // A fence joins a fence gate whose axis runs across the connection, the same
    // FenceGateBlock#connectsToDirection rule wallConnectsTo already ports.
    if (self.connectFamily == ConnectFamily::WoodenFence &&
        neighbor.model == BlockModel::FenceGate) {
        const auto neighborState = world.state(neighborPos.x, neighborPos.y, neighborPos.z);
        return !sameHorizontalAxis(neighborState.orientation(), towardNeighbor);
    }
    // Otherwise: a sturdy face, unless this block is one of the six vanilla
    // refuses to attach to however solid it looks.
    if (neighbor.exceptionForConnection) {
        return false;
    }
    return isFaceSturdy(neighborBlock);
}

} // namespace detail

// The four connection bits, re-derived fresh from the world. Reading every side
// rather than patching the one that changed is what makes repeated notification
// a fixed point — the same convergent shape wallConnectionsFor takes.
[[nodiscard]] inline BlockState crossConnectionsFor(const World& world, BlockPos pos,
                                                    BlockState state) {
    const auto& self = blockDefinition(state.block());
    BlockState result = state;
    for (const auto side : detail::kWallHorizontals) {
        const auto offset = orientationOffset(side);
        const BlockPos neighborPos{pos.x + offset.x, pos.y + offset.y, pos.z + offset.z};
        result = result.withWallConnected(side,
                                          detail::crossConnectsTo(world, self, neighborPos, side));
    }
    return result;
}

// The updateShape slot for BlockModel::CrossCollision. Vanilla's
// CrossCollisionBlock#updateShape only reacts to a horizontal neighbour
// (`direction.getAxis().isHorizontal()`); a vertical one leaves the state
// alone, which is both vanilla's branch and the fixed point this contract needs.
[[nodiscard]] inline BlockState crossUpdateShape(const World& world, BlockPos pos, BlockState state,
                                                 BlockPos fromOffset) {
    const glm::ivec3 offset{fromOffset.x, fromOffset.y, fromOffset.z};
    if (isHorizontal(orientationFromOffset(offset))) {
        return crossConnectionsFor(world, pos, state);
    }
    return state;
}

} // namespace mc::world

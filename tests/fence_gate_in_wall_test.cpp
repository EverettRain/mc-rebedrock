// AR-B4-4 part A: FenceGateBlock's IN_WALL.
//
//   updateShape:88-95           reacts only when the changed neighbour lies on
//                               FACING.getClockWise().getAxis() — the axis
//                               running *across* the gate — and then reads both
//                               that side and its opposite.
//   getStateForPlacement:137-138 the same answer, computed the instant the gate
//                               lands rather than left to a notification that
//                               may never come if the walls were already there.
//
// isWall is vanilla's `state.is(BlockTags.WALLS)`: only a wall counts, never a
// fence, which is what keeps this independent of the fence family this build
// does not have. The 13px visual variant the property drives is RN-10c's; this
// node only writes the property.

#include "gameplay/BlockBehavior.hpp"
#include "world/Block.hpp"
#include "world/BlockPlacement.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/WallShapeDerivation.hpp"
#include "world/World.hpp"

#include <cassert>
#include <utility>

namespace {

using mc::world::Block;
using mc::world::BlockOrientation;
using mc::world::BlockPos;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::World;

[[nodiscard]] World floored() {
    World world;
    Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, Block::Stone);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

} // namespace

int main() {
    const BlockPos gatePos{8, 1, 8};

    // --- The axis. A north-facing gate is walled by its west/east neighbours
    // (FACING.getClockWise() is East, axis X) and is indifferent to north/south. ---
    {
        World world = floored();
        const BlockState northGate{Block::OakFenceGate, BlockOrientation::North};
        world.setState(gatePos.x, gatePos.y, gatePos.z, northGate);
        assert(!mc::world::fenceGateInWallFor(world, gatePos, northGate));

        // A wall on the gate's own axis does nothing.
        world.setState(gatePos.x, gatePos.y, gatePos.z - 1,
                       BlockState{Block::CobblestoneWall});
        assert(!mc::world::fenceGateInWallFor(world, gatePos, northGate));

        // One across the axis does.
        world.setState(gatePos.x - 1, gatePos.y, gatePos.z, BlockState{Block::CobblestoneWall});
        assert(mc::world::fenceGateInWallFor(world, gatePos, northGate));

        // And so does the opposite side alone — vanilla reads both, so either
        // one is enough and neither is privileged.
        world.setState(gatePos.x - 1, gatePos.y, gatePos.z, BlockState{Block::Air});
        assert(!mc::world::fenceGateInWallFor(world, gatePos, northGate));
        world.setState(gatePos.x + 1, gatePos.y, gatePos.z, BlockState{Block::CobblestoneWall});
        assert(mc::world::fenceGateInWallFor(world, gatePos, northGate));

        // An east-facing gate reads the *other* axis. Clear the north wall left
        // over from the first check so only the east one remains: that walls the
        // north-facing gate and leaves the east-facing one bare, which is the
        // whole point of the axis rule.
        world.setState(gatePos.x, gatePos.y, gatePos.z - 1, BlockState{Block::Air});
        const BlockState eastGate{Block::OakFenceGate, BlockOrientation::East};
        assert(mc::world::fenceGateInWallFor(world, gatePos, northGate));
        assert(!mc::world::fenceGateInWallFor(world, gatePos, eastGate));
        // ...and putting a wall back on the east gate's own axis does reach it.
        world.setState(gatePos.x, gatePos.y, gatePos.z + 1, BlockState{Block::CobblestoneWall});
        assert(mc::world::fenceGateInWallFor(world, gatePos, eastGate));
        world.setState(gatePos.x, gatePos.y, gatePos.z + 1, BlockState{Block::Air});

        // Only walls count. A full cube on the same side is not a wall.
        world.setState(gatePos.x + 1, gatePos.y, gatePos.z, BlockState{Block::Stone});
        assert(!mc::world::fenceGateInWallFor(world, gatePos, northGate));
    }

    // --- updateShape: the axis gate, and idempotent convergence. Repeated
    // notification from either side lands on the same state, and a notification
    // on an irrelevant axis (including vertical) is a fixed point. ---
    {
        World world = floored();
        const BlockState gate{Block::OakFenceGate, BlockOrientation::North};
        world.setState(gatePos.x, gatePos.y, gatePos.z, gate);
        world.setState(gatePos.x - 1, gatePos.y, gatePos.z, BlockState{Block::CobblestoneWall});

        const BlockPos fromWest{-1, 0, 0};
        const BlockPos fromEast{1, 0, 0};
        const BlockPos fromNorth{0, 0, -1};
        const BlockPos fromAbove{0, 1, 0};

        const auto walled = mc::world::fenceGateUpdateShape(world, gatePos, gate, fromWest);
        assert(walled.inWall());
        // Idempotent: re-running from the same side, and from the opposite side,
        // both reproduce it exactly.
        assert(mc::world::fenceGateUpdateShape(world, gatePos, walled, fromWest) == walled);
        assert(mc::world::fenceGateUpdateShape(world, gatePos, walled, fromEast) == walled);
        // Off-axis notifications leave the state alone — vanilla's `!= axis ->
        // super` branch, and the fixed point the updateShape contract needs.
        assert(mc::world::fenceGateUpdateShape(world, gatePos, walled, fromNorth) == walled);
        assert(mc::world::fenceGateUpdateShape(world, gatePos, walled, fromAbove) == walled);
        // ...including when the state is stale: an off-axis poke must not
        // silently correct it either, exactly as vanilla does not.
        const auto stale = gate.withInWall(false);
        assert(mc::world::fenceGateUpdateShape(world, gatePos, stale, fromNorth) == stale);

        // Removing the wall converges back.
        world.setState(gatePos.x - 1, gatePos.y, gatePos.z, BlockState{Block::Air});
        const auto bare = mc::world::fenceGateUpdateShape(world, gatePos, walled, fromWest);
        assert(!bare.inWall());
        assert(mc::world::fenceGateUpdateShape(world, gatePos, bare, fromWest) == bare);
    }

    // --- The behaviour table wires the slot, and the property never leaks onto
    // the blocks that share the gate's neighbours. ---
    {
        const auto& behavior =
            mc::gameplay::behaviorFor(mc::world::blockId(Block::OakFenceGate));
        assert(behavior.updateShape != nullptr);
        assert(behavior.prefilter.has(mc::gameplay::BlockBehaviorBit::HasNeighborReaction));
    }

    // --- Placement: the gate lands already knowing, so a gate slotted into a
    // finished wall does not need a later notification to look right. ---
    {
        World world = floored();
        world.setState(7, 1, 8, BlockState{Block::CobblestoneWall});
        world.setState(9, 1, 8, BlockState{Block::CobblestoneWall});
        mc::world::PlacementContext context;
        context.placePosition = {8, 1, 8};
        context.clickedBlock = {8, 0, 8};
        context.clickedFace = BlockOrientation::Up;
        context.hitPosition = {8.5F, 1.0F, 8.5F};
        context.lookDirection = {0.0F, 0.0F, 1.0F}; // faces the gate north/south
        const auto placed = mc::world::placementBlock(world, Block::OakFenceGate, context);
        assert(placed.has_value());
        assert(placed->block() == Block::OakFenceGate);
        // The facing this look direction produces runs along Z, so the walls to
        // the west and east are the ones across its axis.
        assert(mc::world::sameHorizontalAxis(placed->orientation(), BlockOrientation::North));
        assert(placed->inWall());

        // The same placement with no walls lands bare.
        World bareWorld = floored();
        const auto bare = mc::world::placementBlock(bareWorld, Block::OakFenceGate, context);
        assert(bare.has_value() && !bare->inWall());
    }

    return 0;
}

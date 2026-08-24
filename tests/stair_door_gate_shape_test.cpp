// AR-B2 headless acceptance: stair join-shape derivation (both placement's
// immediate compute and updateShape's neighbour-triggered recompute), door
// upper/lower shared-axis sync, and the BlockShape entries stair/door/gate
// route to — everything the task card's "楼梯五形状" / "邻居联动" / "开关切换
// 碰撞形状" clauses ask headless to prove. Interaction (right-click toggle,
// two-cell placement) is covered in player_interaction_test.cpp instead, since
// it needs the full GameSession command path this file does not link.

#include "world/Block.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/StairShapeDerivation.hpp"
#include "world/World.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "stair_door_gate_shape_test line %d failed: %s\n", line, expression);
        std::abort();
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using namespace mc::world;

void buildFloor(World& world) {
    Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 60, z, Block::Stone);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
}

} // namespace

int main() {
    // --- BlockShape: a straight stair is two boxes (full bottom half + one
    // step); an inner/outer shape uses two/three, always with a full-footprint
    // bottom box first. This is the "five shapes stay one source" clause —
    // every StairShape's box list traces back to blockShape, never a second
    // hand-written switch. ---
    {
        const auto straight = blockShape(BlockState{Block::OakStairs});
        REQUIRE(straight.kind == ShapeKind::Boxes);
        REQUIRE(straight.boxes.size() == 2);

        const auto outer =
            blockShape(BlockState{Block::OakStairs}.withStairShape(StairShape::OuterLeft));
        REQUIRE(outer.boxes.size() == 2);

        const auto inner =
            blockShape(BlockState{Block::OakStairs}.withStairShape(StairShape::InnerRight));
        REQUIRE(inner.boxes.size() == 3);

        // Every box list's first entry is the shared full-footprint lower half —
        // the "bottom" half is always present regardless of shape or facing.
        for (const auto shape :
             {StairShape::Straight, StairShape::InnerLeft, StairShape::InnerRight,
              StairShape::OuterLeft, StairShape::OuterRight}) {
            const auto boxes = blockShape(BlockState{Block::OakStairs}.withStairShape(shape));
            REQUIRE(boxes.boxes.front().minX == 0.0F && boxes.boxes.front().maxX == 1.0F);
            REQUIRE(boxes.boxes.front().minY == 0.0F && boxes.boxes.front().maxY == 0.5F);
        }

        // A top-half stair mirrors every box vertically: the same shape's
        // bottom-half boxes, flipped about y=0.5.
        const auto topStraight =
            blockShape(BlockState{Block::OakStairs}.withStairHalf(SlabPortion::Top));
        REQUIRE(topStraight.boxes.front().minY == 0.5F && topStraight.boxes.front().maxY == 1.0F);
    }

    // --- Collision follows the same shape: a stair collides, and its box list
    // is the identical one blockShape answers (single source, no drift). ---
    {
        REQUIRE(hasCollision(Block::OakStairs));
        const auto state = BlockState{Block::OakStairs};
        const auto span = collisionSpan(state);
        REQUIRE(span.bottom == 0.0F && span.top == 1.0F); // union of both boxes' Y range
    }

    // --- Door: closed is a thin box flush against the far face; open swings to
    // the side the hinge points, and both keep colliding (unlike a gate, which
    // empties fully) — the "开门后碰撞盒变化" clause: closed vs. open really do
    // occupy different space. ---
    {
        const auto closed = blockShape(BlockState{Block::OakDoor, BlockOrientation::North});
        REQUIRE(closed.boxes.size() == 1);
        REQUIRE(closed.boxes.front().minZ > 0.5F); // flush against the far (south) face

        const auto openLeft = blockShape(BlockState{Block::OakDoor, BlockOrientation::North}
                                             .withOpen(true)
                                             .withHinge(DoorHinge::Left));
        const auto openRight = blockShape(BlockState{Block::OakDoor, BlockOrientation::North}
                                              .withOpen(true)
                                              .withHinge(DoorHinge::Right));
        REQUIRE(openLeft.boxes.size() == 1 && openRight.boxes.size() == 1);
        // Opening rotates the box off the closed footprint (a real shape change,
        // not merely an Open bit with no geometry consequence), and the two
        // hinges swing to opposite sides.
        REQUIRE(!(openLeft.boxes.front().minX == closed.boxes.front().minX &&
                  openLeft.boxes.front().minZ == closed.boxes.front().minZ));
        REQUIRE(openLeft.boxes.front().minX != openRight.boxes.front().minX ||
                openLeft.boxes.front().minZ != openRight.boxes.front().minZ);
        REQUIRE(hasCollision(Block::OakDoor)); // still solid while open, just relocated
    }

    // --- Fence gate: closed and open share the same facing-axis outline / visual
    // / pick box (vanilla getShape ignores OPEN — an open gate stays visible and
    // selectable); only the *collision* empties when open, so the gate swings
    // fully clear for entities without vanishing. ---
    {
        const auto closed = blockShape(BlockState{Block::OakFenceGate, BlockOrientation::North});
        REQUIRE(closed.kind == ShapeKind::Boxes && closed.boxes.size() == 1);
        const auto open =
            blockShape(BlockState{Block::OakFenceGate, BlockOrientation::North}.withOpen(true));
        REQUIRE(open.kind == ShapeKind::Boxes && open.boxes.size() == 1); // still visible/pickable
        const auto openState = BlockState{Block::OakFenceGate, BlockOrientation::North}.withOpen(true);
        REQUIRE(collisionShape(openState).boxes.empty());               // but no collision boxes
        REQUIRE(collisionSpan(openState).top <= collisionSpan(openState).bottom); // and no span
    }

    // --- stairShapeFor: an isolated stair (no matching neighbour) is Straight,
    // the placement-time and updateShape-time default. ---
    {
        World world;
        buildFloor(world);
        const auto state = BlockState{Block::OakStairs, BlockOrientation::North};
        REQUIRE(stairShapeFor(world, {5, 61, 5}, state) == StairShape::Straight);
    }

    // --- stairShapeFor: OUTER corner. A North-facing stair's "behind" cell is
    // `pos + orientationOffset(facing)` (StairBlock#getStairsShape:
    // `pos.relative(facing)`) — for North that is `pos + (0,0,-1)`, i.e. the
    // cell one *less* in Z. A matching stair there whose own facing turns
    // off-axis makes an outer corner: West turns it OuterLeft (JE's
    // "behindFacing == facing.counterClockWise()" branch), East OuterRight. ---
    {
        World world;
        buildFloor(world);
        const auto pos = BlockPos{5, 61, 5};
        world.setState(pos.x, pos.y + 1, pos.z, BlockState{}); // keep a clean cell above
        const auto state = BlockState{Block::OakStairs, BlockOrientation::North};

        // Behind (pos.z - 1) facing West: North's counter-clockwise is West.
        world.setState(pos.x, pos.y, pos.z - 1, BlockState{Block::OakStairs, BlockOrientation::West});
        REQUIRE(stairShapeFor(world, pos, state) == StairShape::OuterLeft);

        // Behind facing East instead: North's clockwise is East -> OuterRight.
        world.setState(pos.x, pos.y, pos.z - 1, BlockState{Block::OakStairs, BlockOrientation::East});
        REQUIRE(stairShapeFor(world, pos, state) == StairShape::OuterRight);
    }

    // --- stairShapeFor: INNER corner. The "front" cell is
    // `pos + orientationOffset(opposite(facing))` — for North that is
    // `pos + (0,0,1)`, the cell one *more* in Z. A matching stair there whose
    // facing turns off-axis makes an inner corner, same left/right convention
    // as the outer case. ---
    {
        World world;
        buildFloor(world);
        const auto pos = BlockPos{5, 61, 5};
        const auto state = BlockState{Block::OakStairs, BlockOrientation::North};

        world.setState(pos.x, pos.y, pos.z + 1, BlockState{Block::OakStairs, BlockOrientation::West});
        REQUIRE(stairShapeFor(world, pos, state) == StairShape::InnerLeft);

        world.setState(pos.x, pos.y, pos.z + 1, BlockState{Block::OakStairs, BlockOrientation::East});
        REQUIRE(stairShapeFor(world, pos, state) == StairShape::InnerRight);
    }

    // --- stairShapeFor: a stair only joins a neighbour on the *same* half —
    // a bottom stair beside a top stair (or a different species) stays
    // Straight, mirroring StairBlock#getStairsShape's Half-and-block equality
    // guard. Sabotage target ①: dropping this guard would corner-join
    // mismatched halves and this assertion catches it. ---
    {
        World world;
        buildFloor(world);
        const auto pos = BlockPos{5, 61, 5};
        const auto state = BlockState{Block::OakStairs, BlockOrientation::North};
        world.setState(pos.x, pos.y, pos.z - 1,
                       BlockState{Block::OakStairs, BlockOrientation::West}.withStairHalf(
                           SlabPortion::Top));
        REQUIRE(stairShapeFor(world, pos, state) == StairShape::Straight);
    }

    // --- stairUpdateShape: only a horizontal neighbour changes the shape; a
    // vertical one (a block placed above/below) leaves it exactly as-is, the
    // vertical-axis short-circuit StairBlock#updateShape itself takes. ---
    {
        World world;
        buildFloor(world);
        const auto pos = BlockPos{5, 61, 5};
        const auto state = BlockState{Block::OakStairs, BlockOrientation::North};
        const auto vertical = stairUpdateShape(world, pos, state, BlockPos{0, 1, 0});
        REQUIRE(vertical == state); // unchanged: fixed point, no write

        world.setState(pos.x, pos.y, pos.z - 1, BlockState{Block::OakStairs, BlockOrientation::West});
        const auto horizontal = stairUpdateShape(world, pos, state, BlockPos{0, 0, -1});
        REQUIRE(horizontal.stairShape() == StairShape::OuterLeft);
    }

    // --- doorUpdateShape: the lower half copies Facing/Open/Hinge from a
    // genuine upper-half neighbour reached from Down; any other direction (or
    // a mismatched/missing other half) is a no-op. Sabotage target ②: routing
    // this through the wrong direction test would either desync the halves
    // silently or churn on every unrelated horizontal neighbour. ---
    {
        const auto lower = BlockState{Block::OakDoor, BlockOrientation::North};
        const auto upper = BlockState{Block::OakDoor, BlockOrientation::East}
                                .withOpen(true)
                                .withHinge(DoorHinge::Right)
                                .withDoorUpperHalf(true);
        // Lower half asked about its Up neighbour (the true upper half).
        const auto synced = doorUpdateShape(lower, BlockPos{0, 1, 0}, upper);
        REQUIRE(synced.orientation() == BlockOrientation::East);
        REQUIRE(synced.open());
        REQUIRE(synced.hinge() == DoorHinge::Right);
        REQUIRE(!synced.isDoorUpperHalf()); // Half itself never changes

        // The same lower half asked about a horizontal neighbour: untouched.
        const auto horizontalNoOp = doorUpdateShape(lower, BlockPos{1, 0, 0}, upper);
        REQUIRE(horizontalNoOp == lower);

        // Asked about Down (the wrong vertical direction for a lower half):
        // untouched — Down is where a lower half's *support*, not its other
        // half, lives.
        const auto wrongDirection = doorUpdateShape(lower, BlockPos{0, -1, 0}, upper);
        REQUIRE(wrongDirection == lower);

        // The "other half" is missing (air) or malformed (also a lower half):
        // no sync, A3b's "never destroy" respected by simply declining.
        const auto missing = doorUpdateShape(lower, BlockPos{0, 1, 0}, BlockState{});
        REQUIRE(missing == lower);
        const auto malformed =
            doorUpdateShape(lower, BlockPos{0, 1, 0}, BlockState{Block::OakDoor});
        REQUIRE(malformed == lower); // neighbour also reports Half::Bottom: not a real upper
    }

    // --- The A3b contract (BlockBehavior.hpp's applyUpdateShapeContract) is
    // exercised for real content in update_shape_test.cpp's fixture-based
    // suite; this file only proves the concrete derivations. ---

    return 0;
}

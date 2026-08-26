// AR-B3 headless acceptance: trapdoor open/close + half shape, button press
// shape, pressure-plate raised/pressed column, and wall connection shape/
// placement-time-vs-updateShape parity — the shape half of the task card's
// "活板门开合+顶底 / 按钮按下 / 压力板踩踏 / 墙连接形状" quartet. Interaction
// (right-click toggle, button press+release timer, plate tread detection) is
// covered in player_interaction_test.cpp instead, the same split
// stair_door_gate_shape_test.cpp already draws for AR-B2.

#include "world/Block.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/WallShapeDerivation.hpp"
#include "world/World.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "trapdoor_button_plate_wall_shape_test line %d failed: %s\n", line,
                    expression);
        std::abort();
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using namespace mc::world;

} // namespace

int main() {
    // --- Trapdoor: closed lies flat (bottom or top face), open swings to a
    // thin vertical leaf on the Facing side — the "顶/底 half + 开合" clause.
    // Sabotage target: an open trapdoor that never moves off the flat closed
    // box would fail the "genuinely different box" assertions below. ---
    {
        const auto closedBottom =
            blockShape(BlockState{Block::OakTrapdoor, BlockOrientation::North});
        REQUIRE(closedBottom.kind == ShapeKind::Boxes);
        REQUIRE(closedBottom.boxes.size() == 1);
        REQUIRE(closedBottom.boxes.front().minY == 0.0F);
        REQUIRE(closedBottom.boxes.front().maxY < 0.25F); // thin slab near the floor

        const auto closedTop = blockShape(BlockState{Block::OakTrapdoor, BlockOrientation::North}
                                              .withTrapdoorHalf(SlabPortion::Top));
        REQUIRE(closedTop.boxes.front().minY > 0.75F); // thin slab near the ceiling
        REQUIRE(closedTop.boxes.front().maxY == 1.0F);

        const auto open = blockShape(BlockState{Block::OakTrapdoor, BlockOrientation::North}
                                          .withOpen(true));
        REQUIRE(open.boxes.size() == 1);
        // Open swings to a vertical leaf flush against the facing side — its Y
        // span covers the whole cell, unlike either closed box's thin slice.
        REQUIRE(open.boxes.front().minY == 0.0F && open.boxes.front().maxY == 1.0F);
        REQUIRE(hasCollision(Block::OakTrapdoor)); // still solid, just relocated

        // Half has no effect once open (TrapDoorBlock#getShape keys only on
        // Facing when OPEN) — both halves' open box agree.
        const auto openFromTop = blockShape(BlockState{Block::OakTrapdoor, BlockOrientation::North}
                                                .withTrapdoorHalf(SlabPortion::Top)
                                                .withOpen(true));
        REQUIRE(open.boxes.front().minX == openFromTop.boxes.front().minX &&
               open.boxes.front().minZ == openFromTop.boxes.front().minZ);

        // Different Facing values open toward different sides.
        const auto openEast = blockShape(BlockState{Block::OakTrapdoor, BlockOrientation::East}
                                             .withOpen(true));
        REQUIRE(!(open.boxes.front().minX == openEast.boxes.front().minX &&
                  open.boxes.front().minZ == openEast.boxes.front().minZ));
    }

    // --- Button: a small box on the Facing axis, shrinking slightly while
    // Powered — the "按下→短碰撞盒变化" half of the button clause (the timed
    // release itself is player_interaction_test.cpp's job). ---
    {
        const auto unpressed = blockShape(BlockState{Block::StoneButton, BlockOrientation::North});
        REQUIRE(unpressed.kind == ShapeKind::Boxes);
        REQUIRE(unpressed.boxes.size() == 1);
        REQUIRE(hasCollision(Block::StoneButton));

        const auto pressed =
            blockShape(BlockState{Block::StoneButton, BlockOrientation::North}.withPowered(true));
        REQUIRE(pressed.boxes.size() == 1);
        // Pressed protrudes less off the wall than unpressed — a real,
        // observable shape change, not merely a bit with no geometry
        // consequence (the same lesson AR-B2's door-vs-gate distinction
        // already encodes).
        REQUIRE(pressed.boxes.front().minZ != unpressed.boxes.front().minZ);

        // Different Facing values sit against different walls.
        const auto east = blockShape(BlockState{Block::StoneButton, BlockOrientation::East});
        REQUIRE(!(unpressed.boxes.front().minX == east.boxes.front().minX &&
                  unpressed.boxes.front().minZ == east.boxes.front().minZ));
    }

    // --- Pressure plate: a Column shape (full 1x1 footprint), lower while
    // Powered — the "踩踏触发→形状变化" clause, and the reason a plate never
    // needs a Boxes table at all (it is a slab-shaped height difference, the
    // same move a slab's SlabType already makes). ---
    {
        const auto raised = blockShape(BlockState{Block::StonePressurePlate});
        REQUIRE(raised.kind == ShapeKind::Column);
        REQUIRE(raised.bottom == 0.0F);
        REQUIRE(raised.top > 0.0F);

        const auto pressed =
            blockShape(BlockState{Block::StonePressurePlate}.withPowered(true));
        REQUIRE(pressed.kind == ShapeKind::Column);
        REQUIRE(pressed.top < raised.top); // genuinely shorter while pressed
        // BasePressurePlateBlock#getCollisionShape is Shapes.empty(): the raised
        // Column is an outline/visual shape only, so the plate never lifts or
        // blocks an entity (the fix for the step-on bounce). collisionSpan
        // therefore filters to an empty span even though blockShape stays a
        // Column above.
        REQUIRE(!hasCollision(Block::StonePressurePlate));
        const auto plateCollision = collisionSpan(BlockState{Block::StonePressurePlate});
        REQUIRE(plateCollision.top <= plateCollision.bottom); // empty span
        const auto pressedCollision =
            collisionSpan(BlockState{Block::StonePressurePlate}.withPowered(true));
        REQUIRE(pressedCollision.top <= pressedCollision.bottom); // empty in both states
    }

    // --- Wall: connection mask -> box-set table. No connections is post-only
    // (1 box); every connection present is post + 4 arms (5 boxes) — the
    // "邻接连接形状" clause. Sabotage target ③: a wrong-direction connection
    // read would produce the wrong box count/position for a given mask. ---
    {
        const auto isolated = blockShape(BlockState{Block::CobblestoneWall});
        REQUIRE(isolated.kind == ShapeKind::Boxes);
        REQUIRE(isolated.boxes.size() == 1); // post only

        const auto north = blockShape(
            BlockState{Block::CobblestoneWall}.withWallConnected(BlockOrientation::North, true));
        REQUIRE(north.boxes.size() == 2); // post + one arm

        const auto allFour =
            blockShape(BlockState{Block::CobblestoneWall}
                          .withWallConnected(BlockOrientation::North, true)
                          .withWallConnected(BlockOrientation::East, true)
                          .withWallConnected(BlockOrientation::South, true)
                          .withWallConnected(BlockOrientation::West, true));
        REQUIRE(allFour.boxes.size() == 5); // post + 4 arms
        REQUIRE(hasCollision(Block::CobblestoneWall));

        // North-only and East-only connections produce boxes at different XZ
        // positions — proof the four axes are not aliases of one another.
        const auto east = blockShape(
            BlockState{Block::CobblestoneWall}.withWallConnected(BlockOrientation::East, true));
        bool anyDiffers = false;
        for (std::size_t i = 0; i < north.boxes.size(); ++i) {
            if (i >= east.boxes.size()) break;
            if (north.boxes[i].minX != east.boxes[i].minX ||
                north.boxes[i].minZ != east.boxes[i].minZ) {
                anyDiffers = true;
            }
        }
        REQUIRE(anyDiffers);
    }

    // --- wallConnectionsFor: connects to a sturdy full-cube neighbour, a
    // second wall, and (on the perpendicular axis only) a fence gate — the
    // WallBlock#connectsTo port. Does NOT connect to a plain non-sturdy block
    // (e.g. a torch) or to nothing (air). ---
    {
        World world;
        Chunk chunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                chunk.setBlock(x, 60, z, Block::Stone);
            }
        }
        world.setChunk({0, 0}, std::move(chunk));
        const BlockPos pos{5, 61, 5};

        // No neighbours: isolated, post only.
        {
            const auto connected =
                wallConnectionsFor(world, pos, BlockState{Block::CobblestoneWall});
            REQUIRE(!connected.wallConnected(BlockOrientation::North));
            REQUIRE(!connected.wallConnected(BlockOrientation::East));
            REQUIRE(!connected.wallConnected(BlockOrientation::South));
            REQUIRE(!connected.wallConnected(BlockOrientation::West));
        }

        // A sturdy full-cube neighbour to the north: connects north only.
        world.setState(pos.x, pos.y, pos.z - 1, BlockState{Block::Stone});
        {
            const auto connected =
                wallConnectionsFor(world, pos, BlockState{Block::CobblestoneWall});
            REQUIRE(connected.wallConnected(BlockOrientation::North));
            REQUIRE(!connected.wallConnected(BlockOrientation::East));
            REQUIRE(!connected.wallConnected(BlockOrientation::South));
            REQUIRE(!connected.wallConnected(BlockOrientation::West));
        }
        world.setState(pos.x, pos.y, pos.z - 1, BlockState{});

        // Another wall to the east: connects east.
        world.setState(pos.x + 1, pos.y, pos.z, BlockState{Block::CobblestoneWall});
        {
            const auto connected =
                wallConnectionsFor(world, pos, BlockState{Block::CobblestoneWall});
            REQUIRE(connected.wallConnected(BlockOrientation::East));
            REQUIRE(!connected.wallConnected(BlockOrientation::North));
        }
        world.setState(pos.x + 1, pos.y, pos.z, BlockState{});

        // A non-sturdy decoration (a torch) to the south: does not connect —
        // this is sabotage target ③'s negative case, proving the rule is not
        // "any non-air neighbour".
        world.setState(pos.x, pos.y, pos.z + 1, BlockState{Block::Torch});
        {
            const auto connected =
                wallConnectionsFor(world, pos, BlockState{Block::CobblestoneWall});
            REQUIRE(!connected.wallConnected(BlockOrientation::South));
        }
        world.setState(pos.x, pos.y, pos.z + 1, BlockState{});

        // A fence gate to the west, facing North/South (its axis runs
        // perpendicular to the west-east connecting line): connects — the
        // "gate axis crosses the wall" rule (FenceGateBlock#connectsToDirection).
        world.setState(pos.x - 1, pos.y, pos.z,
                       BlockState{Block::OakFenceGate, BlockOrientation::North});
        {
            const auto connected =
                wallConnectionsFor(world, pos, BlockState{Block::CobblestoneWall});
            REQUIRE(connected.wallConnected(BlockOrientation::West));
        }
        // The same gate rotated to face East/West (its axis now runs *along*
        // the connecting line) does NOT connect.
        world.setState(pos.x - 1, pos.y, pos.z,
                       BlockState{Block::OakFenceGate, BlockOrientation::East});
        {
            const auto connected =
                wallConnectionsFor(world, pos, BlockState{Block::CobblestoneWall});
            REQUIRE(!connected.wallConnected(BlockOrientation::West));
        }
    }

    // --- wallUpdateShape: only a horizontal neighbour re-derives connections;
    // a vertical one is a fixed point (no UP-post logic carried in this pass,
    // see WallShapeDerivation.hpp's file comment). ---
    {
        World world;
        Chunk chunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                chunk.setBlock(x, 60, z, Block::Stone);
            }
        }
        world.setChunk({0, 0}, std::move(chunk));
        const BlockPos pos{5, 61, 5};
        const auto state = BlockState{Block::CobblestoneWall};

        const auto vertical = wallUpdateShape(world, pos, state, BlockPos{0, 1, 0});
        REQUIRE(vertical == state); // unchanged: fixed point, no write

        world.setState(pos.x, pos.y, pos.z - 1, BlockState{Block::Stone});
        const auto horizontal = wallUpdateShape(world, pos, state, BlockPos{0, 0, -1});
        REQUIRE(horizontal.wallConnected(BlockOrientation::North));
    }

    return 0;
}

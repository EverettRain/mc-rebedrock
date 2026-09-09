// MDL-1/MDL-2: the CrossCollision family (fence / iron bars / glass pane) and
// the carpet.
//
// 26.1 makes fences, iron bars and glass panes one parameterised class —
// CrossCollisionBlock(postWidth, postHeight, wallWidth, wallHeight,
// collisionHeight) plus four connection booleans and waterlogged — so this build
// makes them one BlockModel plus a two-row parameter table. What this file
// checks is precisely the things that go wrong when a family is folded that way:
//
//   1. the two parameter rows do not bleed into each other (a pane must not
//      inherit the fence's 1.5-cell collision, which is the difference between
//      "a fence pens animals" and "a pane does something absurd");
//   2. the visual shape and the collision shape stay two tables, not one scaled
//      one (the AR-B4-1 lesson, restated for a whole family);
//   3. the four connection bits are derived per side and in the right order —
//      and the fixture is deliberately ASYMMETRIC, because a fixture with a
//      neighbour on every side (or none) passes even when the derivation writes
//      the sides in the wrong order ([[lesson-assertion-fixture-shape]]);
//   4. the family rules are vanilla's, including their asymmetry: a pane
//      attaches to a wall, a wall does not attach back to a pane, a fence
//      attaches to neither, and leaves are refused by everyone.

#include "world/Block.hpp"
#include "world/BlockPlacement.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/CrossCollisionDerivation.hpp"
#include "world/World.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <utility>

namespace {

using mc::world::Block;
using mc::world::BlockOrientation;
using mc::world::BlockPos;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::World;

[[nodiscard]] bool near(float a, float b) { return std::fabs(a - b) < 1.0e-6F; }

// A 16x16 stone floor at y = 0, everything above it air. Blocks under test go
// at y = 1 so the floor never participates in a horizontal connection.
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

constexpr BlockPos kCentre{8, 1, 8};

void putNeighbour(World& world, BlockOrientation side, Block block) {
    const auto offset = mc::world::orientationOffset(side);
    world.setBlock(kCentre.x + offset.x, kCentre.y + offset.y, kCentre.z + offset.z, block);
}

[[nodiscard]] BlockState derived(const World& world, Block self) {
    return mc::world::crossConnectionsFor(world, kCentre, BlockState{self});
}

// The one box a shape has when nothing is connected, or the arm box on `side`.
[[nodiscard]] std::size_t boxCount(const mc::world::BlockShape& shape) {
    return shape.boxes.size();
}

} // namespace

int main() {
    using mc::world::blockShape;
    using mc::world::collisionShape;

    // ---------------------------------------------------------------------
    // 1) The two parameter rows. 26.1: fence 4/16/4/16/24, bars+panes
    //    2/16/2/16/16. A post is `column(width, 0, height)`, centred.
    // ---------------------------------------------------------------------
    {
        const auto fence = blockShape(BlockState{Block::OakFence});
        assert(fence.kind == mc::world::ShapeKind::Boxes);
        assert(boxCount(fence) == 1U); // unconnected: post only
        const auto& post = fence.boxes[0];
        assert(near(post.minX, 6.0F / 16.0F) && near(post.maxX, 10.0F / 16.0F));
        assert(near(post.minZ, 6.0F / 16.0F) && near(post.maxZ, 10.0F / 16.0F));
        assert(near(post.minY, 0.0F) && near(post.maxY, 1.0F));

        const auto pane = blockShape(BlockState{Block::GlassPane});
        assert(boxCount(pane) == 1U);
        const auto& panePost = pane.boxes[0];
        // 2/16 wide, i.e. 7/16..9/16 — NOT the fence's 6/16..10/16.
        assert(near(panePost.minX, 7.0F / 16.0F) && near(panePost.maxX, 9.0F / 16.0F));
        assert(near(panePost.minZ, 7.0F / 16.0F) && near(panePost.maxZ, 9.0F / 16.0F));

        // Iron bars read the same row as the panes.
        const auto bars = blockShape(BlockState{Block::IronBars});
        assert(near(bars.boxes[0].minX, 7.0F / 16.0F));
    }

    // ---------------------------------------------------------------------
    // 2) Visual vs collision are two tables. The fence collides 1.5 cells tall
    //    (that is what makes a fence line unjumpable); the pane collides 1.
    // ---------------------------------------------------------------------
    {
        assert(near(blockShape(BlockState{Block::OakFence}).boxes[0].maxY, 1.0F));
        assert(near(collisionShape(BlockState{Block::OakFence}).boxes[0].maxY, 1.5F));
        assert(near(collisionShape(BlockState{Block::GlassPane}).boxes[0].maxY, 1.0F));
        assert(near(collisionShape(BlockState{Block::IronBars}).boxes[0].maxY, 1.0F));
        // And the prefilter the collision walk uses agrees with the shapes.
        assert(mc::world::hasTallCollision(Block::OakFence));
        assert(!mc::world::hasTallCollision(Block::GlassPane));
        assert(!mc::world::hasTallCollision(Block::IronBars));
    }

    // ---------------------------------------------------------------------
    // 3) Connection derivation on an ASYMMETRIC fixture: stone to the north and
    //    east, air to the south and west. A fixture with neighbours on every
    //    side would pass with the sides written in any order.
    // ---------------------------------------------------------------------
    {
        World world = floored();
        putNeighbour(world, BlockOrientation::North, Block::Stone);
        putNeighbour(world, BlockOrientation::East, Block::Stone);
        const auto state = derived(world, Block::OakFence);
        assert(state.wallConnected(BlockOrientation::North));
        assert(state.wallConnected(BlockOrientation::East));
        assert(!state.wallConnected(BlockOrientation::South));
        assert(!state.wallConnected(BlockOrientation::West));

        // The shape follows: post + exactly two arms, and the north arm reaches
        // the cell's north face while staying the post's width.
        const auto shape = blockShape(state);
        assert(boxCount(shape) == 3U);
        bool sawNorthArm = false;
        for (const auto& box : shape.boxes) {
            if (near(box.minZ, 0.0F) && near(box.maxZ, 0.5F)) {
                sawNorthArm = true;
                assert(near(box.minX, 6.0F / 16.0F) && near(box.maxX, 10.0F / 16.0F));
            }
        }
        assert(sawNorthArm);

        // Re-deriving is a fixed point, and so is the updateShape slot: repeated
        // neighbour notification must converge, never oscillate.
        const auto again = mc::world::crossConnectionsFor(world, kCentre, state);
        assert(again == state);
        const auto viaUpdate = mc::world::crossUpdateShape(
            world, kCentre, state, BlockPos{-1, 0, 0});
        assert(viaUpdate == state);
        // A vertical notification leaves the state alone (vanilla's `super`).
        assert(mc::world::crossUpdateShape(world, kCentre, state, BlockPos{0, 1, 0}) == state);
    }

    // ---------------------------------------------------------------------
    // 4) Family rules, including vanilla's asymmetry.
    // ---------------------------------------------------------------------
    {
        World world = floored();
        // A wooden fence joins another wooden fence...
        putNeighbour(world, BlockOrientation::North, Block::SpruceFence);
        // ...but not a glass pane (different family, and a pane has no sturdy face).
        putNeighbour(world, BlockOrientation::East, Block::GlassPane);
        // ...and not leaves, however solid they look (isExceptionForConnection).
        putNeighbour(world, BlockOrientation::South, Block::OakLeaves);
        // ...and not a wall (a fence never attaches to one in vanilla either).
        putNeighbour(world, BlockOrientation::West, Block::CobblestoneWall);
        const auto fence = derived(world, Block::OakFence);
        assert(fence.wallConnected(BlockOrientation::North));
        assert(!fence.wallConnected(BlockOrientation::East));
        assert(!fence.wallConnected(BlockOrientation::South));
        assert(!fence.wallConnected(BlockOrientation::West));

        // The pane's own rules: it joins panes and iron bars, and it joins a
        // wall — but the wall does not join back.
        World paneWorld = floored();
        putNeighbour(paneWorld, BlockOrientation::North, Block::IronBars);
        putNeighbour(paneWorld, BlockOrientation::East, Block::CobblestoneWall);
        putNeighbour(paneWorld, BlockOrientation::South, Block::OakFence);
        putNeighbour(paneWorld, BlockOrientation::West, Block::OakLeaves);
        const auto pane = derived(paneWorld, Block::GlassPane);
        assert(pane.wallConnected(BlockOrientation::North));
        assert(pane.wallConnected(BlockOrientation::East)); // pane -> wall
        assert(!pane.wallConnected(BlockOrientation::South));
        assert(!pane.wallConnected(BlockOrientation::West));
        // The wall standing where the pane was does NOT connect to a pane.
        World wallWorld = floored();
        putNeighbour(wallWorld, BlockOrientation::North, Block::GlassPane);
        const auto wall =
            mc::world::wallConnectionsFor(wallWorld, kCentre, BlockState{Block::CobblestoneWall});
        assert(!wall.wallConnected(BlockOrientation::North));

        // A stained pane is the same family as the plain one.
        World stained = floored();
        putNeighbour(stained, BlockOrientation::North, Block::GlassPane);
        assert(derived(stained, Block::RedStainedGlassPane).wallConnected(BlockOrientation::North));

        // Glass (the full cube) has a sturdy face, so a fence does attach to it —
        // vanilla's `faceSolid` half of the rule, and the reason the exception
        // list exists at all.
        World glass = floored();
        putNeighbour(glass, BlockOrientation::North, Block::Glass);
        assert(derived(glass, Block::OakFence).wallConnected(BlockOrientation::North));
    }

    // ---------------------------------------------------------------------
    // 5) A fence joins a fence gate whose axis runs ACROSS the connection, and
    //    not one whose axis runs along it (FenceGateBlock#connectsToDirection).
    // ---------------------------------------------------------------------
    {
        World world = floored();
        const auto north = mc::world::orientationOffset(BlockOrientation::North);
        // A gate facing east/west opens along x, so its posts face north/south:
        // it connects to a fence approaching from the north.
        world.setBlock(kCentre.x + north.x, kCentre.y, kCentre.z + north.z, Block::OakFenceGate);
        world.setState(kCentre.x + north.x, kCentre.y, kCentre.z + north.z,
                       BlockState{Block::OakFenceGate, BlockOrientation::East});
        assert(derived(world, Block::OakFence).wallConnected(BlockOrientation::North));
        // Turn the gate to face north: now its axis runs along the connection
        // and the fence does not join it.
        world.setState(kCentre.x + north.x, kCentre.y, kCentre.z + north.z,
                       BlockState{Block::OakFenceGate, BlockOrientation::North});
        assert(!derived(world, Block::OakFence).wallConnected(BlockOrientation::North));
    }

    // ---------------------------------------------------------------------
    // 6) Placement computes the four bits immediately — a fence must never be a
    //    bare post for one tick waiting on a neighbour notification.
    // ---------------------------------------------------------------------
    {
        World world = floored();
        putNeighbour(world, BlockOrientation::North, Block::Stone);
        mc::world::PlacementContext context{};
        context.placePosition = {kCentre.x, kCentre.y, kCentre.z};
        context.clickedFace = BlockOrientation::Up;
        context.lookDirection = {0.0F, 0.0F, -1.0F};
        const auto placed = mc::world::placementBlock(world, Block::OakFence, context);
        assert(placed.has_value());
        assert(placed->wallConnected(BlockOrientation::North));
        assert(!placed->wallConnected(BlockOrientation::South));
    }

    // ---------------------------------------------------------------------
    // 7) Waterlogging: the whole family declares the submerged axis (26.1's
    //    SimpleWaterloggedBlock through CrossCollisionBlock).
    // ---------------------------------------------------------------------
    {
        for (const Block block : {Block::OakFence, Block::GlassPane, Block::IronBars,
                                  Block::BlackStainedGlassPane}) {
            const BlockState dry{block};
            assert(dry.has(mc::world::StateProperty::SubmergedFluid));
            const auto wet = dry.withSubmergedFluid(mc::world::SubmergedFluid::Water);
            assert(wet.submergedFluid() == mc::world::SubmergedFluid::Water);
            // Submerging must not disturb the connection bits.
            assert(wet.block() == block);
        }
    }

    // ---------------------------------------------------------------------
    // 8) MDL-2: the carpet. `Block.column(16, 0, 1)` — a 1/16 slice — surviving
    //    on any non-air cell, not only a sturdy one.
    // ---------------------------------------------------------------------
    {
        const auto shape = blockShape(BlockState{Block::WhiteCarpet});
        assert(shape.kind == mc::world::ShapeKind::Column);
        assert(near(shape.bottom, 0.0F) && near(shape.top, 1.0F / 16.0F));
        assert(!mc::world::isFullCube(Block::WhiteCarpet));

        World world = floored();
        // On the stone floor: survives.
        assert(mc::world::canBlockSurvive(world, {8, 1, 8}, Block::RedCarpet,
                                          BlockOrientation::North));
        // In mid-air: does not.
        assert(!mc::world::canBlockSurvive(world, {8, 5, 8}, Block::RedCarpet,
                                           BlockOrientation::North));
        // On something that is not sturdy — a fence post — it still survives,
        // which is what separates AnyBelow from Ground.
        world.setBlock(8, 1, 8, Block::OakFence);
        assert(mc::world::canBlockSurvive(world, {8, 2, 8}, Block::RedCarpet,
                                          BlockOrientation::North));
    }

    std::cout << "cross_collision_family_test passed\n";
    return 0;
}

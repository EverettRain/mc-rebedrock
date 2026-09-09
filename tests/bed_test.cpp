// SLP-1: the bed block — two cells, their shape, how they hold each other up,
// and what happens to the partner when one half is broken.
//
// The bed is the first block in this roster whose two cells sit SIDE BY SIDE
// (the door's are stacked), and the first whose shape depends on two properties
// at once: FACING says which way the bed points, PART says which end this cell
// is, and together they decide which end the legs are drawn at. A test that only
// checked one PART would pass with the two swapped.
//
// It is also the first block that must NOT drop when it loses its support:
// vanilla removes the orphan half with `setBlock(AIR, 35)` — flag 35 carries no
// drop — so breaking either half of a bed yields exactly one bed. The support
// sweep is what removes the orphan here, so the "no drop" has to be a property
// of the block, not of the breaking code.

#include "gameplay/BlockBehavior.hpp"
#include "gameplay/ItemPlacement.hpp"
#include "world/Block.hpp"
#include "world/BlockPlacement.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/StairShapeDerivation.hpp"
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

// Lays a bed with its foot at `foot`, pointing (and therefore extending) north.
void layBed(World& world, glm::ivec3 foot, BlockOrientation facing = BlockOrientation::North) {
    const auto offset = mc::world::orientationOffset(facing);
    world.setState(foot.x, foot.y, foot.z, BlockState{Block::RedBed, facing}.withBedHead(false));
    world.setState(foot.x + offset.x, foot.y + offset.y, foot.z + offset.z,
                   BlockState{Block::RedBed, facing}.withBedHead(true));
}

// The Y span and footprint of the mattress box, whichever box it is in the set.
[[nodiscard]] bool hasMattress(const mc::world::BlockShape& shape) {
    for (const auto& box : shape.boxes) {
        if (near(box.minY, 3.0F / 16.0F) && near(box.maxY, 9.0F / 16.0F) &&
            near(box.minX, 0.0F) && near(box.maxX, 1.0F) && near(box.minZ, 0.0F) &&
            near(box.maxZ, 1.0F)) {
            return true;
        }
    }
    return false;
}

// Where the legs of this state sit, as the minimum Z (north) or maximum Z
// (south) they touch. Legs are 3/16 cubes on the floor.
[[nodiscard]] bool legsAtNorth(const mc::world::BlockShape& shape) {
    int legs = 0;
    for (const auto& box : shape.boxes) {
        if (near(box.maxY, 3.0F / 16.0F)) {
            ++legs;
            if (!near(box.minZ, 0.0F)) {
                return false;
            }
        }
    }
    return legs == 2;
}

} // namespace

int main() {
    using mc::world::blockShape;

    // ---------------------------------------------------------------------
    // 1) Shape: a 3/16..9/16 mattress plus two floor legs, and the legs are at
    //    the bed's OUTER end — opposite ends for the two halves.
    // ---------------------------------------------------------------------
    {
        const auto foot = BlockState{Block::RedBed, BlockOrientation::North}.withBedHead(false);
        const auto head = BlockState{Block::RedBed, BlockOrientation::North}.withBedHead(true);
        const auto footShape = blockShape(foot);
        const auto headShape = blockShape(head);
        assert(footShape.kind == mc::world::ShapeKind::Boxes);
        assert(footShape.boxes.size() == 3U); // mattress + two legs
        assert(hasMattress(footShape) && hasMattress(headShape));
        // A bed points north: the head is the north cell, so the HEAD's legs are
        // at its north edge and the FOOT's are at its south edge. Swapping PART
        // must therefore move them — this is the assertion that fails if the
        // shape keys on FACING alone.
        assert(legsAtNorth(headShape));
        assert(!legsAtNorth(footShape));
        // You step UP onto a bed: it is not a slab, and nothing about it is
        // flush with the floor except the legs.
        assert(!near(footShape.boxes[0].minY, 0.0F));
    }

    // ---------------------------------------------------------------------
    // 2) Support: a half stands only while its partner is there.
    // ---------------------------------------------------------------------
    {
        World world = floored();
        layBed(world, {8, 1, 8});
        const glm::ivec3 foot{8, 1, 8};
        const glm::ivec3 head{8, 1, 7}; // north of the foot
        assert(mc::world::canBlockSurvive(world, foot, Block::RedBed, BlockOrientation::North));
        assert(mc::world::canBlockSurvive(world, head, Block::RedBed, BlockOrientation::North));

        // Take the head away and the foot is no longer supported.
        world.setBlock(head.x, head.y, head.z, Block::Air);
        assert(!mc::world::canBlockSurvive(world, foot, Block::RedBed, BlockOrientation::North));

        // A bed of a different colour is not a partner.
        world.setState(head.x, head.y, head.z,
                       BlockState{Block::BlueBed, BlockOrientation::North}.withBedHead(true));
        assert(!mc::world::canBlockSurvive(world, foot, Block::RedBed, BlockOrientation::North));
        // Nor is one pointing a different way.
        world.setState(head.x, head.y, head.z,
                       BlockState{Block::RedBed, BlockOrientation::East}.withBedHead(true));
        assert(!mc::world::canBlockSurvive(world, foot, Block::RedBed, BlockOrientation::North));

        // A bed does not need a floor — vanilla has no canSurvive on it at all.
        World air;
        Chunk empty;
        air.setChunk({0, 0}, std::move(empty));
        layBed(air, {8, 40, 8});
        assert(mc::world::canBlockSurvive(air, {8, 40, 8}, Block::RedBed,
                                          BlockOrientation::North));
    }

    // ---------------------------------------------------------------------
    // 3) The orphan half must vanish WITHOUT dropping — otherwise breaking one
    //    bed yields two.
    // ---------------------------------------------------------------------
    {
        assert(!mc::world::blockDefinition(Block::RedBed).dropsWhenUnsupported);
        assert(mc::world::blockDefinition(Block::RedBed).dropsItem); // mined, it drops
        // Everything else in the roster still drops when it pops off.
        assert(mc::world::blockDefinition(Block::Torch).dropsWhenUnsupported);
        assert(mc::world::blockDefinition(Block::WhiteCarpet).dropsWhenUnsupported);
    }

    // ---------------------------------------------------------------------
    // 4) OCCUPIED is mirrored across the two halves, and only from the partner
    //    direction.
    // ---------------------------------------------------------------------
    {
        const auto foot = BlockState{Block::RedBed, BlockOrientation::North}.withBedHead(false);
        const auto occupiedHead =
            BlockState{Block::RedBed, BlockOrientation::North}.withBedHead(true).withOccupied(true);
        // The head is north of the foot, so the foot hears about it from -Z.
        const auto synced = mc::world::bedUpdateShape(foot, BlockPos{0, 0, -1}, occupiedHead);
        assert(synced.occupied());
        assert(!synced.isBedHead()); // PART itself never changes
        // A neighbour on any other side is ignored.
        assert(!mc::world::bedUpdateShape(foot, BlockPos{1, 0, 0}, occupiedHead).occupied());
        assert(!mc::world::bedUpdateShape(foot, BlockPos{0, 1, 0}, occupiedHead).occupied());
        // And a neighbour that is not the other half is ignored too.
        const auto notAPartner =
            BlockState{Block::RedBed, BlockOrientation::North}.withBedHead(false).withOccupied(true);
        assert(!mc::world::bedUpdateShape(foot, BlockPos{0, 0, -1}, notAPartner).occupied());
    }

    // ---------------------------------------------------------------------
    // 5) Placement puts down two cells, and refuses when the second is blocked.
    // ---------------------------------------------------------------------
    {
        World world = floored();
        mc::world::PlacementContext context{};
        context.placePosition = {8, 1, 8};
        context.clickedFace = BlockOrientation::Up;
        context.lookDirection = {0.0F, 0.0F, -1.0F}; // looking north
        const auto placed =
            mc::gameplay::itemUseOn(mc::gameplay::blockItemFor(Block::RedBed), world, context);
        assert(placed.action == mc::gameplay::ItemUseAction::PlaceBed);
        assert(placed.state.block() == Block::RedBed);
        assert(!placed.state.isBedHead()); // the clicked cell is the FOOT
        // AwayFromPlayer: looking north puts the head to the north.
        assert(placed.state.orientation() == BlockOrientation::North);

        // Block the head's cell and the whole placement is refused.
        world.setBlock(8, 1, 7, Block::Stone);
        const auto refused =
            mc::gameplay::itemUseOn(mc::gameplay::blockItemFor(Block::RedBed), world, context);
        assert(refused.action == mc::gameplay::ItemUseAction::Nothing);
    }

    // ---------------------------------------------------------------------
    // 6) ★ The field bug: a bed placed from a LEGACY stack (a block stack whose
    //    item pointer is null, which is what the creative catalog and the tests
    //    hand out) went down the single-cell path, because that path only knew
    //    about the door. A lone bed half then failed its own support rule
    //    (BedOtherHalf: no partner), so nothing was placed and nothing said why.
    // ---------------------------------------------------------------------
    {
        World world = floored();
        mc::world::PlacementContext context{};
        context.clickedBlock = {8, 0, 7};
        context.placePosition = {8, 1, 7};
        context.clickedFace = BlockOrientation::Up;
        context.lookDirection = {0.0F, 0.0F, -1.0F};
        const mc::gameplay::ItemStack legacy{Block::RedBed, 1U, nullptr};
        const auto result = mc::gameplay::legacyBlockStackUseOn(legacy, world, context);
        assert(result.action == mc::gameplay::ItemUseAction::PlaceBed);
        assert(result.state.block() == Block::RedBed);
        assert(!result.state.isBedHead());
        assert(result.state.orientation() == BlockOrientation::North);
        // The live-item path agrees with it, which is the property that was
        // silently untrue.
        const auto viaItem =
            mc::gameplay::itemUseOn(mc::gameplay::blockItemFor(Block::RedBed), world, context);
        assert(viaItem.action == result.action);
        assert(viaItem.state == result.state);
    }

    std::cout << "bed_test passed\n";
    return 0;
}

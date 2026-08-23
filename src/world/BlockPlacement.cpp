#include "world/BlockPlacement.hpp"

#include "world/BlockPos.hpp"
#include "world/StairShapeDerivation.hpp" // AR-B2: stairShapeFor
#include "world/WallShapeDerivation.hpp"  // AR-B3: wallConnectionsFor
#include "world/World.hpp"

#include <array>
#include <cmath>

namespace mc::world {
namespace {

// The horizontal directions in clockwise order, matching Java's Direction
// values (NORTH, EAST, SOUTH, WEST).
constexpr std::array<BlockOrientation, 4> kClockwiseHorizontals{
    BlockOrientation::North,
    BlockOrientation::East,
    BlockOrientation::South,
    BlockOrientation::West,
};

[[nodiscard]] constexpr std::size_t horizontalIndex(BlockOrientation orientation) {
    for (std::size_t index = 0; index < kClockwiseHorizontals.size(); ++index) {
        if (kClockwiseHorizontals[index] == orientation) return index;
    }
    return 0U;
}

// The four horizontal directions ordered like Java's
// BlockPlaceContext#getNearestLookingDirections: starting from `first` and
// walking clockwise, so a fallback torch walks toward the wall the player is
// aiming at rather than an arbitrary direction.
[[nodiscard]] std::array<BlockOrientation, 4> horizontalDirectionsFrom(
    BlockOrientation first) {
    std::array<BlockOrientation, 4> ordered;
    const std::size_t start = horizontalIndex(first);
    for (std::size_t offset = 0; offset < ordered.size(); ++offset) {
        ordered[offset] =
            kClockwiseHorizontals[(start + offset) % kClockwiseHorizontals.size()];
    }
    return ordered;
}

// BlockBehaviour#canSurvive as data: one rule per BlockSupport category, indexed
// by the block's support field. B1-2 replaces canBlockSurvive's
// `switch(blockSupport(block))` with this table, so adding a block never adds a
// case — it names the support it needs and the table routes it (the R1 audit
// flagged the switch as one of the six parallel lists). `facing` is only read by
// the wall rule, whose support sits behind the block's FACING state.
using SupportRuleFn = bool (*)(const World&, glm::ivec3, BlockOrientation);

[[nodiscard]] bool supportAlways(const World&, glm::ivec3, BlockOrientation) {
    return true;
}
[[nodiscard]] bool supportGround(const World& world, glm::ivec3 position, BlockOrientation) {
    return isFaceSturdy(world.block(position.x, position.y - 1, position.z));
}
[[nodiscard]] bool supportWall(const World& world, glm::ivec3 position, BlockOrientation facing) {
    // A wall block's support is behind its FACING, which is state rather than
    // identity now, so the caller supplies it.
    const auto support = position + orientationOffset(wallTorchSupportSide(facing));
    return isFaceSturdy(world.block(support.x, support.y, support.z));
}
[[nodiscard]] bool supportSoil(const World& world, glm::ivec3 position, BlockOrientation) {
    return isSoil(world.block(position.x, position.y - 1, position.z));
}
[[nodiscard]] bool supportFarmland(const World& world, glm::ivec3 position, BlockOrientation) {
    // CropsBlock#canSurvive: only farmland (tilled soil) holds a crop.
    return isFarmland(world.block(position.x, position.y - 1, position.z));
}

inline constexpr std::array<SupportRuleFn, 5> kSupportRules{{
    &supportAlways,   // BlockSupport::None
    &supportGround,   // BlockSupport::Ground
    &supportWall,     // BlockSupport::Wall
    &supportSoil,     // BlockSupport::Soil
    &supportFarmland, // BlockSupport::Farmland
}};
static_assert(static_cast<std::size_t>(BlockSupport::None) == 0U);
static_assert(static_cast<std::size_t>(BlockSupport::Ground) == 1U);
static_assert(static_cast<std::size_t>(BlockSupport::Wall) == 2U);
static_assert(static_cast<std::size_t>(BlockSupport::Soil) == 3U);
static_assert(static_cast<std::size_t>(BlockSupport::Farmland) == 4U);

} // namespace

glm::ivec3 orientationOffset(BlockOrientation orientation) {
    switch (orientation) {
    case BlockOrientation::North:
        return {0, 0, -1};
    case BlockOrientation::East:
        return {1, 0, 0};
    case BlockOrientation::South:
        return {0, 0, 1};
    case BlockOrientation::West:
        return {-1, 0, 0};
    case BlockOrientation::Up:
        return {0, 1, 0};
    case BlockOrientation::Down:
        return {0, -1, 0};
    }
    return {0, 0, -1};
}

BlockOrientation orientationFromOffset(glm::ivec3 offset) {
    if (offset.y > 0) return BlockOrientation::Up;
    if (offset.y < 0) return BlockOrientation::Down;
    if (offset.x > 0) return BlockOrientation::East;
    if (offset.x < 0) return BlockOrientation::West;
    if (offset.z > 0) return BlockOrientation::South;
    return BlockOrientation::North;
}

BlockOrientation horizontalFacing(glm::vec3 lookDirection) {
    // Direction#fromYRot rounds the yaw to the nearest quadrant, which is the
    // same as taking the dominant horizontal component of the view vector.
    if (std::abs(lookDirection.x) > std::abs(lookDirection.z)) {
        return lookDirection.x >= 0.0F ? BlockOrientation::East : BlockOrientation::West;
    }
    return lookDirection.z >= 0.0F ? BlockOrientation::South : BlockOrientation::North;
}

bool canBlockSurvive(const World& world, glm::ivec3 position, Block block,
                     BlockOrientation facing) {
    return kSupportRules[static_cast<std::size_t>(blockSupport(block))](world, position, facing);
}

std::optional<BlockState> standingAndWallPlacement(
    const World& world,
    const PlacementContext& context) {
    // StandingAndWallBlockItem#getPlacementState: the wall variant wins on a
    // side face, then the floor variant, then any other wall that happens to be
    // available.
    if (isHorizontal(context.clickedFace)) {
        // The clicked block's own wall first, exactly as Java's
        // getNearestLookingDirections starts at the clicked side, so the torch
        // leans off the wall the player aimed at.
        if (canBlockSurvive(world, context.placePosition, Block::WallTorch,
                            context.clickedFace)) {
            return BlockState{Block::WallTorch, context.clickedFace};
        }
    }
    if (canBlockSurvive(world, context.placePosition, Block::Torch,
                        BlockOrientation::North)) {
        return BlockState{Block::Torch};
    }
    // A torch placed at an angle — or onto a non-sturdy block — falls back to
    // the remaining walls nearest to where the player is looking, so it leans
    // off the wall the player is aiming at instead of an arbitrary direction.
    // A fixed north/south/west/east sweep could leave it attached to a wall
    // behind the placement cell, leaning toward the player, which reads as a
    // torch floating off the wall.
    for (const auto facing : horizontalDirectionsFrom(horizontalFacing(context.lookDirection))) {
        if (isHorizontal(context.clickedFace) && facing == context.clickedFace) {
            continue;
        }
        if (canBlockSurvive(world, context.placePosition, Block::WallTorch, facing)) {
            return BlockState{Block::WallTorch, facing};
        }
    }
    return std::nullopt;
}

// SimpleWaterloggedBlock#getStateForPlacement's own contribution: `context.
// getLevel().getFluidState(pos).getType() == Fluids.WATER` sets the new
// block's WATERLOGGED true. Read before the write happens — the caller places
// into `context.placePosition`, which right now (before this state lands)
// either is the water source itself (placing into a water cell replaces it,
// per `isReplaceable(Water)`) or is unrelated dry air, so "was that cell a
// still water source" is exactly the question to ask about the *current*
// world, not the one this function is about to produce.
[[nodiscard]] bool placingIntoWaterSource(const World& world, const PlacementContext& context) {
    const auto& target = context.placePosition;
    return world.block(target.x, target.y, target.z) == Block::Water &&
        world.state(target.x, target.y, target.z).value(StateProperty::FluidLevel) == 0U;
}

std::optional<BlockState> placementBlock(
    const World& world,
    Block selected,
    const PlacementContext& context) {
    if (!isRenderable(selected)) {
        return std::nullopt;
    }
    if (selected == Block::Torch) {
        return standingAndWallPlacement(world, context);
    }
    if (!canBlockSurvive(world, context.placePosition, selected,
                         placementOrientation(selected, context))) {
        return std::nullopt;
    }
    // F2: a submergible block placed into a still water source comes out wet
    // (SimpleWaterloggedBlock#getStateForPlacement), regardless of which shape
    // branch below decides the rest of its state.
    const SubmergedFluid submerged = (canBeSubmerged(selected) && placingIntoWaterSource(world, context))
                                         ? SubmergedFluid::Water
                                         : SubmergedFluid::None;
    if (isSlab(selected)) {
        // SlabBlock#getStateForPlacement: a Down face hangs a top slab, an Up
        // face rests a bottom one, and a horizontal face reads the sub-cell hit
        // height within the placement cell — aim at the upper half of a block's
        // side and the slab rests on top, the lower half and it sits on the floor.
        const bool aboveHalf =
            context.hitPosition.y - static_cast<float>(context.placePosition.y) > 0.5F;
        const SlabPortion portion =
            (context.clickedFace != BlockOrientation::Down &&
             (context.clickedFace == BlockOrientation::Up || !aboveHalf))
                ? SlabPortion::Bottom
                : SlabPortion::Top;
        return BlockState{selected}.withSlabPortion(portion).withSubmergedFluid(submerged);
    }
    if (blockDefinition(selected).model == BlockModel::Stairs) {
        // StairBlock#getStateForPlacement: the same up/down/hit-height rule a
        // slab's half uses (BlockPlacement.hpp's PlacementContext carries the
        // same hitPosition a slab reads), plus the join shape computed against
        // the *current* world right away — a stair's SHAPE is known the instant
        // it lands, not left for the first neighbour notification to fill in.
        const bool aboveHalf =
            context.hitPosition.y - static_cast<float>(context.placePosition.y) > 0.5F;
        const SlabPortion half =
            (context.clickedFace != BlockOrientation::Down &&
             (context.clickedFace == BlockOrientation::Up || !aboveHalf))
                ? SlabPortion::Bottom
                : SlabPortion::Top;
        const BlockState oriented =
            BlockState{selected, placementOrientation(selected, context)}.withStairHalf(half);
        const BlockPos placePos{context.placePosition.x, context.placePosition.y,
                                context.placePosition.z};
        return oriented.withStairShape(stairShapeFor(world, placePos, oriented))
            .withSubmergedFluid(submerged);
    }
    if (blockDefinition(selected).model == BlockModel::TrapDoor) {
        // TrapDoorBlock#getStateForPlacement: a horizontal clicked face hangs
        // the trapdoor on that wall, half decided by the sub-cell hit height
        // (upper half of the click -> Top, lower -> Bottom); a vertical
        // clicked face (top/bottom of the block below/above) instead uses the
        // player's own horizontal facing and reads Bottom/Top straight off
        // which face was clicked.
        BlockOrientation facing;
        SlabPortion half;
        if (isHorizontal(context.clickedFace)) {
            facing = context.clickedFace;
            const bool aboveHalf =
                context.hitPosition.y - static_cast<float>(context.placePosition.y) > 0.5F;
            half = aboveHalf ? SlabPortion::Top : SlabPortion::Bottom;
        } else {
            facing = oppositeOrientation(horizontalFacing(context.lookDirection));
            half = context.clickedFace == BlockOrientation::Up ? SlabPortion::Bottom
                                                                : SlabPortion::Top;
        }
        return BlockState{selected, facing}
            .withTrapdoorHalf(half)
            .withSubmergedFluid(submerged);
    }
    if (blockDefinition(selected).model == BlockModel::Wall) {
        // WallBlock#getStateForPlacement: the connection mask is known the
        // instant the wall lands, not left for the first neighbour
        // notification — same "compute immediately" move stairs already make.
        const BlockPos placePos{context.placePosition.x, context.placePosition.y,
                                context.placePosition.z};
        return wallConnectionsFor(world, placePos, BlockState{selected})
            .withSubmergedFluid(submerged);
    }
    return BlockState{selected, placementOrientation(selected, context)}.withSubmergedFluid(submerged);
}

BlockOrientation placementOrientation(Block placed, const PlacementContext& context) {
    if (isLog(placed)) {
        // RotatedPillarBlock stores the axis of the clicked face.
        return context.clickedFace;
    }
    // AR-B3: the button's wall-only simplification (support(Wall), Facing(6)
    // but only ever placed against a horizontal face here — see button()'s own
    // comment). Its FACING is the side it protrudes away from, exactly
    // WallTorch's convention: the clicked face directly, not its opposite —
    // clicking a wall's east face hangs the button facing east, off that wall.
    if (blockDefinition(placed).model == BlockModel::Button && isHorizontal(context.clickedFace)) {
        return context.clickedFace;
    }
    if (hasHorizontalFacing(placed)) {
        // HorizontalDirectionalBlock: the front faces back at the player.
        return oppositeOrientation(horizontalFacing(context.lookDirection));
    }
    // Leaves' PERSISTENT state is set by the LeavesBlockItem at the gameplay
    // layer (see itemPlacementOrientation); the block properties default here.
    return defaultOrientation(placed);
}

} // namespace mc::world

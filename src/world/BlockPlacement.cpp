#include "world/BlockPlacement.hpp"

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
    switch (blockSupport(block)) {
    case BlockSupport::None:
        return true;
    case BlockSupport::Ground:
        return isFaceSturdy(world.block(position.x, position.y - 1, position.z));
    case BlockSupport::Wall: {
        // A wall block's support is behind its FACING, which is state rather
        // than identity now, so the caller supplies it.
        const auto support = position + orientationOffset(wallTorchSupportSide(facing));
        return isFaceSturdy(world.block(support.x, support.y, support.z));
    }
    case BlockSupport::Soil:
        return isSoil(world.block(position.x, position.y - 1, position.z));
    case BlockSupport::Farmland:
        // CropsBlock#canSurvive: only farmland (tilled soil) holds a crop.
        return isFarmland(world.block(position.x, position.y - 1, position.z));
    }
    return true;
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
    if (isSlab(selected)) {
        // SlabBlock#getStateForPlacement: the clicked face decides the half.
        // Clicking a top face rests a bottom slab, a bottom face hangs a top
        // slab; a side face has no sub-cell hit fraction here, so it defaults to
        // the bottom half the way a floor placement does.
        const SlabPortion portion =
            context.clickedFace == BlockOrientation::Down ? SlabPortion::Top
                                                          : SlabPortion::Bottom;
        return BlockState{selected}.withSlabPortion(portion);
    }
    return BlockState{selected, placementOrientation(selected, context)};
}

BlockOrientation placementOrientation(Block placed, const PlacementContext& context) {
    if (isLog(placed)) {
        // RotatedPillarBlock stores the axis of the clicked face.
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

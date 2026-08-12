#include "world/VoxelRaycast.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace mc::world {
namespace {

[[nodiscard]] int stepFor(float value) {
    return value > 0.0F ? 1 : (value < 0.0F ? -1 : 0);
}

[[nodiscard]] float firstBoundaryDistance(float origin, float direction, int cell, int step) {
    if (step == 0) {
        return std::numeric_limits<float>::infinity();
    }
    const float boundary = static_cast<float>(cell + (step > 0 ? 1 : 0));
    return (boundary - origin) / direction;
}

struct SelectionBox final {
    glm::vec3 minimum{};
    glm::vec3 maximum{1.0F};
};

struct BoxRaycastHit final {
    float distance = 0.0F;
    glm::ivec3 normal{};
};

// A wall torch's lean is its FACING state now, so the box needs the cell's
// orientation rather than just its block.
[[nodiscard]] SelectionBox torchSelectionBox(Block block, BlockOrientation orientation) {
    glm::vec3 facing{0.0F};
    const bool wall = block == Block::WallTorch;
    if (wall) {
        if (orientation == BlockOrientation::North) facing.z = -1.0F;
        if (orientation == BlockOrientation::East) facing.x = 1.0F;
        if (orientation == BlockOrientation::South) facing.z = 1.0F;
        if (orientation == BlockOrientation::West) facing.x = -1.0F;
    }
    const glm::vec3 base = wall
        ? glm::vec3{0.5F, 0.18F, 0.5F} - facing * kWallTorchInset
        : glm::vec3{0.5F, 0.0F, 0.5F};
    const glm::vec3 axis = wall
        ? glm::vec3{facing.x * 0.28F, 0.58F, facing.z * 0.28F}
        : glm::vec3{0.0F, 0.625F, 0.0F};
    const glm::vec3 up = glm::normalize(axis);
    const glm::vec3 right = wall
        ? glm::normalize(glm::vec3{facing.z, 0.0F, -facing.x})
        : glm::vec3{1.0F, 0.0F, 0.0F};
    const glm::vec3 forward = glm::normalize(glm::cross(right, up));
    constexpr float halfWidth = 1.0F / 16.0F;
    const glm::vec3 r = right * halfWidth;
    const glm::vec3 f = forward * halfWidth;
    const std::array corners{
        base - r - f, base + r - f, base + r + f, base - r + f,
        base + axis - r - f, base + axis + r - f,
        base + axis + r + f, base + axis - r + f,
    };
    SelectionBox bounds{corners.front(), corners.front()};
    for (const glm::vec3& corner : corners) {
        bounds.minimum.x = std::min(bounds.minimum.x, corner.x);
        bounds.minimum.y = std::min(bounds.minimum.y, corner.y);
        bounds.minimum.z = std::min(bounds.minimum.z, corner.z);
        bounds.maximum.x = std::max(bounds.maximum.x, corner.x);
        bounds.maximum.y = std::max(bounds.maximum.y, corner.y);
        bounds.maximum.z = std::max(bounds.maximum.z, corner.z);
    }
    return bounds;
}

// The selection box a block's raycast tests against, or nullopt for a block
// whose whole cell is selectable (the common cube and cross-plant cases). A
// crop reads its stage from the orientation state and shrinks to that height;
// farmland is the vanilla 15/16 box. Torches keep their dedicated shape.
[[nodiscard]] std::optional<SelectionBox> blockInteractionShape(
    const World& world, glm::ivec3 cell, Block block) {
    if (isTorch(block)) {
        return torchSelectionBox(block, world.orientation(cell.x, cell.y, cell.z));
    }
    if (isCrop(block)) {
        const int age = cropAge(world.orientation(cell.x, cell.y, cell.z));
        return SelectionBox{{0.0F, 0.0F, 0.0F}, {1.0F, cropSelectionHeight(age), 1.0F}};
    }
    if (isFarmland(block)) {
        return SelectionBox{{0.0F, 0.0F, 0.0F}, {1.0F, kFarmlandModelHeight, 1.0F}};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<BoxRaycastHit> raycastBox(
    glm::vec3 origin,
    glm::vec3 direction,
    const SelectionBox& localBounds,
    glm::ivec3 cell,
    float maximumDistance) {
    const glm::vec3 cellOrigin{cell};
    const glm::vec3 minimum = cellOrigin + localBounds.minimum;
    const glm::vec3 maximum = cellOrigin + localBounds.maximum;
    float nearDistance = -std::numeric_limits<float>::infinity();
    float farDistance = std::numeric_limits<float>::infinity();
    glm::ivec3 nearNormal{};
    constexpr float epsilon = 0.000001F;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) <= epsilon) {
            if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis]) {
                return std::nullopt;
            }
            continue;
        }
        float first = (minimum[axis] - origin[axis]) / direction[axis];
        float second = (maximum[axis] - origin[axis]) / direction[axis];
        glm::ivec3 firstNormal{};
        glm::ivec3 secondNormal{};
        firstNormal[axis] = -1;
        secondNormal[axis] = 1;
        if (first > second) {
            std::swap(first, second);
            std::swap(firstNormal, secondNormal);
        }
        if (first > nearDistance) {
            nearDistance = first;
            nearNormal = firstNormal;
        }
        farDistance = std::min(farDistance, second);
        if (nearDistance > farDistance) return std::nullopt;
    }
    if (farDistance < 0.0F || nearDistance > maximumDistance) {
        return std::nullopt;
    }
    if (nearDistance < 0.0F) return BoxRaycastHit{0.0F, {}};
    return BoxRaycastHit{nearDistance, nearNormal};
}

} // namespace

std::optional<VoxelRaycastHit> raycastVoxels(
    const World& world,
    glm::vec3 origin,
    glm::vec3 direction,
    float maximumDistance,
    bool includeFluids) {
    if (maximumDistance < 0.0F || glm::length(direction) <= 0.000001F) {
        return std::nullopt;
    }
    direction = glm::normalize(direction);
    glm::ivec3 cell{
        static_cast<int>(std::floor(origin.x)),
        static_cast<int>(std::floor(origin.y)),
        static_cast<int>(std::floor(origin.z)),
    };
    const glm::ivec3 step{
        stepFor(direction.x), stepFor(direction.y), stepFor(direction.z)};
    glm::vec3 nextDistance{
        firstBoundaryDistance(origin.x, direction.x, cell.x, step.x),
        firstBoundaryDistance(origin.y, direction.y, cell.y, step.y),
        firstBoundaryDistance(origin.z, direction.z, cell.z, step.z),
    };
    const glm::vec3 distancePerCell{
        step.x == 0 ? std::numeric_limits<float>::infinity() : std::abs(1.0F / direction.x),
        step.y == 0 ? std::numeric_limits<float>::infinity() : std::abs(1.0F / direction.y),
        step.z == 0 ? std::numeric_limits<float>::infinity() : std::abs(1.0F / direction.z),
    };

    glm::ivec3 entryNormal{};
    float distance = 0.0F;
    while (distance <= maximumDistance) {
        const Block current = world.block(cell.x, cell.y, cell.z);
        // A bucket ray stops only at a still-water source (BucketItem's
        // SOURCE_ONLY); flowing water is walked past, so a block behind it
        // stays reachable.
        if (isSelectable(current) ||
            (includeFluids && isFluid(current) && world.fluidLevel(cell.x, cell.y, cell.z) == 0U)) {
            // Sub-block shapes (torch, crop, farmland) are tested against their
            // actual box; a full-cell block is hit the moment the ray enters.
            const auto shape = blockInteractionShape(world, cell, current);
            if (shape.has_value()) {
                const auto shapeHit = raycastBox(
                    origin, direction, *shape, cell, maximumDistance);
                const float cellExitDistance = std::min(
                    nextDistance.x, std::min(nextDistance.y, nextDistance.z));
                if (shapeHit.has_value() &&
                    shapeHit->distance <= cellExitDistance + 0.00001F) {
                    return VoxelRaycastHit{
                        cell,
                        cell + shapeHit->normal,
                        shapeHit->normal,
                        shapeHit->distance,
                    };
                }
            } else {
                return VoxelRaycastHit{cell, cell + entryNormal, entryNormal, distance};
            }
        }

        if (nextDistance.x <= nextDistance.y && nextDistance.x <= nextDistance.z) {
            distance = nextDistance.x;
            nextDistance.x += distancePerCell.x;
            cell.x += step.x;
            entryNormal = {-step.x, 0, 0};
        } else if (nextDistance.y <= nextDistance.z) {
            distance = nextDistance.y;
            nextDistance.y += distancePerCell.y;
            cell.y += step.y;
            entryNormal = {0, -step.y, 0};
        } else {
            distance = nextDistance.z;
            nextDistance.z += distancePerCell.z;
            cell.z += step.z;
            entryNormal = {0, 0, -step.z};
        }
    }
    return std::nullopt;
}

BlockBounds blockSelectionBounds(
    const World& world, glm::ivec3 position, Block block) {
    switch (blockDefinition(block).model) {
        case BlockModel::Cross:
            // Plants: a slim upright box, roughly the cross's footprint.
            return {{0.1F, 0.0F, 0.1F}, {0.9F, 0.8F, 0.9F}};
        case BlockModel::Crop:
            // CropsBlock.SHAPES: the box grows with the age stored in the
            // orientation state, from 2/16 to a full block.
            return {{0.0F, 0.0F, 0.0F},
                    {1.0F, cropSelectionHeight(
                               cropAge(world.orientation(position.x, position.y, position.z))),
                     1.0F}};
        case BlockModel::Torch: {
            const SelectionBox box = torchSelectionBox(
                block, world.orientation(position.x, position.y, position.z));
            return {box.minimum, box.maximum};
        }
        case BlockModel::Chest:
            // 14x14x14 chest sitting on the floor (1/16 inset on each side).
            return {{0.0625F, 0.0F, 0.0625F}, {0.9375F, 0.875F, 0.9375F}};
        case BlockModel::Cube:
            break;
    }
    // Farmland is a cube whose solid box is the vanilla 15/16 shape.
    if (isFarmland(block)) {
        return {{0.0F, 0.0F, 0.0F}, {1.0F, kFarmlandModelHeight, 1.0F}};
    }
    return {};
}

} // namespace mc::world

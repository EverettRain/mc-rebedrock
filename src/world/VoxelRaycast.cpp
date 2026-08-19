#include "world/VoxelRaycast.hpp"

#include "world/BlockShape.hpp"

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

// The box for a shape's Column form: it fills the whole footprint, so only its
// height varies.
[[nodiscard]] SelectionBox columnBox(const BlockShape& shape) {
    return SelectionBox{{0.0F, shape.bottom, 0.0F}, {1.0F, shape.top, 1.0F}};
}

[[nodiscard]] SelectionBox boxOf(const ShapeBox& box) {
    return SelectionBox{{box.minX, box.minY, box.minZ}, {box.maxX, box.maxY, box.maxZ}};
}

// Whether a shape fills its whole cell, so the ray hits it the instant it enters
// — the common cube, and a double slab. Kept as a fast path so the overwhelming
// majority of cells never build or test a box.
[[nodiscard]] bool isFullCellShape(const BlockShape& shape) {
    return shape.kind == ShapeKind::Column && shape.bottom <= 0.0F && shape.top >= 1.0F;
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

// The nearest hit among a non-full shape's boxes. A Column contributes its one
// footprint box; Boxes contributes each of its boxes; the closest wins so a
// stair or fence (many boxes) reports the face the ray reaches first.
[[nodiscard]] std::optional<BoxRaycastHit> raycastShape(
    glm::vec3 origin,
    glm::vec3 direction,
    const BlockShape& shape,
    glm::ivec3 cell,
    float maximumDistance) {
    std::optional<BoxRaycastHit> best;
    const auto consider = [&](const SelectionBox& box) {
        const auto hit = raycastBox(origin, direction, box, cell, maximumDistance);
        if (hit.has_value() && (!best.has_value() || hit->distance < best->distance)) {
            best = hit;
        }
    };
    switch (shape.kind) {
    case ShapeKind::Empty:
        break;
    case ShapeKind::Column:
        consider(columnBox(shape));
        break;
    case ShapeKind::Boxes:
        for (const ShapeBox& box : shape.boxes) {
            consider(boxOf(box));
        }
        break;
    }
    return best;
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
        if (isSelectable(current)) {
            // The pick ray tests the block's base shape — the one source the
            // outline and collision also read, so a slab's half box, a chest's
            // 14/16 box and a flower's stalk are hit where they are drawn rather
            // than anywhere in the cell. A full-cell shape is hit the instant
            // the ray enters, exactly as before.
            const BlockShape shape = blockShape(world.state(cell.x, cell.y, cell.z));
            if (isFullCellShape(shape)) {
                return VoxelRaycastHit{cell, cell + entryNormal, entryNormal, distance};
            }
            const auto shapeHit = raycastShape(origin, direction, shape, cell, maximumDistance);
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
        } else if (includeFluids && isFluid(current) &&
                   world.fluidLevel(cell.x, cell.y, cell.z) == 0U) {
            // A bucket ray stops only at a still-water source (BucketItem's
            // SOURCE_ONLY); flowing water is walked past, so a block behind it
            // stays reachable. A source fills its cell, hit on entry.
            return VoxelRaycastHit{cell, cell + entryNormal, entryNormal, distance};
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

BlockBounds blockSelectionBounds(const World& world, glm::ivec3 position) {
    // The outline is the block's base shape — the same source the pick ray tests
    // — so the highlight can never again hug a shape the ray does not hit. A
    // Column is its footprint at its height; Boxes shows the boxes' bounding box
    // (a single box for a torch, chest or plant today).
    const BlockShape shape = blockShape(world.state(position.x, position.y, position.z));
    switch (shape.kind) {
    case ShapeKind::Empty:
        break;
    case ShapeKind::Column:
        return {{0.0F, shape.bottom, 0.0F}, {1.0F, shape.top, 1.0F}};
    case ShapeKind::Boxes: {
        if (shape.boxes.empty()) {
            break;
        }
        glm::vec3 minimum{shape.boxes.front().minX, shape.boxes.front().minY,
                          shape.boxes.front().minZ};
        glm::vec3 maximum{shape.boxes.front().maxX, shape.boxes.front().maxY,
                          shape.boxes.front().maxZ};
        for (const ShapeBox& box : shape.boxes) {
            minimum.x = std::min(minimum.x, box.minX);
            minimum.y = std::min(minimum.y, box.minY);
            minimum.z = std::min(minimum.z, box.minZ);
            maximum.x = std::max(maximum.x, box.maxX);
            maximum.y = std::max(maximum.y, box.maxY);
            maximum.z = std::max(maximum.z, box.maxZ);
        }
        return {minimum, maximum};
    }
    }
    return {};
}

} // namespace mc::world

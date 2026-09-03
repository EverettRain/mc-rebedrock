#pragma once

#include "world/World.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <optional>

namespace mc::world {

struct VoxelRaycastHit final {
    glm::ivec3 block{};
    glm::ivec3 adjacent{};
    glm::ivec3 normal{};
    float distance = 0.0F;
};

// Axis-aligned bounds (in block-local 0..1 space) of a block's visual shape,
// used to size the selection outline so sub-block shapes (torch, plants, chest,
// the 15/16 farmland box and a crop's per-stage box) no longer show a
// full-cube marker. The world and cell are read so a crop's outline follows the
// age its AGE property carries.
struct BlockBounds final {
    glm::vec3 minimum{0.0F};
    glm::vec3 maximum{1.0F};
};

// RN-10f / audit R17: the outline is drawn BOX BY BOX, the way vanilla draws a
// VoxelShape's edges, rather than as one box around the whole shape. A stair
// used to be outlined by a full cube, a wall by a cube, an open fence gate by a
// slab of air: the marker claimed a shape the pick ray does not hit and the
// player cannot stand on.
//
// Five is the roster's widest shape (a wall's post plus four arms); a stair is
// two or three. `voxel_raycast_test` asserts no block state exceeds it, so a new
// shape that does cannot silently lose boxes here.
inline constexpr std::size_t kMaxSelectionBoxes = 5;

struct BlockSelectionBoxes final {
    std::array<BlockBounds, kMaxSelectionBoxes> boxes{};
    std::size_t count = 0;
};

[[nodiscard]] BlockSelectionBoxes blockSelectionBoxes(const World& world, glm::ivec3 position);

[[nodiscard]] std::optional<VoxelRaycastHit> raycastVoxels(
    const World& world,
    glm::vec3 origin,
    glm::vec3 direction,
    float maximumDistance,
    bool includeFluids = false);

} // namespace mc::world

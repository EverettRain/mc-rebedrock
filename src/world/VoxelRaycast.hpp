#pragma once

#include "world/World.hpp"

#include <glm/vec3.hpp>

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

[[nodiscard]] BlockBounds blockSelectionBounds(
    const World& world, glm::ivec3 position, Block block);

[[nodiscard]] std::optional<VoxelRaycastHit> raycastVoxels(
    const World& world,
    glm::vec3 origin,
    glm::vec3 direction,
    float maximumDistance,
    bool includeFluids = false);

} // namespace mc::world

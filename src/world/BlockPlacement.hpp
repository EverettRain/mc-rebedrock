#pragma once

#include "world/Block.hpp"

#include <glm/vec3.hpp>

#include <optional>

namespace mc::world {

class World;

// The unit offset that points out of a block along the given direction.
[[nodiscard]] glm::ivec3 orientationOffset(BlockOrientation orientation);

// The direction the offset points at. Non-unit offsets fall back to North.
[[nodiscard]] BlockOrientation orientationFromOffset(glm::ivec3 offset);

// Direction#fromYRot: the cardinal direction the player is looking along.
[[nodiscard]] BlockOrientation horizontalFacing(glm::vec3 lookDirection);

// Everything BlockItem#getPlacementState reads off a use-on-block interaction.
struct PlacementContext final {
    // The block that was clicked and the cell the new block would occupy.
    glm::ivec3 clickedBlock{};
    glm::ivec3 placePosition{};
    // The face of the clicked block that the ray hit, pointing away from it.
    BlockOrientation clickedFace = BlockOrientation::Up;
    // The player's view direction, used for the horizontal FACING property.
    glm::vec3 lookDirection{0.0F, 0.0F, -1.0F};
};

// BlockBehaviour#canSurvive: whether the block has the support it requires.
[[nodiscard]] bool canBlockSurvive(const World& world, glm::ivec3 position, Block block);

// StandingAndWallBlockItem#getPlacementState: the torch item's own policy. The
// wall variant wins on a side face, then the floor variant, then any other wall
// that happens to be available; nullopt when none can survive there.
[[nodiscard]] std::optional<Block> standingAndWallPlacement(
    const World& world,
    const PlacementContext& context);

// BlockItem#getPlacementState: resolves the block variant that will actually be
// placed, or nullopt when no variant can survive there.
[[nodiscard]] std::optional<Block> placementBlock(
    const World& world,
    Block selected,
    const PlacementContext& context);

// The orientation stored with a freshly placed block.
[[nodiscard]] BlockOrientation placementOrientation(
    Block placed,
    const PlacementContext& context);

} // namespace mc::world

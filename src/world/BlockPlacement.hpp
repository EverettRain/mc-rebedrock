#pragma once

#include "world/Block.hpp"
#include "world/BlockState.hpp"

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
    // The precise point the ray struck the block's shape, in world space. Its
    // sub-cell height decides a slab's half when the clicked face alone cannot
    // (a horizontal face: upper half rests a top slab, lower half a bottom one).
    glm::vec3 hitPosition{0.0F};
    // The player's view direction, used for the horizontal FACING property.
    glm::vec3 lookDirection{0.0F, 0.0F, -1.0F};
};

// BlockBehaviour#canSurvive: whether the block has the support it requires.
// `facing` is only read by wall-mounted blocks, whose support sits behind their
// FACING state; everything else ignores it. It has no default on purpose — a
// wall torch checked against the wrong wall survives nothing, or survives
// everything, and a defaulted argument would hide that at every call site.
[[nodiscard]] bool canBlockSurvive(const World& world, glm::ivec3 position, Block block,
                                   BlockOrientation facing);

// StandingAndWallBlockItem#getPlacementState: the standing/wall item's own
// policy. The wall variant wins on a side face, then the floor (standing)
// variant, then any other wall that happens to be available; nullopt when none
// can survive there. `standing`/`wall` are the block pair the item carries
// (Torch/WallTorch, RedstoneTorch/RedstoneWallTorch, ...), so the one policy
// serves every such item rather than hardcoding the torch.
[[nodiscard]] std::optional<BlockState> standingAndWallPlacement(
    const World& world,
    const PlacementContext& context,
    Block standing,
    Block wall);

// BlockItem#getPlacementState: resolves the state that will actually be placed,
// or nullopt when nothing can survive there. It returns a whole state rather
// than a block because a wall torch's facing is decided here, by which wall it
// found, and cannot be recovered afterwards from the block alone.
[[nodiscard]] std::optional<BlockState> placementBlock(
    const World& world,
    Block selected,
    const PlacementContext& context);

// The orientation stored with a freshly placed block.
[[nodiscard]] BlockOrientation placementOrientation(
    Block placed,
    const PlacementContext& context);

} // namespace mc::world

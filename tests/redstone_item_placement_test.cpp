// AR-CX: two redstone components the player could not actually put down.
//
// Both are "the mechanism works, nobody can reach it" defects, which is the AR-CX
// axis's whole subject. Neither is a regression: the lever's placement branch has
// never existed (`git log -S "Block::Lever" -- src/world/BlockPlacement.cpp` is
// empty, and the Wall support that exposes the gap arrived later), and redstone
// dust has never had a block item.
//
//   (1) A lever cannot be placed at all, in any orientation. It declares a bare
//       `state(Facing, 6)` — neither hasDirectionalFacing nor hasHorizontalFacing
//       — so placementOrientation fell through to defaultOrientation and every
//       lever was oriented the same way, whichever wall was clicked. Its Wall
//       support then checked one fixed, wrong cell and always failed.
//
//   (2) Redstone dust could be placed as a *block* but not as the item players
//       actually hold: `items::Redstone` was a plain Item with no block mapping,
//       where vanilla's is `new BlockItem(Blocks.REDSTONE_WIRE)` registered under
//       the name `redstone`. The name and the block genuinely differ, which is
//       the case BlockItem could not express.

#include "gameplay/ItemPlacement.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "world/Block.hpp"
#include "world/BlockPlacement.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <cassert>
#include <utility>

namespace {

using mc::world::Block;
using mc::world::BlockOrientation;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::World;

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

// Clicking the face `face` of the block at `clicked`, landing in the cell just
// outside it.
[[nodiscard]] mc::world::PlacementContext against(glm::ivec3 clicked, BlockOrientation face) {
    const auto offset = mc::world::orientationOffset(face);
    mc::world::PlacementContext context;
    context.clickedBlock = clicked;
    context.placePosition = clicked + offset;
    context.clickedFace = face;
    context.hitPosition = glm::vec3{context.placePosition} + glm::vec3{0.5F};
    context.lookDirection = -glm::vec3{offset};
    return context;
}

} // namespace

int main() {
    // --- (1) A lever on each of the four walls of a pillar. ---
    {
        World world = floored();
        world.setState(8, 1, 8, BlockState{Block::Stone}); // the pillar to hang off
        for (const auto face : {BlockOrientation::North, BlockOrientation::East,
                                BlockOrientation::South, BlockOrientation::West}) {
            const auto context = against({8, 1, 8}, face);
            const auto placed = mc::world::placementBlock(world, Block::Lever, context);
            assert(placed.has_value()); // RED before the fix, for every face
            assert(placed->block() == Block::Lever);
            // Its FACING is the side it protrudes away from — the clicked face
            // itself, WallTorch's and the button's convention.
            assert(placed->orientation() == face);
            // ...and the state it produced really does survive where it landed,
            // which is what "has_value" alone would not prove.
            assert(mc::world::canBlockSurvive(world, context.placePosition, Block::Lever,
                                              placed->orientation()));
        }

        // A lever aimed at open air still fails, so the fix is "orient it right",
        // not "stop checking support".
        World bare = floored();
        auto midair = against({8, 5, 8}, BlockOrientation::North);
        assert(!mc::world::placementBlock(bare, Block::Lever, midair).has_value());
    }

    // --- (2) Redstone dust, held as the item a player actually has. ---
    {
        World world = floored();
        const auto context = against({8, 0, 8}, BlockOrientation::Up);

        // The block layer was never the problem.
        const auto asBlock = mc::world::placementBlock(world, Block::RedstoneWire, context);
        assert(asBlock.has_value() && asBlock->block() == Block::RedstoneWire);

        // The item is what could not place. This is the RED assertion.
        const mc::gameplay::ItemStack stack{Block::Air, 1U, &mc::gameplay::items::Redstone};
        const auto placed = mc::gameplay::itemPlacementBlock(world, stack, context);
        assert(placed.has_value());
        assert(placed->block() == Block::RedstoneWire);

        // The creative block-stack path (a null item pointer naming the block)
        // could already place it before the fix — via a generated BlockItem
        // literally called "redstone_wire". So the symptom players saw was
        // "creative places it, the redstone in my hand does not", and the
        // catalog carried two entries for one block. It still works, and now it
        // resolves to the same single item.
        const mc::gameplay::ItemStack legacy{Block::RedstoneWire, 1U, nullptr};
        const auto viaBlockStack = mc::gameplay::itemPlacementBlock(world, legacy, context);
        assert(viaBlockStack.has_value() && viaBlockStack->block() == Block::RedstoneWire);
        assert(mc::gameplay::blockItemFor(Block::RedstoneWire) == &mc::gameplay::items::Redstone);

        // And the item keeps its own name. Renaming it to `redstone_wire` would
        // "fix" placement while breaking every recipe, drop and JE name mapping
        // that spells it `redstone`.
        assert(mc::gameplay::items::Redstone.identifier.path == "redstone");
    }

    return 0;
}

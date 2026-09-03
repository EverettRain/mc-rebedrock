// AR-B4-0b: the two collision walks are column-major — (z, x) outside, y inside,
// one chunk pointer per column. This is a pure restructuring of
// PlayerController::collidesAtHeight and EntitySystem::boxIntersectsWorld, so
// what it needs is *equivalence* evidence, not new behaviour: the cases below
// pin the semantics that the per-cell `World::state` calls used to provide for
// free and that a hoisted chunk pointer now has to reproduce by hand.
//
// Three of them are only observable once the chunk lookup is hoisted:
//   - the deliberate asymmetry (an unloaded column is SOLID to the player so it
//     cannot fall through a streaming seam, but AIR to a creature),
//   - a query box straddling a chunk boundary with one side unloaded,
//   - negative world coordinates, where a truncating `x % 16` yields a negative
//     local index and a `>> 4`-derived origin silently reads the wrong column.
//
// The entity walk had no direct test coverage at all before this file.

#include "gameplay/EntitySystem.hpp"
#include "gameplay/PlayerController.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <cassert>
#include <utility>

namespace {

using mc::world::Block;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::World;

constexpr int kFloorY = 4;
constexpr mc::gameplay::entities::EntityDimensions kZombie{0.6F, 1.95F};

// A chunk with a stone floor at kFloorY and air above it.
[[nodiscard]] Chunk flooredChunk() {
    Chunk chunk;
    for (int z = 0; z < mc::world::kChunkDepth; ++z) {
        for (int x = 0; x < mc::world::kChunkWidth; ++x) {
            chunk.setBlock(x, kFloorY, z, Block::Stone);
        }
    }
    return chunk;
}

// Whether a standing player fits with its feet at (x, y, z).
[[nodiscard]] bool playerFits(const World& world, float x, float y, float z) {
    mc::gameplay::PlayerController player({x, y, z});
    return player.canStandUp(world);
}

[[nodiscard]] bool entityFits(const World& world, float x, float y, float z) {
    return mc::gameplay::EntitySystem::canOccupy(world, {x, y, z}, kZombie);
}

} // namespace

int main() {
    const float standY = static_cast<float>(kFloorY) + 1.0F;

    // --- Baseline: a loaded column with air above the floor takes both bodies. ---
    {
        World world;
        world.setChunk({0, 0}, flooredChunk());
        assert(playerFits(world, 8.5F, standY, 8.5F));
        assert(entityFits(world, 8.5F, standY, 8.5F));
    }

    // --- The asymmetry, stated outright. An unloaded column is solid to the
    // player (PlayerController's rule: never fall through a streaming seam) and
    // air to a creature (EntitySystem reads whatever World::state answered, and
    // that is air). The column-major walk has to reproduce both from one
    // `world.chunk(...) == nullptr` test, where before they fell out of two
    // different per-cell helpers. ---
    {
        World empty;
        assert(!playerFits(empty, 8.5F, standY, 8.5F)); // solid seam
        assert(entityFits(empty, 8.5F, standY, 8.5F));  // open air
    }

    // --- A box straddling a chunk boundary with one side unloaded. The body is
    // centred on x=16.0, so it spans cell 15 (chunk 0, loaded and clear) and
    // cell 16 (chunk 1, absent). One column resolving to nullptr must still
    // stop the player, i.e. the per-column pointer is genuinely re-resolved
    // when x crosses into the next chunk rather than reused from the last one. ---
    {
        World seam;
        seam.setChunk({0, 0}, flooredChunk());
        assert(!playerFits(seam, 16.0F, standY, 8.5F)); // half the body is over the seam
        assert(playerFits(seam, 8.5F, standY, 8.5F));   // wholly inside the loaded chunk
        // Same geometry the other way round: load chunk 1 as well and it clears.
        seam.setChunk({1, 0}, flooredChunk());
        assert(playerFits(seam, 16.0F, standY, 8.5F));
        // And on the Z axis, which is the outer loop rather than the inner one.
        World seamZ;
        seamZ.setChunk({0, 0}, flooredChunk());
        assert(!playerFits(seamZ, 8.5F, standY, 16.0F));
        seamZ.setChunk({0, 1}, flooredChunk());
        assert(playerFits(seamZ, 8.5F, standY, 16.0F));
    }

    // --- Negative world coordinates. Chunk {-1,-1} covers world x,z in
    // [-16,-1], so world x=-1 is local 15 — a truncating `x % 16` would say -1
    // and read air, and an origin computed without a floor division would land
    // in the wrong chunk entirely. A pillar at (-1, *, -1) must be felt, and the
    // cell beside it must not be. ---
    {
        World negative;
        negative.setChunk({-1, -1}, flooredChunk());
        negative.setBlock(-1, kFloorY + 1, -1, Block::Stone);
        negative.setBlock(-1, kFloorY + 2, -1, Block::Stone);
        assert(!playerFits(negative, -0.5F, standY, -0.5F)); // standing in the pillar
        assert(!entityFits(negative, -0.5F, standY, -0.5F));
        assert(playerFits(negative, -3.5F, standY, -3.5F)); // three cells clear of it
        assert(entityFits(negative, -3.5F, standY, -3.5F));
        // The pillar really is where the test thinks it is.
        assert(negative.block(-1, kFloorY + 1, -1) == Block::Stone);
        assert(negative.block(-2, kFloorY + 1, -2) == Block::Air);
    }

    // --- AR-B4-0's row-below scan, now that it rides the same column loop:
    // a closed fence gate one cell below the feet still reaches up into them,
    // including when the gate sits in a different chunk from the body's centre. ---
    {
        World gates;
        gates.setChunk({0, 0}, flooredChunk());
        gates.setChunk({1, 0}, flooredChunk());
        const BlockState gate{Block::OakFenceGate};
        // Feet at kFloorY+2 means the body's own cells are kFloorY+2 and above;
        // the gate's cell is kFloorY+1, one row below, and its 1.5-cell box
        // reaches half a cell into the body.
        const float ledgeY = static_cast<float>(kFloorY) + 2.0F;
        gates.setState(8, kFloorY + 1, 8, gate);
        assert(!playerFits(gates, 8.5F, ledgeY, 8.5F));
        assert(!entityFits(gates, 8.5F, ledgeY, 8.5F));
        // Across the chunk seam: the gate is the last cell of chunk 0 and the
        // body straddles into chunk 1.
        gates.setState(15, kFloorY + 1, 8, gate);
        assert(!playerFits(gates, 16.0F, ledgeY, 8.5F));
        assert(!entityFits(gates, 16.0F, ledgeY, 8.5F));
        // An open gate in the same place is passable at that height, so the
        // row-below scan is not simply reporting "something is down there".
        gates.setState(8, kFloorY + 1, 8, gate.withOpen(true));
        assert(playerFits(gates, 8.5F, ledgeY, 8.5F));
        assert(entityFits(gates, 8.5F, ledgeY, 8.5F));
    }

    // --- The world's floor and ceiling, which the player walk answers itself
    // rather than through a chunk: below kMinY is solid, above kMaxY is air. ---
    {
        World world;
        world.setChunk({0, 0}, flooredChunk());
        assert(!playerFits(world, 8.5F, static_cast<float>(mc::world::kMinY) - 2.0F, 8.5F));
        assert(playerFits(world, 8.5F, static_cast<float>(mc::world::kMaxY) + 2.0F, 8.5F));
    }

    return 0;
}

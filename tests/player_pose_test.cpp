// PX-3: the player pose state machine (LivingEntity#updatePlayerPose). The pose
// is resolved once per tick from the shift input and the headroom overhead, and
// it — not the raw shift key — drives the collision height, the eye height and
// the crouch the render snapshot carries. The load-bearing behaviour is the
// forced crouch: releasing shift under a 1.5-high ceiling must keep the player
// crouched (no head-clip / no snap up) until it can actually stand.
//
// Headless: a small flat world plus a couple of blocks overhead, then assert the
// resolved Pose and the collision height. No renderer, no Vulkan.

#include "gameplay/PlayerController.hpp"

#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <cassert>
#include <cmath>
#include <utility>

using mc::gameplay::PlayerController;
using mc::gameplay::PlayerInput;
using mc::gameplay::Pose;

namespace {

// A 16x16 stone floor at y=0 so a player at y=1 stands on the ground.
mc::world::World flatWorld() {
    mc::world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    mc::world::World world;
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

PlayerInput idleInput() {
    PlayerInput in;
    in.sprintAllowed = true;
    return in;
}

// The collision height a controller reports through its public surface: the eye
// height distinguishes crouch (1.27) from stand (1.62), and it is pose-driven.
bool crouchEye(const PlayerController& p) {
    return std::abs(p.eyeHeight() - PlayerController::kSneakingEyeHeight) < 0.001F;
}
bool standEye(const PlayerController& p) {
    return std::abs(p.eyeHeight() - PlayerController::kEyeHeight) < 0.001F;
}

}  // namespace

int main() {
    mc::world::World world = flatWorld();

    // --- Default is Standing; the eye/collision height is the full body ---------
    {
        PlayerController p({8.5F, 1.0F, 8.5F});
        p.tick(world, idleInput());
        assert(p.pose() == Pose::Standing);
        assert(!p.sneaking());
        assert(standEye(p));
        assert(p.canStandUp(world));
    }

    // --- Holding shift crouches; releasing it in open space stands back up ------
    {
        PlayerController p({8.5F, 1.0F, 8.5F});
        PlayerInput crouch = idleInput();
        crouch.sneakHeld = true;
        p.tick(world, crouch);
        assert(p.pose() == Pose::Crouching);
        assert(p.sneaking());
        assert(crouchEye(p));

        // Release: open headroom, so the player stands again the same tick.
        p.tick(world, idleInput());
        assert(p.pose() == Pose::Standing);
        assert(!p.sneaking());
        assert(standEye(p));
    }

    // --- Forced crouch: a 1.5-high ceiling keeps the player crouched even after
    // shift is released. A TOP slab in the cell at y=2 has its box at [2.5, 3.0],
    // so the floor-to-ceiling gap over the feet (y=1) is exactly 1.5: the 1.8
    // standing body does not fit, but the 1.5 crouch body does. Vanilla keeps the
    // pose Crouching until there is room to stand, instead of snapping up (which
    // would clip the head into the slab / jitter the body). -----------------------
    {
        mc::world::World ceiling = flatWorld();
        ceiling.setState(8, 2, 8,
                         mc::world::BlockState{mc::world::Block::OakSlab}.withSlabPortion(
                             mc::world::SlabPortion::Top));
        PlayerController p({8.5F, 1.0F, 8.5F});

        // First crouch under the slab.
        PlayerInput crouch = idleInput();
        crouch.sneakHeld = true;
        p.tick(ceiling, crouch);
        assert(p.pose() == Pose::Crouching);
        assert(crouchEye(p));
        // The standing body would not fit here.
        assert(!p.canStandUp(ceiling));

        // Release shift: the player must STAY crouched (forced), not pop up.
        for (int t = 0; t < 5; ++t) {
            p.tick(ceiling, idleInput());
        }
        assert(p.pose() == Pose::Crouching);   // held down by the low ceiling
        assert(p.sneaking());                  // still reads as sneaking
        assert(crouchEye(p));                  // and still at the crouch height
    }

    // --- Pose drives the collision height, not the raw shift. A body-tall gap the
    // standing player cannot pass but the crouching one can proves the collision
    // box shrinks with the pose. The player starts crouched under the slab; moving
    // forward it stays under the slab (crouch box clears the [2.5,3.0] ceiling),
    // whereas a 1.8 body would be blocked from occupying that cell at all. --------
    {
        mc::world::World tunnel = flatWorld();
        // A run of top slabs forming a 1.5-high tunnel roof along +X.
        for (int x = 8; x <= 12; ++x) {
            tunnel.setState(x, 2, 8,
                            mc::world::BlockState{mc::world::Block::OakSlab}.withSlabPortion(
                                mc::world::SlabPortion::Top));
        }
        PlayerController p({8.5F, 1.0F, 8.5F});
        PlayerInput crawl = idleInput();
        crawl.sneakHeld = true;
        crawl.forward = 1.0F;
        crawl.lookDirection = {1.0F, 0.0F, 0.0F};
        for (int t = 0; t < 60; ++t) {
            p.tick(tunnel, crawl);
        }
        // It crawled forward under the roof and never got shoved up out of it: the
        // crouch collision box (top 2.5) cleared the slab bottom (2.5), and the
        // feet stayed on the floor.
        assert(p.position().x > 9.0F);
        assert(std::abs(p.position().y - 1.0F) < 0.05F);
        assert(p.pose() == Pose::Crouching);
    }

    // --- canStandUp is a pure headroom query independent of the current pose. ----
    {
        mc::world::World ceiling = flatWorld();
        ceiling.setState(8, 2, 8,
                         mc::world::BlockState{mc::world::Block::OakSlab}.withSlabPortion(
                             mc::world::SlabPortion::Top));
        // Under the slab: cannot stand.
        PlayerController under({8.5F, 1.0F, 8.5F});
        assert(!under.canStandUp(ceiling));
        // One cell over, open sky: can stand.
        PlayerController open({10.5F, 1.0F, 10.5F});
        assert(open.canStandUp(ceiling));
    }

    return 0;
}

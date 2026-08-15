#include "gameplay/PlayerController.hpp"

#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <cassert>
#include <cmath>
#include <utility>

// Exercises the player's collision move: the vanilla per-axis resolution and the
// Entity#maxUpStep retry. The player keeps vanilla's 0.6 step, so a full
// one-block rise needs a jump — either the space bar or the optional auto-jump.
int main() {
    mc::world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    mc::world::World world;
    world.setChunk({0, 0}, std::move(chunk));

    // A grounded player resting on the floor stays put and reads onGround.
    mc::gameplay::PlayerController resting({8.5F, 1.0F, 8.5F});
    mc::gameplay::PlayerInput idle;
    idle.sprintAllowed = true;
    for (int tick = 0; tick < 20; ++tick) {
        resting.tick(world, idle);
    }
    assert(resting.onGround());
    assert(std::abs(resting.position().y - 1.0F) < 0.01F);

    // A 1-high block at (10,1,10): its top sits at y=2. Walking into it does NOT
    // lift the body — the player's step is vanilla's 0.6, never a full block, so
    // the rise requires a jump rather than an automatic climb.
    world.setBlock(10, 1, 10, mc::world::Block::Stone);
    mc::gameplay::PlayerController climber({9.3F, 1.0F, 9.3F});
    mc::gameplay::PlayerInput diagonal;
    diagonal.forward = 0.7F;
    diagonal.strafe = 0.7F;
    diagonal.lookDirection = {1.0F, 0.0F, 0.0F};
    diagonal.sprintAllowed = true;
    for (int tick = 0; tick < 30; ++tick) {
        climber.tick(world, diagonal);
    }
    assert(std::abs(climber.position().y - 1.0F) < 0.1F);

    // With auto-jump enabled the same walk climbs the rise on its own: the
    // horizontal collision triggers the jump and the body lands on top.
    mc::gameplay::PlayerController autoClimber({9.3F, 1.0F, 9.3F});
    mc::gameplay::PlayerInput autoInput;
    autoInput.forward = 0.7F;
    autoInput.strafe = 0.7F;
    autoInput.lookDirection = {1.0F, 0.0F, 0.0F};
    autoInput.sprintAllowed = true;
    autoInput.autoJump = true;
    bool reachedTop = false;
    for (int tick = 0; tick < 60; ++tick) {
        autoClimber.tick(world, autoInput);
        if (std::abs(autoClimber.position().y - 2.0F) < 0.05F) {
            reachedTop = true;
            break;
        }
    }
    assert(reachedTop);

    // A wall (a 2-high pillar) still blocks the player even with auto-jump: the
    // jump check sees no headroom and never fires, so the body stays grounded.
    world.setBlock(14, 1, 8, mc::world::Block::Stone);
    world.setBlock(14, 2, 8, mc::world::Block::Stone);
    mc::gameplay::PlayerController blocked({12.7F, 1.0F, 8.5F});
    mc::gameplay::PlayerInput intoWall;
    intoWall.forward = 1.0F;
    intoWall.lookDirection = {1.0F, 0.0F, 0.0F};
    intoWall.sprintAllowed = true;
    intoWall.autoJump = true;
    for (int tick = 0; tick < 30; ++tick) {
        blocked.tick(world, intoWall);
    }
    // The wall's west face is at x=14; the 0.6-wide body stops half a body
    // width short of it (center 13.7) and the two-high rise is not jumpable.
    assert(blocked.position().x < 13.8F);
    assert(blocked.position().y < 1.5F);

    // Farmland is a 15/16-high box, so the player rests a sixteenth lower on it
    // than on a full block (vanilla FarmlandBlock.SHAPE).
    mc::world::Chunk farmChunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            farmChunk.setBlock(x, 0, z, mc::world::Block::Farmland);
        }
    }
    mc::world::World farmWorld;
    farmWorld.setChunk({0, 0}, std::move(farmChunk));
    mc::gameplay::PlayerController onFarmland({8.5F, 1.0F, 8.5F});
    for (int tick = 0; tick < 40; ++tick) {
        onFarmland.tick(farmWorld, idle);
    }
    assert(onFarmland.onGround());
    assert(std::abs(onFarmland.position().y - (15.0F / 16.0F)) < 0.01F);

    // Entity.fallDistance: a player that falls onto the floor reports the fall
    // on the landing tick (the farmland trample reads it there), then resets to
    // zero once grounded.
    mc::gameplay::PlayerController falling({8.5F, 3.0F, 8.5F});
    mc::gameplay::PlayerInput none;
    none.sprintAllowed = true;
    bool sawLandingFall = false;
    for (int tick = 0; tick < 80; ++tick) {
        falling.tick(world, none);
        if (falling.onGround() && falling.fallDistance() > 0.5F) {
            sawLandingFall = true;
        }
    }
    assert(sawLandingFall);
    assert(falling.onGround());
    assert(falling.fallDistance() == 0.0F);

    // Creative flight uses 26.1's seven-tick double-tap window. One press plus
    // a held key never toggles flight; a real second press inside the window
    // does, while a press after the seventh tick starts a fresh window.
    mc::gameplay::PlayerInput creativeJump;
    creativeJump.flightAllowed = true;
    creativeJump.sprintAllowed = true;
    creativeJump.jumpHeld = true;
    creativeJump.jumpPressed = true;

    mc::gameplay::PlayerController doubleTap({6.5F, 1.0F, 6.5F});
    for (int tick = 0; tick < 2; ++tick) {
        doubleTap.tick(world, idle);
    }
    doubleTap.tick(world, creativeJump);
    assert(!doubleTap.flying());
    creativeJump.jumpHeld = false;
    creativeJump.jumpPressed = false;
    doubleTap.tick(world, creativeJump);
    creativeJump.jumpHeld = true;
    creativeJump.jumpPressed = true;
    doubleTap.tick(world, creativeJump);
    assert(doubleTap.flying());

    mc::gameplay::PlayerController expiredTap({4.5F, 1.0F, 4.5F});
    for (int tick = 0; tick < 2; ++tick) {
        expiredTap.tick(world, idle);
    }
    expiredTap.tick(world, creativeJump);
    assert(!expiredTap.flying());
    creativeJump.jumpHeld = false;
    creativeJump.jumpPressed = false;
    for (int tick = 0;
         tick < mc::gameplay::PlayerController::kCreativeFlightToggleWindowTicks; ++tick) {
        expiredTap.tick(world, creativeJump);
    }
    creativeJump.jumpHeld = true;
    creativeJump.jumpPressed = true;
    expiredTap.tick(world, creativeJump);
    assert(!expiredTap.flying());

    return 0;
}

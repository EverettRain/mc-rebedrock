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

    // A slab's half box holds the player at the slab's own surface, not a full
    // block's top. A bottom slab on the floor (its box [1.0, 1.5]) rests a
    // falling player at y=1.5; the open upper half is walkable, so the same
    // player is not pushed up to y=2 the way a full block would.
    world.setState(2, 1, 2,
                   mc::world::BlockState{mc::world::Block::OakSlab}.withSlabPortion(
                       mc::world::SlabPortion::Bottom));
    mc::gameplay::PlayerController onSlab({2.5F, 3.0F, 2.5F});
    for (int tick = 0; tick < 40; ++tick) {
        onSlab.tick(world, idle);
    }
    assert(onSlab.onGround());
    assert(std::abs(onSlab.position().y - 1.5F) < 0.02F);

    // A top slab in the air (box [1.5, 2.0]) leaves its lower half open: a player
    // standing on the floor below it at y=1 is not lifted by the slab overhead.
    world.setState(3, 1, 3,
                   mc::world::BlockState{mc::world::Block::OakSlab}.withSlabPortion(
                       mc::world::SlabPortion::Top));
    mc::gameplay::PlayerController underTopSlab({3.5F, 1.0F, 3.5F});
    for (int tick = 0; tick < 20; ++tick) {
        underTopSlab.tick(world, idle);
    }
    assert(std::abs(underTopSlab.position().y - 1.0F) < 0.02F);

    // Placement occupancy respects the would-be block's box, not a full cube. A
    // player standing in the upper half of a cell (feet at y=1.5) blocks a full
    // block there, and blocks a top slab (whose box is that same upper half), but
    // does not block a bottom slab dropped into the empty lower half.
    {
        mc::gameplay::PlayerController probe({0.5F, 1.5F, 0.5F});
        assert(probe.intersectsBlock(0, 1, 0));               // full cube [1,2]
        assert(probe.intersectsBlock(0, 1, 0, 0.5F, 1.0F));   // top slab   [1.5,2]
        assert(!probe.intersectsBlock(0, 1, 0, 0.0F, 0.5F));  // bottom slab[1,1.5]
    }

    // A double slab is a full cube again: it rests the player a whole block up.
    world.setState(2, 1, 2,
                   mc::world::BlockState{mc::world::Block::OakSlab}.withSlabPortion(
                       mc::world::SlabPortion::Double));
    mc::gameplay::PlayerController onDouble({2.5F, 3.0F, 2.5F});
    for (int tick = 0; tick < 40; ++tick) {
        onDouble.tick(world, idle);
    }
    assert(std::abs(onDouble.position().y - 2.0F) < 0.02F);

    // Auto-stepping up a partial rise keeps a sprint. A sprinter crossing a row
    // of bottom slabs (each a 0.5 step) lifts onto them without the step counting
    // as a horizontal collision — the flag that would otherwise cancel the
    // sprint the next tick. A row (not one) so the player is still on a slab,
    // mid-run, when the assertions read.
    {
        mc::world::Chunk slabRun;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                slabRun.setBlock(x, 0, z, mc::world::Block::Stone);
            }
        }
        mc::world::World runWorld;
        runWorld.setChunk({0, 0}, std::move(slabRun));
        for (int x = 6; x < 14; ++x) {
            runWorld.setState(x, 1, 8,
                              mc::world::BlockState{mc::world::Block::OakSlab}.withSlabPortion(
                                  mc::world::SlabPortion::Bottom));
        }
        mc::gameplay::PlayerController sprinter({4.5F, 1.0F, 8.5F});
        mc::gameplay::PlayerInput run;
        run.forward = 1.0F;
        run.lookDirection = {1.0F, 0.0F, 0.0F};
        run.sprintHeld = true;
        run.sprintAllowed = true;
        bool steppedOnto = false;
        float speedBeforeStep = 0.0F;
        float speedAfterStep = -1.0F;
        float previousY = sprinter.position().y;
        float previousSpeed = 0.0F;
        for (int tick = 0; tick < 30; ++tick) {
            const float speedEnteringTick = std::abs(sprinter.velocity().x);
            sprinter.tick(runWorld, run);
            // The tick that lifts the player onto the first slab: the step must
            // preserve the horizontal speed, not zero it against the slab's side
            // (which would stutter the run to a crawl and rebuild). Capture the
            // speed just before that step and the speed left right after it.
            if (previousY < 1.4F && sprinter.position().y > 1.4F && speedAfterStep < 0.0F) {
                speedBeforeStep = previousSpeed;
                speedAfterStep = std::abs(sprinter.velocity().x);
            }
            previousY = sprinter.position().y;
            previousSpeed = speedEnteringTick;
            if (sprinter.position().y > 1.4F) {
                steppedOnto = true;
                assert(!sprinter.horizontalCollision());
                assert(sprinter.sprinting());
            }
        }
        assert(steppedOnto);
        // The speed going into the step was a real sprint (not a standstill), and
        // it survived the step rather than being zeroed against the slab: without
        // the velocity restore this drops to ~0 and the run visibly stutters.
        assert(speedBeforeStep > 0.05F);
        assert(speedAfterStep > 0.5F * speedBeforeStep);

    // A full wall still breaks the sprint: a one-block rise is not steppable, so
    // the collision flag stays set and sprinting stops.
        runWorld.setBlock(9, 1, 2, mc::world::Block::Stone);
        runWorld.setBlock(9, 2, 2, mc::world::Block::Stone);
        mc::gameplay::PlayerController intoWall({6.5F, 1.0F, 2.5F});
        mc::gameplay::PlayerInput runNoJump = run;
        runNoJump.autoJump = false;
        bool sprintBroke = false;
        for (int tick = 0; tick < 30; ++tick) {
            intoWall.tick(runWorld, runNoJump);
            if (intoWall.horizontalCollision() && !intoWall.sprinting()) {
                sprintBroke = true;
            }
        }
        assert(sprintBroke);
    }

    // ANIM A1/A2: the walk-animation drive quantities. Walking forward drives the
    // gait AMPLITUDE (walkAnimationSpeed) up toward saturation; stopping decays it
    // back to ~0 (the "stops -> limbs return to rest" root). The phase accumulates
    // monotonically by the amplitude, and the sprint flag is NOT a multiplier.
    {
        mc::world::Chunk floor;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                floor.setBlock(x, 0, z, mc::world::Block::Stone);
            }
        }
        mc::world::World flat;
        flat.setChunk({0, 0}, std::move(floor));
        // Start near one edge and walk toward the far side so the short run stays
        // on the loaded chunk (leaving it would freeze the player and confound the
        // amplitude reading).
        mc::gameplay::PlayerController walker({2.5F, 1.0F, 8.5F});
        mc::gameplay::PlayerInput forward;
        forward.forward = 1.0F;
        forward.lookDirection = {1.0F, 0.0F, 0.0F};
        forward.sprintAllowed = true;
        // At rest the amplitude is zero.
        assert(walker.walkAnimationSpeed() < 0.01F);
        // Walk a short while (staying on the chunk): the amplitude eases up off
        // zero and the phase accumulates monotonically.
        const float phaseStart = walker.walkAnimationPosition();
        for (int tick = 0; tick < 20; ++tick) {
            walker.tick(flat, forward);
        }
        const float movingAmp = walker.walkAnimationSpeed();
        assert(movingAmp > 0.01F);                    // the gait is active while moving
        assert(movingAmp <= 1.0F + 1e-4F);            // never exceeds saturation
        assert(walker.walkAnimationPosition() > phaseStart);  // phase advanced by amplitude

        // Stop: the amplitude decays back toward zero over a handful of ticks —
        // this is what returns the limbs to rest instead of freezing at the last
        // angle (the reported "stops but keeps swinging" root cause).
        mc::gameplay::PlayerInput stop;
        stop.sprintAllowed = true;
        for (int tick = 0; tick < 30; ++tick) {
            walker.tick(flat, stop);
        }
        assert(walker.walkAnimationSpeed() < movingAmp);
        assert(walker.walkAnimationSpeed() < 0.05F);  // decayed essentially to rest
    }

    // Steady-state movement speed matches 26.1 §18.1. The physics loop
    // (accel a/tick + drag g) converges to a/(1-g) blocks/tick; this pins that the
    // constants already produce vanilla speeds — walk 0.2159, sprint 0.2806, sneak
    // 0.0648 — so the animation amplitude min(4*d,1) lands on the spec values
    // (walk ~0.86, sprint ~1.0) with NO animation compensation. A short run on a
    // 16-wide chunk reaches steady state (~tick 10) without leaving the chunk.
    {
        mc::world::Chunk floor;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                floor.setBlock(x, 0, z, mc::world::Block::Stone);
            }
        }
        mc::world::World flat;
        flat.setChunk({0, 0}, std::move(floor));

        const auto steadySpeed = [&flat](const mc::gameplay::PlayerInput& in) {
            mc::gameplay::PlayerController p({2.5F, 1.0F, 8.5F});
            glm::vec3 prev = p.position();
            float last = 0.0F;
            for (int tick = 0; tick < 12; ++tick) {
                p.tick(flat, in);
                const glm::vec3 now = p.position();
                const float dx = now.x - prev.x;
                const float dz = now.z - prev.z;
                last = std::sqrt(dx * dx + dz * dz);
                prev = now;
            }
            return std::pair<float, float>{last, p.walkAnimationSpeed()};
        };

        mc::gameplay::PlayerInput walk;
        walk.forward = 1.0F;
        walk.lookDirection = {1.0F, 0.0F, 0.0F};
        walk.sprintAllowed = true;
        const auto [walkV, walkAmp] = steadySpeed(walk);
        assert(std::abs(walkV - 0.2159F) < 0.005F);   // 26.1 walk
        assert(std::abs(walkAmp - 0.863F) < 0.03F);   // min(4d,1) ~ 0.86

        mc::gameplay::PlayerInput sprint = walk;
        sprint.sprintHeld = true;
        const auto [sprintV, sprintAmp] = steadySpeed(sprint);
        assert(std::abs(sprintV - 0.2806F) < 0.005F);  // 26.1 sprint
        assert(sprintAmp > 0.98F);                     // saturates to ~1.0
        assert(sprintV > walkV);                       // sprint genuinely faster

        mc::gameplay::PlayerInput sneak = walk;
        sneak.sneakHeld = true;
        const auto [sneakV, sneakAmp] = steadySpeed(sneak);
        assert(std::abs(sneakV - 0.0648F) < 0.005F);   // 26.1 sneak
        assert(sneakAmp > 0.0F && sneakAmp < walkAmp); // smaller gait than walking
    }

    return 0;
}

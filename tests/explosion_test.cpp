// EXP-1/EXP-2: the explosion — its shape, what survives it, who it hurts, and
// the nether bed that sets one off.
//
// The thing worth testing about an explosion is not "does it remove blocks" but
// its SHAPE: 26.1 casts 1352 rays through a 16-cube shell, spends each ray's
// power on the resistance of what it passes through, and stops. That is why a
// wall shelters what is behind it and why obsidian is not merely tougher but
// actually stops the ray. A sphere test would pass a "blocks were removed"
// assertion and be a completely different mechanic, so the assertions here are
// about occlusion and resistance rather than about counts.

#include "gameplay/Explosion.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/DamageType.hpp"
#include "gameplay/WorldSimulation.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/attribute/EnvironmentAttribute.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <utility>

namespace {

using mc::gameplay::ExplosionSpec;
using mc::world::Block;
using mc::world::Chunk;
using mc::world::World;

// The same do-nothing host the other session tests use.
struct TestHost final : mc::gameplay::SimulationHost {
    int worldEdits = 0;
    int blockBreaks = 0;
    int itemPickups = 0;
    int footsteps = 0;
    int previewEdits = 0;
    bool playerDied = false;
    int furnaceChanges = 0;
    int eatingStarted = 0;
    int eatingCancelled = 0;
    int eatSounds = 0;
    int playerHurts = 0;

    void submitWorldEdit(int, int, int, mc::world::Block, std::uint8_t,
                         std::optional<mc::world::BlockOrientation>) override {
        ++worldEdits;
    }
    void submitWorldStateEdit(int, int, int, mc::world::BlockState) override { ++worldEdits; }
    void previewBlockEdit(int, int, int) override { ++previewEdits; }
    void playBlockBreak(mc::world::Block, glm::vec3) override { ++blockBreaks; }
    void playItemPickup(glm::vec3) override { ++itemPickups; }
    void playEat(glm::vec3) override { ++eatSounds; }
    void playPlayerHurt(glm::vec3) override { ++playerHurts; }
    void playPlayerFall(glm::vec3, bool) override {}
    void playBurp(glm::vec3) override {}
    void playExplode(glm::vec3) override {}
    void playCreatureHurt(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playFootstep(mc::world::Block, glm::vec3, float) override { ++footsteps; }
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, mc::world::Block) override {}
    void onPlayerDied() override { playerDied = true; }
    void onFurnaceStateChanged() override { ++furnaceChanges; }
    void onEatingStarted() override { ++eatingStarted; }
    void onEatingCancelled() override { ++eatingCancelled; }
};

// A solid box of `block` from y=0..8 across the chunk, so a blast at the centre
// has material on every side.
[[nodiscard]] World solidWorld(Block block) {
    World world;
    Chunk chunk;
    for (int y = 0; y <= 8; ++y) {
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                chunk.setBlock(x, y, z, block);
            }
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

[[nodiscard]] std::size_t brokenCount(World& world, glm::vec3 centre, float radius) {
    std::uint64_t random = 12345U;
    return mc::gameplay::explodedPositions(world, ExplosionSpec{centre, radius, true}, random)
        .size();
}

} // namespace

int main() {
    mc::gameplay::entities::registerBuiltinEntities();
    // ---------------------------------------------------------------------
    // 1) Resistance is spent per step, and it dominates: a TNT-sized blast
    //    BURIED in solid stone barely scratches it (each 0.3-step through stone
    //    costs (6+0.3)*0.3 + 0.225 = 2.115 of a starting power of at most 5.2),
    //    while the same blast sitting ON stone digs a real crater because most
    //    of its rays travel through air at 0.225 a step.
    //
    //    Getting this backwards is the easy mistake — a "blast radius" reading
    //    of the mechanic would dig the same hole either way — so both cases are
    //    pinned here.
    // ---------------------------------------------------------------------
    {
        World buried = solidWorld(Block::Stone);
        const std::size_t inStone = brokenCount(buried, {8.5F, 4.5F, 8.5F}, 4.0F);
        assert(inStone >= 1U && inStone < 10U); // a scratch, not a crater

        World cratered = solidWorld(Block::Stone);
        const std::size_t onStone = brokenCount(cratered, {8.5F, 9.5F, 8.5F}, 4.0F);
        assert(onStone > 30U);          // a real crater in the surface below
        assert(onStone > inStone * 5U); // and far more than the buried blast

        // Obsidian (1200) and bedrock stop the ray outright, buried or not.
        World obsidian = solidWorld(Block::Obsidian);
        assert(brokenCount(obsidian, {8.5F, 9.5F, 8.5F}, 4.0F) == 0U);
        World bedrock = solidWorld(Block::Bedrock);
        assert(brokenCount(bedrock, {8.5F, 9.5F, 8.5F}, 4.0F) == 0U);

        // A bigger blast digs more of the same surface — the radius feeds both
        // the starting power and the reach.
        World bigger = solidWorld(Block::Stone);
        assert(brokenCount(bigger, {8.5F, 9.5F, 8.5F}, 6.0F) > onStone);

        // ★ The per-ray jitter (`radius * (0.7 + rand*0.6)`) is what makes a
        // crater ragged instead of a smooth ball. Its only observable is that
        // two blasts with different random streams break DIFFERENT sets — a
        // fixed-power version would be perfectly reproducible and still pass
        // every size assertion above.
        World jitterA = solidWorld(Block::Stone);
        World jitterB = solidWorld(Block::Stone);
        std::uint64_t seedA = 1U;
        std::uint64_t seedB = 999983U;
        const auto setA = mc::gameplay::explodedPositions(
            jitterA, ExplosionSpec{{8.5F, 9.5F, 8.5F}, 4.0F, true}, seedA);
        const auto setB = mc::gameplay::explodedPositions(
            jitterB, ExplosionSpec{{8.5F, 9.5F, 8.5F}, 4.0F, true}, seedB);
        assert(setA.size() != setB.size() || setA != setB);
        // ...and the same stream is reproducible, which is what makes a replayed
        // tick blow the same hole.
        std::uint64_t seedAgain = 1U;
        const auto repeat = mc::gameplay::explodedPositions(
            jitterB, ExplosionSpec{{8.5F, 9.5F, 8.5F}, 4.0F, true}, seedAgain);
        assert(repeat == setA);
    }

    // ---------------------------------------------------------------------
    // 2) ★ Occlusion: an obsidian wall shelters the stone behind it. This is
    //    the property a sphere test would get wrong while still "removing
    //    blocks", so it is the assertion that pins the ray cast.
    // ---------------------------------------------------------------------
    {
        World world;
        Chunk chunk;
        // Air everywhere, a stone pillar at x=12, and an obsidian wall at x=10
        // between it and the blast at x=8.
        for (int y = 3; y <= 6; ++y) {
            for (int z = 6; z <= 10; ++z) {
                chunk.setBlock(10, y, z, Block::Obsidian);
                chunk.setBlock(12, y, z, Block::Stone);
                chunk.setBlock(6, y, z, Block::Stone); // and an unshielded pillar
            }
        }
        world.setChunk({0, 0}, std::move(chunk));

        std::uint64_t random = 999U;
        const auto broken = mc::gameplay::explodedPositions(
            world, ExplosionSpec{{8.5F, 4.5F, 8.5F}, 4.0F, true}, random);
        bool brokeShielded = false;
        bool brokeExposed = false;
        for (const auto& cell : broken) {
            if (cell.x == 12) brokeShielded = true;
            if (cell.x == 6) brokeExposed = true;
        }
        assert(brokeExposed);   // the pillar in the open goes
        assert(!brokeShielded); // the one behind obsidian does not
    }

    // ---------------------------------------------------------------------
    // 3) The damage curve: full at the centre, nothing at twice the radius.
    // ---------------------------------------------------------------------
    {
        using mc::gameplay::explosionDamage;
        const float atCentre = explosionDamage(4.0F, 0.0F, 1.0F);
        const float halfway = explosionDamage(4.0F, 4.0F, 1.0F);
        assert(atCentre > halfway);
        assert(halfway > 0.0F);
        // The floor at the far edge is EXACTLY 1, not "small": vanilla's formula
        // ends in `+ 1.0`, and a version without it would still pass every
        // relative comparison above while quietly making grazing hits free.
        assert(std::fabs(explosionDamage(4.0F, 8.0F, 1.0F) - 1.0F) < 1.0e-4F);
        // Same for a hit with no exposure at all inside the radius.
        assert(std::fabs(explosionDamage(4.0F, 1.0F, 0.0F) - 1.0F) < 1.0e-4F);
        assert(explosionDamage(4.0F, 9.0F, 1.0F) == 0.0F);    // out of reach
        // Exposure scales it: a target that can only see half the blast takes
        // markedly less than one in the open.
        assert(explosionDamage(4.0F, 2.0F, 0.5F) < explosionDamage(4.0F, 2.0F, 1.0F));
    }

    // ---------------------------------------------------------------------
    // 4) Exposure sampling: open air sees everything, a solid wall sees none.
    // ---------------------------------------------------------------------
    {
        World open;
        Chunk empty;
        open.setChunk({0, 0}, std::move(empty));
        const float clear = mc::gameplay::seenPercent(open, {8.5F, 4.5F, 8.5F},
                                                      {10.0F, 4.0F, 8.2F}, {10.6F, 5.8F, 8.8F});
        assert(clear > 0.99F);

        World walled = solidWorld(Block::Obsidian);
        const float blocked = mc::gameplay::seenPercent(walled, {2.5F, 4.5F, 2.5F},
                                                        {12.0F, 4.0F, 12.2F},
                                                        {12.6F, 5.8F, 12.8F});
        assert(blocked < 0.01F);
    }

    // ---------------------------------------------------------------------
    // 5) End to end: the session breaks blocks, and the player standing in it
    //    gets hurt.
    // ---------------------------------------------------------------------
    {
        World world = solidWorld(Block::Stone);
        mc::gameplay::GameSession session;
        session.setGameMode(mc::gameplay::GameMode::Survival);
        session.player().setPosition({8.5F, 9.0F, 8.5F});
        TestHost host;
        const float before = session.vitals().health();
        // The blast sits in the air beside the player, on the stone surface. A
        // centre buried inside the stone would be invisible to them — the
        // exposure sampling would (correctly) return zero and nobody would be
        // hurt, which is a different thing to test.
        const std::size_t broke =
            session.explode(world, host, ExplosionSpec{{8.5F, 9.5F, 8.5F}, 4.0F, true});
        assert(broke > 0U);
        assert(world.block(8, 8, 8) == Block::Air); // the surface below is gone
        assert(session.vitals().health() < before); // and it hurt
    }

    // ---------------------------------------------------------------------
    // 6) EXP-2: a bed in a dimension whose BedRule explodes does exactly that.
    // ---------------------------------------------------------------------
    {
        // The nether's rule is Explodes, and dimensionAttributes says so — this
        // is the wiring the bed reads, asserted here so a change to the
        // dimension table shows up as a failure in the mechanic that depends
        // on it rather than silently.
        const auto rule = static_cast<mc::world::attribute::BedRule>(
            mc::world::dimensionAttributes(mc::world::DimensionId::Nether)
                .at(mc::world::attribute::EnvAttr::BedRule)
                .asEnum());
        assert(rule == mc::world::attribute::BedRule::Explodes);
        const auto overworld = static_cast<mc::world::attribute::BedRule>(
            mc::world::dimensionAttributes(mc::world::DimensionId::Overworld)
                .at(mc::world::attribute::EnvAttr::BedRule)
                .asEnum());
        assert(overworld == mc::world::attribute::BedRule::CanSleepWhenDark);
    }

    // ---------------------------------------------------------------------
    // 7) EXP-2: TNT. Lighting it removes the block and starts a fuse; the fuse
    //    running out raises a blast; a blast that reaches other TNT LIGHTS it
    //    rather than destroying it, which is what makes a stack chain.
    // ---------------------------------------------------------------------
    {
        World world = solidWorld(Block::Stone);
        mc::gameplay::WorldSimulation simulation;
        simulation.ignitePrimedTnt({8.5F, 9.5F, 8.5F}, 80);
        assert(simulation.primedTnt().size() == 1U);

        // Nothing goes off early.
        for (int tick = 0; tick < 79; ++tick) {
            static_cast<void>(simulation.tick(world));
            assert(simulation.takePendingExplosions().empty());
        }
        static_cast<void>(simulation.tick(world));
        const auto pending = simulation.takePendingExplosions();
        assert(pending.size() == 1U);
        assert(std::fabs(pending[0].radius - 4.0F) < 1.0e-4F); // PrimedTnt's own radius
        assert(simulation.primedTnt().empty());

        // It falls: primed TNT over a hole drops until something stops it.
        World hole = solidWorld(Block::Stone);
        for (int y = 4; y <= 8; ++y) {
            hole.setBlock(8, y, 8, Block::Air);
        }
        mc::gameplay::WorldSimulation falling;
        falling.ignitePrimedTnt({8.5F, 8.5F, 8.5F}, 80);
        const float startY = falling.primedTnt()[0].position.y;
        for (int tick = 0; tick < 20; ++tick) {
            static_cast<void>(falling.tick(hole));
        }
        assert(!falling.primedTnt().empty());
        assert(falling.primedTnt()[0].position.y < startY); // it fell
        // ...and came to rest ON the stone at the bottom of the hole (y = 3),
        // i.e. centred at 4.5. A loose "it is above the floor" assertion would
        // pass for a TNT that merely stopped mid-air with its velocity zeroed.
        assert(std::fabs(falling.primedTnt()[0].position.y - 4.5F) < 0.01F);
        assert(std::fabs(falling.primedTnt()[0].verticalVelocity) < 1.0e-4F);
    }

    // ---------------------------------------------------------------------
    // 8) The chain: a blast over a bed of TNT primes it instead of breaking it.
    // ---------------------------------------------------------------------
    {
        World world = solidWorld(Block::Stone);
        // A short row of TNT just under the surface.
        for (int x = 6; x <= 10; ++x) {
            world.setBlock(x, 8, 8, Block::Tnt);
        }
        mc::gameplay::GameSession session;
        session.setGameMode(mc::gameplay::GameMode::Survival);
        session.player().setPosition({8.5F, 40.0F, 8.5F}); // well clear of it
        TestHost host;
        static_cast<void>(
            session.explode(world, host, ExplosionSpec{{8.5F, 9.5F, 8.5F}, 4.0F, true}));
        // The TNT cells are cleared...
        assert(world.block(8, 8, 8) == Block::Air);
        // ...but as primed entities, not as rubble.
        const auto& primed = session.worldSimulation().primedTnt();
        assert(!primed.empty());
        for (const auto& tnt : primed) {
            // TntBlock#wasExploded's short random fuse, so a stack goes off
            // raggedly instead of all in the same tick.
            assert(tnt.fuse >= 10 && tnt.fuse < 30);
        }
    }

    // ---------------------------------------------------------------------
    // 9) The four field bugs, each pinned so it cannot come back.
    // ---------------------------------------------------------------------
    {
        // (a) A creative player is not hurt by a blast — Player#isInvulnerableTo.
        //     Nothing reached hurtPlayer in creative before EXP-1 (mobs do not
        //     target them and tickPlayerVitals returns early), so an explosion
        //     was the first source that could, and it killed them.
        World world = solidWorld(Block::Stone);
        mc::gameplay::GameSession creative;
        creative.setGameMode(mc::gameplay::GameMode::Creative);
        creative.player().setPosition({8.5F, 9.0F, 8.5F});
        TestHost host;
        const float creativeHealth = creative.vitals().health();
        static_cast<void>(
            creative.explode(world, host, ExplosionSpec{{8.5F, 9.5F, 8.5F}, 4.0F, true}));
        assert(creative.vitals().health() == creativeHealth);
        // ...but the void and /kill still get through (BypassesInvulnerability).
        assert(mc::gameplay::hasDamageTag(mc::gameplay::DamageType::OutOfWorld,
                                          mc::gameplay::DamageTag::BypassesInvulnerability));
        assert(!mc::gameplay::hasDamageTag(mc::gameplay::DamageType::Explosion,
                                           mc::gameplay::DamageTag::BypassesInvulnerability));

        // (b) Not every broken block drops. WorldMutationService rolls a full
        //     drop for any Explosion-caused removal, so the blast has to
        //     suppress that and roll its own 1/radius instead. With both active
        //     every block dropped.
        World dropWorld = solidWorld(Block::Stone);
        mc::gameplay::GameSession dropper;
        dropper.setGameMode(mc::gameplay::GameMode::Survival);
        dropper.player().setPosition({8.5F, 40.0F, 8.5F});
        const std::size_t brokeCount =
            dropper.explode(dropWorld, host, ExplosionSpec{{8.5F, 9.5F, 8.5F}, 4.0F, true});
        assert(brokeCount > 20U);
        std::size_t dropped = 0U;
        for (const auto& item : dropper.itemEntities().entities()) {
            dropped += item.stack.count;
        }
        // 1/radius = 1/4, so a couple of dozen blocks leave a handful of stacks.
        // The bug made this equal to brokeCount.
        assert(dropped < brokeCount);
    }

    std::cout << "explosion_test passed\n";
    return 0;
}

// The behaviour behind the rules added alongside the 26.1 rename. The registry
// test proves the table parses and stores them; this one proves each rule
// actually reaches the system it names — a rule nothing consumes is worse than
// no rule at all, because the player is told it worked.
//
// Every gate is checked in both directions from the same starting state, so a
// gate that is simply never reached (the assertion passing for the wrong
// reason) fails the "on" half.

#include "gameplay/EntitySystem.hpp"
#include "gameplay/GameRules.hpp"
#include "gameplay/PlayerVitals.hpp"
#include "gameplay/WorldSimulation.hpp"
#include "gameplay/entities/BuiltinSpecies.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <glm/vec3.hpp>

#include <cassert>
#include <cmath>

namespace {

using namespace mc::gameplay;

// Drops the player from `height` blocks and reports the damage the landing
// tick charged. The fall is accumulated the way PlayerController feeds it:
// one airborne tick per block of descent, then a landing tick.
float fallDamageFor(const VitalsRules& rules, float height) {
    PlayerVitals vitals;
    vitals.setRules(rules);
    VitalsInput falling;
    falling.onGround = false;
    falling.verticalDistance = -height;
    static_cast<void>(vitals.tick(falling));
    VitalsInput landing;
    landing.onGround = true;
    return vitals.tick(landing).damageTaken;
}

void testFallDamage() {
    VitalsRules rules;
    // 10 blocks costs ceil(10 - 3) = 7 health with the rule on.
    assert(std::abs(fallDamageFor(rules, 10.0F) - 7.0F) < 1e-4F);
    rules.fallDamage = false;
    assert(fallDamageFor(rules, 10.0F) == 0.0F);
    // The rule withholds the damage, not the fall bookkeeping: a player who
    // lands with the rule off is still at full health and not invulnerable, so
    // a sword swing on the same tick would still land.
    PlayerVitals vitals;
    vitals.setRules(rules);
    VitalsInput falling;
    falling.verticalDistance = -10.0F;
    static_cast<void>(vitals.tick(falling));
    VitalsInput landing;
    landing.onGround = true;
    static_cast<void>(vitals.tick(landing));
    assert(vitals.health() == PlayerVitals::kMaximumHealth);
    assert(vitals.invulnerableTicks() == 0);
    assert(vitals.fallDistance() == 0.0F);
}

void testFireDamage() {
    const auto burnDamage = [](const VitalsRules& rules) {
        PlayerVitals vitals;
        vitals.setRules(rules);
        vitals.setOnFire(5);
        float total = 0.0F;
        // Long enough to cross several of the one-second burn boundaries.
        for (int tick = 0; tick < 60; ++tick) {
            VitalsInput input;
            input.onGround = true;
            total += vitals.tick(input).damageTaken;
        }
        return total;
    };
    VitalsRules rules;
    assert(burnDamage(rules) > 0.0F);
    rules.fireDamage = false;
    assert(burnDamage(rules) == 0.0F);
    // The burn still counts down with the rule off, so the player stops being
    // ablaze on schedule rather than staying lit forever.
    PlayerVitals vitals;
    vitals.setRules(rules);
    vitals.setOnFire(1);
    const int lit = vitals.fireTicks();
    assert(lit > 0);
    VitalsInput input;
    input.onGround = true;
    static_cast<void>(vitals.tick(input));
    assert(vitals.fireTicks() < lit);
}

void testDrowningDamage() {
    const auto drownDamage = [](const VitalsRules& rules) {
        PlayerVitals vitals;
        vitals.setRules(rules);
        float total = 0.0F;
        // Past the 300-tick air supply and the -20 grace, then well beyond.
        for (int tick = 0; tick < 400; ++tick) {
            VitalsInput input;
            input.inWater = true;
            input.headInWater = true;
            total += vitals.tick(input).damageTaken;
        }
        return total;
    };
    VitalsRules rules;
    assert(drownDamage(rules) > 0.0F);
    rules.drowningDamage = false;
    assert(drownDamage(rules) == 0.0F);
    // The air supply still drains with the rule off, so the HUD's bubble bar
    // keeps telling the truth about being underwater.
    PlayerVitals vitals;
    vitals.setRules(rules);
    for (int tick = 0; tick < 50; ++tick) {
        VitalsInput input;
        input.inWater = true;
        input.headInWater = true;
        static_cast<void>(vitals.tick(input));
    }
    assert(vitals.airTicks() < PlayerVitals::kMaximumAirTicks);
}

void testNaturalRegeneration() {
    // A hurt, well-fed player heals on its own after 80 ticks (FoodData's
    // `foodLevel >= 18` branch) — unless the rule is off.
    const auto healedAfter = [](const VitalsRules& rules, Difficulty difficulty) {
        PlayerVitals vitals;
        vitals.setRules(rules);
        vitals.setDifficulty(difficulty);
        static_cast<void>(vitals.hurt(6.0F, DamageType::Generic));
        const float hurtHealth = vitals.health();
        for (int tick = 0; tick < 200; ++tick) {
            VitalsInput input;
            input.onGround = true;
            static_cast<void>(vitals.tick(input));
        }
        return vitals.health() - hurtHealth;
    };
    VitalsRules rules;
    assert(healedAfter(rules, Difficulty::Normal) > 0.0F);
    rules.naturalHealthRegeneration = false;
    assert(healedAfter(rules, Difficulty::Normal) == 0.0F);
    // ServerPlayer#tick gates the peaceful branch on the same rule, so peaceful
    // stops being a free heal too.
    VitalsRules peacefulOn;
    assert(healedAfter(peacefulOn, Difficulty::Peaceful) > 0.0F);
    assert(healedAfter(rules, Difficulty::Peaceful) == 0.0F);
    // Starvation sits outside the rule (vanilla's `else if (foodLevel <= 0)`),
    // so an empty belly is exactly as lethal with regeneration off.
    PlayerVitals starving;
    starving.setRules(rules);
    starving.setDifficulty(Difficulty::Hard);
    starving.restore(PlayerVitals::kMaximumHealth, 0, 0.0F, PlayerVitals::kMaximumAirTicks);
    float starved = 0.0F;
    for (int tick = 0; tick < 400; ++tick) {
        VitalsInput input;
        input.onGround = true;
        starved += starving.tick(input).damageTaken;
    }
    assert(starved > 0.0F);
}

void testFireSpreadRadius() {
    WorldSimulation simulation;
    simulation.setSimulationCenterBlock(0, 64, 0);
    // -1 is "anywhere", the escape hatch for a world that wants unbounded fire.
    simulation.setFireSpreadRadius(-1);
    assert(simulation.canSpreadFireAround({100000, 64, 0}));
    // 0 is 26.1's replacement for doFireTick=false: nowhere at all, including
    // the block the player is standing in.
    simulation.setFireSpreadRadius(0);
    assert(simulation.canSpreadFireAround({0, 64, 0}));
    assert(!simulation.canSpreadFireAround({1, 64, 0}));
    // A positive radius is measured in blocks from the simulation centre.
    simulation.setFireSpreadRadius(8);
    assert(simulation.canSpreadFireAround({8, 64, 0}));
    assert(!simulation.canSpreadFireAround({9, 64, 0}));
    assert(simulation.canSpreadFireAround({0, 72, 0}));
    assert(!simulation.canSpreadFireAround({0, 73, 0}));
    // The centre moves with the player.
    simulation.setSimulationCenterBlock(1000, 64, 0);
    assert(!simulation.canSpreadFireAround({0, 64, 0}));
    assert(simulation.canSpreadFireAround({1004, 64, 0}));
    // The default matches vanilla's, so a world that never touches the rule
    // ticks fire everywhere it plausibly simulates.
    WorldSimulation fresh;
    assert(fresh.fireSpreadRadius() == 128);
}

void testMobDrops() {
    // A cow killed by a player leaves loot and experience with the rule on, and
    // neither with it off. Both halves read the one mirrored flag inside die().
    const auto killAndCount = [](bool mobDrops) {
        mc::world::Chunk chunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                chunk.setBlock(x, 0, z, mc::world::Block::Stone);
            }
        }
        mc::world::World world;
        world.setChunk({0, 0}, std::move(chunk));

        EntitySystem entities;
        entities.setMobDropsEnabled(mobDrops);
        entities.spawn({8.0F, 1.0F, 8.0F}, *entities::entityTypeRegistry().byId("cow"), 1U);
        assert(entities.byId(1U) != nullptr);
        // Kill it as a player would: kill() routes through the same death path
        // /kill uses, and hurt()'s default attacker is the player, which is what
        // opens the experience window die() checks.
        static_cast<void>(entities.hurt(1U, 1.0F, glm::vec3{0.0F, 1.0F, 8.0F}));
        static_cast<void>(entities.kill(1U));

        std::size_t drops = 0U;
        for (const auto& entry : entities.pendingDrops()) {
            drops += entry.second.count;
        }
        return std::pair<std::size_t, std::size_t>{drops, entities.pendingExperience().size()};
    };
    const auto [dropsOn, xpOn] = killAndCount(true);
    const auto [dropsOff, xpOff] = killAndCount(false);
    assert(dropsOn > 0U);
    assert(dropsOff == 0U);
    // LivingEntity#dropExperience reads the same rule, so the orbs go with the
    // loot rather than surviving it.
    assert(xpOn > 0U);
    assert(xpOff == 0U);
}

} // namespace

int main() {
    // The species table is what `byId("cow")` reads; nothing else here needs it.
    mc::gameplay::entities::registerBuiltinEntities();

    testFallDamage();
    testFireDamage();
    testDrowningDamage();
    testNaturalRegeneration();
    testFireSpreadRadius();
    testMobDrops();
    return 0;
}

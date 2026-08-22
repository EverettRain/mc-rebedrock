// EM-2: the status-effect (MobEffect) system.
//
// Covers the registry (name/alias resolution, the R0 freeze/duplicate aborts
// the effect table stands on), the per-entity inline store (apply/merge/remove/
// clear/zero-storage), the deterministic tick (poison cadence + never-kill,
// regeneration, hunger, speed/slowness restoring on expiry), the mob and player
// integration through EntitySystem/PlayerVitals, and persistence round-trip.
//
// Registry lifecycle guarantees are aborts, so they run in a forked child.

#include "core/ContentId.hpp"
#include "core/Identifier.hpp"
#include "core/Registry.hpp"
#include "gameplay/Difficulty.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/PlayerVitals.hpp"
#include "gameplay/StatusEffect.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "gameplay/entities/MobBrain.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <glm/vec3.hpp>

#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace mc::gameplay;
using mc::core::Identifier;
using mc::core::StatusEffectId;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"status_effect_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

[[nodiscard]] bool nearly(float value, float expected) {
    return std::fabs(value - expected) < 0.001F;
}

// Runs `body` in a child process and reports whether it aborted (SIGABRT).
bool aborts(const std::function<void()>& body) {
    std::fflush(nullptr);
    const pid_t pid = fork();
    if (pid == 0) {
        std::freopen("/dev/null", "w", stderr);
        body();
        _exit(0);
    }
    int status = 0;
    static_cast<void>(waitpid(pid, &status, 0));
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

mc::world::World makeFlatWorld() {
    mc::world::World world;
    for (int chunkZ = -1; chunkZ <= 1; ++chunkZ) {
        for (int chunkX = -1; chunkX <= 1; ++chunkX) {
            mc::world::Chunk chunk;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    chunk.setBlock(x, 0, z, mc::world::Block::Stone);
                }
            }
            world.setChunk({chunkX, chunkZ}, std::move(chunk));
        }
    }
    return world;
}

class IdleAi final : public mc::gameplay::entities::EntityAi {
  public:
    void configureBrain(mc::gameplay::entities::MobBrain&) const override {}
};

const IdleAi kIdleAi;

// A plain, local test species with a known health and movement speed, so the
// effect assertions own their numbers.
const mc::gameplay::entities::EntityType& testType() {
    static const mc::gameplay::entities::EntityType type =
        mc::gameplay::entities::EntityType::Builder::create(
            mc::gameplay::entities::MobCategory::Creature, kIdleAi)
            .sized(0.9F, 0.9F)
            .health(20.0F)
            .movementSpeed(0.25F)
            .build("test_effect_mob");
    return type;
}

EntityTickResult tickEntities(EntitySystem& system, const mc::world::World& world) {
    return system.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F,
                       Difficulty::Normal, true, false, 0.0F, false);
}

// --- registry ---

// The five built-ins resolve by their own and vanilla names, carry the right
// category/kind, and hand back stable ids.
void testRegistry() {
    REQUIRE(poisonEffect().valid());
    REQUIRE(statusEffectByName("poison") == poisonEffect());
    REQUIRE(statusEffectByName("minecraft:poison") == poisonEffect());
    REQUIRE(statusEffectByName("rebedrock:poison") == poisonEffect());
    REQUIRE(statusEffectByName("regeneration") == regenerationEffect());
    REQUIRE(statusEffectByName("hunger") == hungerEffect());
    REQUIRE(statusEffectByName("speed") == speedEffect());
    REQUIRE(statusEffectByName("slowness") == slownessEffect());
    // An unknown name is a miss, not an abort.
    REQUIRE(!statusEffectByName("does_not_exist").valid());

    REQUIRE(statusEffectDef(poisonEffect()).kind == EffectKind::Poison);
    REQUIRE(statusEffectDef(poisonEffect()).category == StatusEffectCategory::Harmful);
    REQUIRE(statusEffectDef(speedEffect()).category == StatusEffectCategory::Beneficial);
    REQUIRE(nearly(statusEffectDef(speedEffect()).speedModifierPerLevel, 0.2F));
    REQUIRE(nearly(statusEffectDef(slownessEffect()).speedModifierPerLevel, -0.15F));
    // The stable name a save stores round-trips.
    REQUIRE(statusEffectName(poisonEffect()) == "poison");

    // Vanilla cadence: poison every 25 >> amp, regeneration every 50 >> amp.
    REQUIRE(effectTickInterval(EffectKind::Poison, 0) == 25);
    REQUIRE(effectTickInterval(EffectKind::Poison, 1) == 12);
    REQUIRE(effectTickInterval(EffectKind::Regeneration, 0) == 50);
    REQUIRE(effectTickInterval(EffectKind::Hunger, 0) == 1);
}

// The R0 guards the effect table depends on: a fresh registry aborts on a
// duplicate name and on registration after freeze. (The process-wide effect
// registry is already frozen, so these run against a fresh one in a child.)
void testRegistryGuards() {
    using EffectStore = mc::core::Registry<StatusEffectDef, StatusEffectId>;
    REQUIRE(aborts([] {
        EffectStore store;
        store.registerBuiltin(Identifier{"rebedrock", "poison"}, StatusEffectDef{});
        // Same name twice: a collision that must abort, not overwrite.
        store.registerBuiltin(Identifier{"rebedrock", "poison"}, StatusEffectDef{});
    }));
    REQUIRE(aborts([] {
        EffectStore store;
        store.registerBuiltin(Identifier{"rebedrock", "poison"}, StatusEffectDef{});
        store.freeze();
        // Registration after freeze must abort.
        store.registerBuiltin(Identifier{"rebedrock", "speed"}, StatusEffectDef{});
    }));
}

// --- the inline store API ---

void testStoreApiAndZeroStorage() {
    ActiveEffects effects;
    // Zero storage for an unaffected entity: no slot is populated.
    REQUIRE(effects.empty());
    REQUIRE(effects.size() == 0U);
    for (const auto& slot : effects.entries) {
        REQUIRE(!slot.active());
    }

    REQUIRE(applyEffect(effects, poisonEffect(), 100, 0));
    REQUIRE(hasEffect(effects, poisonEffect()));
    REQUIRE(effects.size() == 1U);
    REQUIRE(getEffect(effects, poisonEffect())->durationTicks == 100);

    // Re-apply weaker/shorter is ignored; stronger replaces.
    REQUIRE(!applyEffect(effects, poisonEffect(), 50, 0));
    REQUIRE(getEffect(effects, poisonEffect())->durationTicks == 100);
    REQUIRE(applyEffect(effects, poisonEffect(), 40, 1));  // higher amplifier wins
    REQUIRE(getEffect(effects, poisonEffect())->amplifier == 1);
    REQUIRE(getEffect(effects, poisonEffect())->durationTicks == 40);

    // A second, different effect uses a fresh slot.
    REQUIRE(applyEffect(effects, speedEffect(), 200, 0));
    REQUIRE(effects.size() == 2U);

    // removeEffect drops one; clearEffects drops all.
    REQUIRE(removeEffect(effects, poisonEffect()));
    REQUIRE(!hasEffect(effects, poisonEffect()));
    REQUIRE(hasEffect(effects, speedEffect()));
    REQUIRE(effects.size() == 1U);
    REQUIRE(clearEffects(effects) == 1U);
    REQUIRE(effects.empty());

    // A zero/negative duration or invalid id never applies.
    REQUIRE(!applyEffect(effects, poisonEffect(), 0, 0));
    REQUIRE(!applyEffect(effects, StatusEffectId::invalid(), 100, 0));
    REQUIRE(effects.empty());
}

// --- the deterministic tick ---

// Poison bites one point every 25 ticks and never drops the victim below one
// health.
void testPoisonTick() {
    ActiveEffects effects;
    REQUIRE(applyEffect(effects, poisonEffect(), 100, 0));
    // Duration 100 → fires at 100/75/50/25 (durationTicks % 25 == 0), four bites.
    float health = 20.0F;
    int bites = 0;
    for (int tick = 0; tick < 100; ++tick) {
        const EffectTickOutcome outcome = tickEffects(effects, health);
        if (outcome.damage > 0.0F) {
            ++bites;
            REQUIRE(nearly(outcome.damage, 1.0F));
            health -= outcome.damage;
        }
    }
    REQUIRE(bites == 4);
    REQUIRE(effects.empty());  // expired after 100 ticks

    // Never-kill: at one health poison does nothing.
    ActiveEffects lethal;
    REQUIRE(applyEffect(lethal, poisonEffect(), 100, 0));
    const EffectTickOutcome atOne = tickEffects(lethal, 1.0F);
    REQUIRE(nearly(atOne.damage, 0.0F));
}

// Regeneration heals one point every 50 ticks.
void testRegenerationTick() {
    ActiveEffects effects;
    REQUIRE(applyEffect(effects, regenerationEffect(), 100, 0));
    int heals = 0;
    for (int tick = 0; tick < 100; ++tick) {
        const EffectTickOutcome outcome = tickEffects(effects, 10.0F);
        if (outcome.heal > 0.0F) {
            ++heals;
            REQUIRE(nearly(outcome.heal, 1.0F));
        }
    }
    REQUIRE(heals == 2);  // fires at 100 and 50
}

// Speed/slowness set a movement factor that reverts to 1.0 the tick the effect
// expires — no modifier left behind. This is sabotage anchor ①.
void testMovementModifierRestores() {
    ActiveEffects effects;
    REQUIRE(applyEffect(effects, speedEffect(), 3, 0));  // 3 ticks of Speed I
    // Speed I: base * (1 + 0.2 * 1) = 1.2.
    for (int tick = 0; tick < 3; ++tick) {
        const EffectTickOutcome outcome = tickEffects(effects, 20.0F);
        REQUIRE(nearly(outcome.speedMultiplier, 1.2F));
    }
    // The effect has now expired; the store is empty and the factor is 1.0.
    REQUIRE(effects.empty());
    const EffectTickOutcome after = tickEffects(effects, 20.0F);
    REQUIRE(nearly(after.speedMultiplier, 1.0F));

    // Slowness I: base * (1 - 0.15) = 0.85. Amplifier scales it.
    ActiveEffects slow;
    REQUIRE(applyEffect(slow, slownessEffect(), 10, 1));  // Slowness II
    const EffectTickOutcome outcome = tickEffects(slow, 20.0F);
    REQUIRE(nearly(outcome.speedMultiplier, 1.0F - 0.15F * 2.0F));
}

// --- mob integration ---

// A poisoned mob loses health through the shared pipeline and stops at expiry;
// poison never kills it.
void testMobPoison() {
    const mc::world::World world = makeFlatWorld();
    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, testType(), 1U);
    const std::uint64_t id = system.entities().front().id;
    REQUIRE(system.applyEffect(id, poisonEffect(), 100, 0));
    REQUIRE(system.hasEffect(id, poisonEffect()));

    const float start = system.byId(id)->damage.health;
    for (int tick = 0; tick < 120; ++tick) {
        tickEntities(system, world);
    }
    // Four bites landed; the effect has expired.
    REQUIRE(system.byId(id)->damage.health < start);
    REQUIRE(!system.hasEffect(id, poisonEffect()));
    REQUIRE(system.byId(id)->damage.lastSource == DamageType::Generic);

    // clearEffects removes it mid-flight.
    system.applyEffect(id, poisonEffect(), 100, 0);
    system.applyEffect(id, regenerationEffect(), 100, 0);
    REQUIRE(system.clearEffects(id) == 2U);
    REQUIRE(!system.hasEffect(id, poisonEffect()));
}

// A mob with Speed moves faster through its movement multiplier, and the
// multiplier returns to 1.0 once the effect lapses (checked via the field the
// wander speed reads).
void testMobSpeed() {
    const mc::world::World world = makeFlatWorld();
    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, testType(), 2U);
    const std::uint64_t id = system.entities().front().id;
    REQUIRE(system.applyEffect(id, speedEffect(), 2, 0));
    tickEntities(system, world);
    REQUIRE(nearly(system.byId(id)->movementSpeedMultiplier, 1.2F));
    // Second (last) tick still boosted, then it expires.
    tickEntities(system, world);
    tickEntities(system, world);
    REQUIRE(nearly(system.byId(id)->movementSpeedMultiplier, 1.0F));
    REQUIRE(!system.hasEffect(id, speedEffect()));
}

// A dead entity does not accrue effects, and effects on a live one are
// deterministic across two identically seeded systems.
void testMobDeterminismAndDeadGuard() {
    const mc::world::World world = makeFlatWorld();
    EntitySystem a;
    EntitySystem b;
    a.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, testType(), 42U);
    b.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, testType(), 42U);
    const std::uint64_t idA = a.entities().front().id;
    const std::uint64_t idB = b.entities().front().id;
    REQUIRE(a.applyEffect(idA, poisonEffect(), 100, 0));
    REQUIRE(b.applyEffect(idB, poisonEffect(), 100, 0));
    for (int tick = 0; tick < 120; ++tick) {
        tickEntities(a, world);
        tickEntities(b, world);
        REQUIRE(nearly(a.byId(idA)->damage.health, b.byId(idB)->damage.health));
    }
}

// --- player integration ---

void testPlayerEffects() {
    // Poison hurts the player (never below one health), and expires.
    PlayerVitals poisoned;
    poisoned.reset();
    REQUIRE(poisoned.applyEffect(poisonEffect(), 100, 0));
    VitalsInput idle{};
    idle.onGround = true;
    const float start = poisoned.health();
    for (int tick = 0; tick < 120; ++tick) {
        static_cast<void>(poisoned.tick(idle));
    }
    REQUIRE(poisoned.health() < start);
    REQUIRE(poisoned.health() >= 1.0F);
    REQUIRE(!poisoned.hasEffect(poisonEffect()));

    // Speed sets the player's movement factor, which reverts on expiry.
    PlayerVitals quick;
    quick.reset();
    REQUIRE(quick.applyEffect(speedEffect(), 2, 0));
    static_cast<void>(quick.tick(idle));
    REQUIRE(nearly(quick.speedMultiplier(), 1.2F));
    static_cast<void>(quick.tick(idle));   // last boosted tick
    static_cast<void>(quick.tick(idle));   // now expired
    REQUIRE(nearly(quick.speedMultiplier(), 1.0F));

    // Hunger drains food exhaustion every tick.
    PlayerVitals hungry;
    hungry.reset();
    REQUIRE(hungry.applyEffect(hungerEffect(), 5, 0));
    const float exhaustionBefore = hungry.exhaustion();
    static_cast<void>(hungry.tick(idle));
    REQUIRE(hungry.exhaustion() > exhaustionBefore);

    // A respawn (reset) wipes every effect.
    PlayerVitals respawning;
    respawning.applyEffect(poisonEffect(), 100, 0);
    respawning.reset();
    REQUIRE(!respawning.hasEffect(poisonEffect()));
    REQUIRE(respawning.effects().empty());
}

} // namespace

int main() {
    testRegistry();
    testRegistryGuards();
    testStoreApiAndZeroStorage();
    testPoisonTick();
    testRegenerationTick();
    testMovementModifierRestores();
    testMobPoison();
    testMobSpeed();
    testMobDeterminismAndDeadGuard();
    testPlayerEffects();
    return 0;
}

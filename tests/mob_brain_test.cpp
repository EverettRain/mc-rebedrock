#include "gameplay/Difficulty.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/entities/BuiltinSpecies.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"mob_brain_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

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

float horizontalDistance(glm::vec3 first, glm::vec3 second) {
    first.y = 0.0F;
    second.y = 0.0F;
    return glm::length(first - second);
}

class NoGoalAi final : public mc::gameplay::entities::EntityAi {
  public:
    void configureBrain(mc::gameplay::entities::MobBrain&) const override {}
};

class BlockedCellEvaluator final : public mc::gameplay::entities::GroundNodeEvaluator {
  public:
    explicit BlockedCellEvaluator(glm::ivec3 blocked) : blocked_(blocked) {}

    [[nodiscard]] std::optional<mc::gameplay::entities::GroundNodeEvaluation>
    evaluate(const mc::world::World& world, const mc::gameplay::SimpleEntity& self,
             glm::ivec3 feet) const override {
        if (feet == blocked_) {
            return std::nullopt;
        }
        return GroundNodeEvaluator::evaluate(world, self, feet);
    }

  private:
    glm::ivec3 blocked_{0};
};

} // namespace

int main() {
    using mc::gameplay::ActorReference;
    using mc::gameplay::Difficulty;
    using mc::gameplay::EntitySystem;

    mc::gameplay::entities::registerBuiltinEntities();
    auto world = makeFlatWorld();

    // Every spawn gets an independent stateful brain. Pig and cow share the
    // five ordinary land-animal goal definitions; zombie keeps its own profile.
    EntitySystem entities;
    entities.spawn({8.0F, 1.001F, 8.0F}, mc::gameplay::entities::builtinSpecies("cow"), 11U);
    entities.spawn({12.0F, 1.001F, 8.0F}, mc::gameplay::entities::builtinSpecies("cow"), 12U);
    entities.spawn({8.0F, 1.001F, 12.0F}, mc::gameplay::entities::builtinSpecies("pig"), 13U);
    entities.spawn({12.0F, 1.001F, 12.0F}, mc::gameplay::entities::builtinSpecies("zombie"), 14U);
    REQUIRE(entities.entities()[0].id != entities.entities()[1].id);
    // AR-A3: the cow is now breedable (tempt=wheat), so installBreedingGoals
    // adds AnimalMateGoal/TemptGoal/FollowParentGoal on top of AnimalAi's five
    // (Swim/EscapeDanger/WanderAroundFar/LookAtPlayer/LookAround) — 8 total.
    // The pig (index 2) is not yet breedable, so its count is unchanged.
    REQUIRE(entities.entities()[0].brain.goals().size() == 8U);
    REQUIRE(entities.entities()[2].brain.goals().size() == 5U);
    REQUIRE(entities.entities()[3].brain.goals().size() == 4U);
    REQUIRE(entities.entities()[3].brain.targets().size() == 1U);

    const std::uint64_t firstCowId = entities.entities()[0].id;
    const std::uint64_t secondCowId = entities.entities()[1].id;
    const glm::vec3 attacker{14.0F, 1.001F, 8.0F};
    static_cast<void>(entities.tick(world, attacker, 0.6F, 1.8F, Difficulty::Normal));
    const glm::vec3 beforeEscape = entities.byId(firstCowId)->position;

    // A landed player hit records a stable attacker reference. EscapeDangerGoal
    // (priority 1 MOVE) must preempt any running WanderAroundFarGoal (priority 5)
    // in this cow only and choose a destination away from the attacker.
    REQUIRE(entities.hurt(firstCowId, 1.0F, attacker, ActorReference::player()));
    static_cast<void>(entities.tick(world, attacker, 0.6F, 1.8F, Difficulty::Normal));
    const auto* escapingCow = entities.byId(firstCowId);
    const auto* calmCow = entities.byId(secondCowId);
    REQUIRE(escapingCow != nullptr && calmCow != nullptr);
    REQUIRE(escapingCow->lastAttacker == ActorReference::player());
    REQUIRE(escapingCow->brain.goals().isRunning("escape_danger"));
    REQUIRE(escapingCow->movementSpeedMultiplier == 2.0F);
    REQUIRE(!calmCow->brain.goals().isRunning("escape_danger"));
    REQUIRE(escapingCow->brain.navigation().destination().has_value());
    REQUIRE(escapingCow->brain.navigation().destination()->x < beforeEscape.x);

    for (int tick = 0; tick < 25; ++tick) {
        static_cast<void>(entities.tick(world, attacker, 0.6F, 1.8F, Difficulty::Normal));
    }
    escapingCow = entities.byId(firstCowId);
    REQUIRE(escapingCow != nullptr);
    REQUIRE(horizontalDistance(escapingCow->position, attacker) >
            horizontalDistance(beforeEscape, attacker));

    // Registered speeds remain the real 26.1 attributes, but the local
    // locomotion integrator converts them to its historical movement scale at
    // one shared boundary. A normal 0.2 cow must therefore stay in the normal
    // walking range, while a 2.0 panic modifier remains visibly faster rather
    // than making both modes five times too fast.
    static const NoGoalAi speedProbeAi;
    static const auto speedProbeType =
        mc::gameplay::entities::EntityType::Builder::create(
            mc::gameplay::entities::MobCategory::Creature, speedProbeAi)
            .sized(0.9F, 1.4F)
            .movementSpeed(0.2F)
            .build("speed_probe");
    EntitySystem normalSpeedEntities;
    EntitySystem panicSpeedEntities;
    normalSpeedEntities.spawn({2.5F, 1.001F, 4.5F}, speedProbeType, 101U);
    panicSpeedEntities.spawn({2.5F, 1.001F, 6.5F}, speedProbeType, 102U);
    auto* normalSpeedEntity = normalSpeedEntities.byId(normalSpeedEntities.entities()[0].id);
    auto* panicSpeedEntity = panicSpeedEntities.byId(panicSpeedEntities.entities()[0].id);
    REQUIRE(normalSpeedEntity != nullptr && panicSpeedEntity != nullptr);
    const glm::vec3 normalStart = normalSpeedEntities.entities()[0].position;
    const glm::vec3 panicStart = panicSpeedEntities.entities()[0].position;
    REQUIRE(normalSpeedEntity->brain.navigation().startMovingTo(
        world, *normalSpeedEntity, {25.5F, 1.001F, 4.5F}, 1.0F));
    REQUIRE(panicSpeedEntity->brain.navigation().startMovingTo(
        world, *panicSpeedEntity, {25.5F, 1.001F, 6.5F}, 2.0F));
    for (int tick = 0; tick < 20; ++tick) {
        static_cast<void>(normalSpeedEntities.tick(world));
        static_cast<void>(panicSpeedEntities.tick(world));
    }
    const float normalTravel =
        horizontalDistance(normalSpeedEntities.entities()[0].position, normalStart);
    const float panicTravel =
        horizontalDistance(panicSpeedEntities.entities()[0].position, panicStart);
    REQUIRE(normalTravel > 0.4F && normalTravel < 1.0F);
    REQUIRE(panicTravel > normalTravel * 1.7F);
    REQUIRE(panicTravel < normalTravel * 2.2F);

    // Hostiles no longer inherit the old global "every hit bolts" shortcut.
    const std::uint64_t zombieId = entities.entities()[3].id;
    REQUIRE(entities.hurt(zombieId, 1.0F, attacker, ActorReference::player()));
    static_cast<void>(entities.tick(world, attacker, 0.6F, 1.8F, Difficulty::Normal));
    REQUIRE(!entities.byId(zombieId)->brain.goals().isRunning("escape_danger"));

    // A zombie acquires a visible survival player, preempts its wander goal,
    // follows through GroundNavigation, and emits raw attack events no faster
    // than the 20-tick melee cooldown.
    auto combatWorld = makeFlatWorld();
    EntitySystem combatEntities;
    combatEntities.spawn({4.5F, 1.001F, 8.5F}, mc::gameplay::entities::builtinSpecies("zombie"), 41U);
    const std::uint64_t combatZombieId = combatEntities.entities()[0].id;
    const glm::vec3 combatPlayer{10.5F, 1.001F, 8.5F};
    const float combatStartDistance =
        horizontalDistance(combatEntities.entities()[0].position, combatPlayer);
    bool acquiredPlayer = false;
    std::vector<int> attackTicks;
    for (int tick = 0; tick < 240; ++tick) {
        const auto result = combatEntities.tick(combatWorld, combatPlayer, 0.6F, 1.8F,
                                                Difficulty::Normal, true, false);
        const auto* zombie = combatEntities.byId(combatZombieId);
        REQUIRE(zombie != nullptr);
        acquiredPlayer = acquiredPlayer || zombie->brain.combatTarget() == ActorReference::player();
        for (const auto& attack : result.mobAttacks) {
            REQUIRE(attack.attackerId == combatZombieId);
            REQUIRE(attack.target == ActorReference::player());
            REQUIRE(attack.amount == 3.0F);
            attackTicks.push_back(tick);
        }
    }
    const auto* combatZombie = combatEntities.byId(combatZombieId);
    REQUIRE(acquiredPlayer);
    REQUIRE(combatZombie != nullptr);
    REQUIRE(horizontalDistance(combatZombie->position, combatPlayer) < combatStartDistance);
    REQUIRE(!attackTicks.empty());
    for (std::size_t index = 1; index < attackTicks.size(); ++index) {
        REQUIRE(attackTicks[index] - attackTicks[index - 1U] >= 20);
    }

    // Creative/dead players are not valid hostile targets.
    EntitySystem creativeTargetEntities;
    creativeTargetEntities.spawn({7.5F, 1.001F, 8.5F}, mc::gameplay::entities::builtinSpecies("zombie"), 42U);
    for (int tick = 0; tick < 80; ++tick) {
        const auto result = creativeTargetEntities.tick(combatWorld, combatPlayer, 0.6F, 1.8F,
                                                        Difficulty::Normal, true, true);
        REQUIRE(result.mobAttacks.empty());
        REQUIRE(!creativeTargetEntities.entities()[0].brain.combatTarget().valid());
    }

    // Solid terrain blocks acquisition. The target selector must not let the
    // melee goal attack through a three-block-high wall.
    auto occludedWorld = makeFlatWorld();
    for (int z = -16; z < 32; ++z) {
        for (int y = 1; y <= 3; ++y) {
            REQUIRE(occludedWorld.setBlock(8, y, z, mc::world::Block::Stone));
        }
    }
    EntitySystem occludedEntities;
    occludedEntities.spawn({6.5F, 1.001F, 8.5F}, mc::gameplay::entities::builtinSpecies("zombie"), 43U);
    for (int tick = 0; tick < 80; ++tick) {
        const auto result = occludedEntities.tick(occludedWorld, combatPlayer, 0.6F, 1.8F,
                                                  Difficulty::Normal, true, false);
        REQUIRE(result.mobAttacks.empty());
        REQUIRE(!occludedEntities.entities()[0].brain.combatTarget().valid());
    }

    // A visible player across an unbridgeable void is targetable but not
    // reachable. MeleeAttackGoal must retain vanilla's 20-tick initial-search
    // throttle rather than spending one bounded A* search every simulation tick.
    auto unreachableWorld = makeFlatWorld();
    for (int z = -16; z < 32; ++z) {
        REQUIRE(unreachableWorld.setBlock(8, 0, z, mc::world::Block::Air));
    }
    EntitySystem unreachableEntities;
    unreachableEntities.spawn({6.5F, 1.001F, 8.5F}, mc::gameplay::entities::builtinSpecies("zombie"), 44U);
    auto* unreachableZombie = unreachableEntities.byId(unreachableEntities.entities()[0].id);
    REQUIRE(unreachableZombie != nullptr);
    unreachableZombie->brain.setCombatTarget(ActorReference::player());
    mc::gameplay::MobAiContext unreachableContext{
        unreachableWorld,
        std::span<const mc::gameplay::SimpleEntity>{unreachableEntities.entities()},
        mc::gameplay::PlayerAiView{combatPlayer, true, true, false, 0.6F, 1.8F}, 0U};
    mc::gameplay::entities::MeleeAttackGoal unreachableMelee{1.0F};
    for (int tick = 0; tick < 120; ++tick) {
        REQUIRE(!unreachableMelee.canStart(*unreachableZombie, unreachableContext,
                                           unreachableZombie->brain));
    }
    const auto& unreachableSearches =
        unreachableEntities.entities()[0].brain.navigation().cumulativeSearchStats();
    REQUIRE(unreachableSearches.searches > 0U);
    REQUIRE(unreachableSearches.searches <= 6U);

    // The land navigator plans around a three-block-high wall rather than
    // walking its heading into the obstacle. A direct six-cell path would have
    // size 6, so the detour must contain more nodes.
    EntitySystem navigatorEntities;
    navigatorEntities.spawn({5.5F, 1.001F, 8.5F}, mc::gameplay::entities::builtinSpecies("cow"), 21U);
    auto* navigator = navigatorEntities.byId(navigatorEntities.entities()[0].id);
    REQUIRE(navigator != nullptr);
    for (int z = 4; z <= 12; ++z) {
        for (int y = 1; y <= 3; ++y) {
            REQUIRE(world.setBlock(8, y, z, mc::world::Block::Stone));
        }
    }
    REQUIRE(navigator->brain.navigation().startMovingTo(world, *navigator, {11.5F, 1.001F, 8.5F},
                                                        1.0F));
    REQUIRE(navigator->brain.navigation().pathSize() > 6U);

    // Flat-ground A* uses all eight horizontal neighbours. A 45-degree target
    // is therefore eight diagonal nodes, not the old sixteen-node Manhattan
    // staircase, and the search exposes deterministic diagnostics for profiling.
    auto diagonalWorld = makeFlatWorld();
    EntitySystem diagonalEntities;
    diagonalEntities.spawn({2.5F, 1.001F, 2.5F}, mc::gameplay::entities::builtinSpecies("cow"), 24U);
    auto* diagonalCow = diagonalEntities.byId(diagonalEntities.entities()[0].id);
    REQUIRE(diagonalCow != nullptr);
    auto& diagonalNavigation = diagonalCow->brain.navigation();
    REQUIRE(diagonalNavigation.startMovingTo(diagonalWorld, *diagonalCow, {10.5F, 1.001F, 10.5F},
                                             1.0F));
    REQUIRE(diagonalNavigation.pathSize() == 8U);
    glm::ivec3 previousCell{2, 1, 2};
    for (const glm::ivec3 node : diagonalNavigation.pathNodes()) {
        REQUIRE(std::abs(node.x - previousCell.x) == 1);
        REQUIRE(std::abs(node.z - previousCell.z) == 1);
        previousCell = node;
    }
    REQUIRE(diagonalNavigation.lastSearchStats().reachedTarget);
    REQUIRE(diagonalNavigation.lastSearchStats().expandedNodes <= 16U);

    // A geometrically open diagonal cell must not be reached through the corner
    // between two solid cardinal neighbours.
    auto cornerWorld = makeFlatWorld();
    for (int y = 1; y <= 3; ++y) {
        REQUIRE(cornerWorld.setBlock(6, y, 5, mc::world::Block::Stone));
        REQUIRE(cornerWorld.setBlock(5, y, 6, mc::world::Block::Stone));
    }
    EntitySystem cornerEntities;
    cornerEntities.spawn({5.5F, 1.001F, 5.5F}, mc::gameplay::entities::builtinSpecies("cow"), 25U);
    auto* cornerCow = cornerEntities.byId(cornerEntities.entities()[0].id);
    REQUIRE(cornerCow != nullptr);
    REQUIRE(cornerCow->brain.navigation().startMovingTo(cornerWorld, *cornerCow,
                                                        {6.5F, 1.001F, 6.5F}, 1.0F));
    REQUIRE(cornerCow->brain.navigation().pathSize() > 1U);
    REQUIRE(cornerCow->brain.navigation().pathNodes().front() != glm::ivec3(6, 1, 6));

    // Node classification checks the complete entity footprint. This wider
    // synthetic mob cannot occupy the one-cell aisle that an ordinary cow can.
    static const NoGoalAi noGoalAi;
    static const auto wideType = mc::gameplay::entities::EntityType::Builder::create(
                                     mc::gameplay::entities::MobCategory::Creature, noGoalAi)
                                     .sized(1.4F, 1.4F)
                                     .build("wide_path_test");
    mc::gameplay::SimpleEntity wideEntity;
    wideEntity.type = &wideType;
    wideEntity.position = {5.5F, 1.001F, 5.5F};
    auto corridorWorld = makeFlatWorld();
    for (int y = 1; y <= 2; ++y) {
        REQUIRE(corridorWorld.setBlock(4, y, 5, mc::world::Block::Stone));
        REQUIRE(corridorWorld.setBlock(6, y, 5, mc::world::Block::Stone));
    }
    mc::gameplay::entities::GroundNodeEvaluator defaultEvaluator;
    REQUIRE(!defaultEvaluator.isStandable(corridorWorld, wideEntity, {5, 1, 5}));

    // Collision alone does not make a land node: leaf crowns use a collision
    // shape for entities and raycasts, but are excluded from ordinary land
    // navigation just like MOTION_BLOCKING_NO_LEAVES excludes them from the
    // natural-spawn surface.
    auto leafWorld = makeFlatWorld();
    REQUIRE(leafWorld.setBlock(5, 1, 5, mc::world::Block::OakLeaves));
    REQUIRE(!defaultEvaluator.isStandable(leafWorld, *diagonalCow, {5, 2, 5}));

    // Terrain policy is injectable independently from A* and path following.
    // A future door/hazard evaluator can reject or penalise cells this way.
    mc::gameplay::entities::GroundNavigation extendedNavigation{
        std::make_unique<BlockedCellEvaluator>(glm::ivec3{6, 1, 2})};
    REQUIRE(
        extendedNavigation.startMovingTo(diagonalWorld, *diagonalCow, {8.5F, 1.001F, 2.5F}, 1.0F));
    REQUIRE(std::none_of(extendedNavigation.pathNodes().begin(),
                         extendedNavigation.pathNodes().end(),
                         [](glm::ivec3 node) { return node == glm::ivec3{6, 1, 2}; }));

    // Swim owns MOVE/JUMP without a land-navigation path. An idle navigator
    // must not erase the goal's movement intent, and the impulse must overcome
    // this engine's gravity pass.
    EntitySystem swimmerEntities;
    REQUIRE(world.setBlock(3, 1, 3, mc::world::Block::Water));
    swimmerEntities.spawn({3.5F, 1.001F, 3.5F}, mc::gameplay::entities::builtinSpecies("pig"), 22U);
    const float swimmerStartY = swimmerEntities.entities()[0].position.y;
    static_cast<void>(swimmerEntities.tick(world));
    REQUIRE(swimmerEntities.entities()[0].brain.goals().isRunning("swim"));
    REQUIRE(swimmerEntities.entities()[0].position.y > swimmerStartY);

    // A hit on a small plateau must fall back from the preferred eight-block
    // escape distance to a shorter reachable route. Once at the last safe node,
    // navigation must not let residual AI velocity carry the cow over the edge.
    auto plateauWorld = makeFlatWorld();
    for (int z = -16; z < 32; ++z) {
        for (int x = -16; x < 32; ++x) {
            if (x < 4 || x > 12 || z < 4 || z > 12) {
                REQUIRE(plateauWorld.setBlock(x, 0, z, mc::world::Block::Air));
            }
        }
    }
    EntitySystem plateauEntities;
    plateauEntities.spawn({8.5F, 1.001F, 8.5F}, mc::gameplay::entities::builtinSpecies("cow"), 23U);
    const std::uint64_t plateauCowId = plateauEntities.entities()[0].id;
    const glm::vec3 plateauAttacker{10.5F, 1.001F, 8.5F};
    static_cast<void>(plateauEntities.tick(plateauWorld, plateauAttacker));
    REQUIRE(!plateauEntities.byId(plateauCowId)
                 ->brain.navigation()
                 .startMovingTo(plateauWorld, *plateauEntities.byId(plateauCowId),
                                {2.5F, 1.001F, 8.5F}, 1.0F));
    REQUIRE(plateauEntities.hurt(plateauCowId, 1.0F, plateauAttacker, ActorReference::player()));
    bool startedPlateauEscape = false;
    float minimumPlateauY = plateauEntities.byId(plateauCowId)->position.y;
    for (int tick = 0; tick < 160; ++tick) {
        static_cast<void>(plateauEntities.tick(plateauWorld, plateauAttacker));
        const auto* plateauCow = plateauEntities.byId(plateauCowId);
        REQUIRE(plateauCow != nullptr);
        startedPlateauEscape =
            startedPlateauEscape || plateauCow->brain.goals().isRunning("escape_danger");
        minimumPlateauY = std::min(minimumPlateauY, plateauCow->position.y);
    }
    REQUIRE(startedPlateauEscape);
    REQUIRE(minimumPlateauY > 0.9F);

    // Stable ActorReference lookup survives vector compaction after an earlier
    // entity is removed; array indices would point at the wrong creature here.
    EntitySystem referenceEntities;
    referenceEntities.spawn({2.0F, 1.001F, 2.0F}, mc::gameplay::entities::builtinSpecies("pig"), 31U);
    referenceEntities.spawn({6.0F, 1.001F, 2.0F}, mc::gameplay::entities::builtinSpecies("cow"), 32U);
    const std::uint64_t referencedCowId = referenceEntities.entities()[1].id;
    REQUIRE(referenceEntities.kill(referenceEntities.entities()[0].id));
    for (int tick = 0; tick < 21; ++tick) {
        static_cast<void>(referenceEntities.tick(world));
    }
    const auto* referencedCow = referenceEntities.byId(referencedCowId);
    REQUIRE(referencedCow != nullptr);
    mc::gameplay::MobAiContext context{
        world, std::span<const mc::gameplay::SimpleEntity>{referenceEntities.entities()},
        mc::gameplay::PlayerAiView{}, 0U};
    const auto resolved = context.actorPosition(ActorReference::entity(referencedCowId));
    REQUIRE(resolved.has_value());
    REQUIRE(horizontalDistance(*resolved, referencedCow->position) < 0.001F);

    return 0;
}

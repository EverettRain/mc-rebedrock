#include "gameplay/GameSession.hpp"

#include "world/World.hpp"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace mc::gameplay {

namespace {
constexpr float kInfiniteDamage = std::numeric_limits<float>::infinity();
} // namespace

GameSession::GameSession() : player_({24.0F, 78.0F - PlayerController::kEyeHeight, 24.0F}) {}

void GameSession::tick(world::World& world, SimulationHost& host) {
    playerInput_.jumpPressed = jumpPressed_;
    physicsPreviousPosition_ = physicsCurrentPosition_;
    player_.tick(world, playerInput_);
    // FarmlandBlock#onLandedUpon: on a landing, the player's fall distance
    // (Entity.fallDistance, tracked across frames in PlayerController) decides
    // whether the tilled soil under the feet tramples back to dirt. Vanilla
    // 1.16.1 rolls nextFloat() < fallDistance - 0.5f, so a one-block fall breaks
    // farmland half the time and a taller one almost always; walking never
    // tramples.
    if (player_.onGround() && player_.fallDistance() > 0.5F) {
        const auto feet = player_.position();
        const int trampleX = static_cast<int>(std::floor(feet.x));
        const int trampleY = static_cast<int>(std::floor(feet.y - 0.001F));
        const int trampleZ = static_cast<int>(std::floor(feet.z));
        const auto soil = world.block(trampleX, trampleY, trampleZ);
        lootRandomState_ = lootRandomState_ * 1664525U + 1013904223U;
        const float roll =
            static_cast<float>(lootRandomState_ >> 8) / static_cast<float>(1U << 24);
        if (world::isFarmland(soil) && roll < player_.fallDistance() - 0.5F) {
            world.setBlock(trampleX, trampleY, trampleZ, world::Block::Dirt);
            host.submitWorldEdit(trampleX, trampleY, trampleZ, world::Block::Dirt, 0U, std::nullopt);
            host.previewBlockEdit(trampleX, trampleY, trampleZ);
            host.playBlockBreak(
                soil, {static_cast<float>(trampleX) + 0.5F, static_cast<float>(trampleY) + 0.5F,
                       static_cast<float>(trampleZ) + 0.5F});
            host.spawnBlockBreakParticles({trampleX, trampleY, trampleZ}, soil);
            // The crop above loses its farmland and pops.
            worldSimulation_.notifyNeighborChanged(world, {trampleX, trampleY, trampleZ});
        }
    }
    updateMovementAudio(host, world, physicsPreviousPosition_, player_.position());
    tickPlayerVitals(host, world, physicsPreviousPosition_, player_.jumpedThisTick());
    tickEating(host);
    // The fluid phase runs once per accumulator drain; the renderer never lets
    // more than one batch of overdue water updates stack up across frames.
    bool fluidUpdatePhaseConsumed = false;
    for (const auto& change : worldSimulation_.tick(world, !fluidUpdatePhaseConsumed)) {
        host.submitWorldEdit(change.position.x, change.position.y, change.position.z,
                             change.block, change.fluidLevel, change.orientation);
        // A simulated break — an attached block that lost its support, a
        // decoration a fluid washed away, or a leaf that decayed — is a real
        // block break: vanilla plays the break sound, throws the break particles
        // and rolls the loot table through World#breakBlock(pos, true), which is
        // game-mode independent (a wall torch drops in creative too). The column
        // under the break has to be relit or the removed block's light stays
        // behind. Fluid spread changes carry dropped == Air, so the thousand-cell
        // flows never pay for this pass.
        if (change.dropped != world::Block::Air) {
            host.previewBlockEdit(change.position.x, change.position.y, change.position.z);
            host.playBlockBreak(
                change.dropped,
                {static_cast<float>(change.position.x) + 0.5F,
                 static_cast<float>(change.position.y) + 0.5F,
                 static_cast<float>(change.position.z) + 0.5F});
            host.spawnBlockBreakParticles(
                {change.position.x, change.position.y, change.position.z}, change.dropped);
            // Nobody swung a tool at these, so they roll the same loot table an
            // empty hand would. The dropped block's captured orientation lets a
            // popped crop roll its loot from the age it had reached.
            spawnBlockDrops({change.position.x, change.position.y, change.position.z},
                            change.dropped, ItemStack{}, change.droppedOrientation);
        }
    }
    fluidUpdatePhaseConsumed = true;
    craftingSystem_.tickFurnace();
    // A burning furnace swaps its block to the lit state so the front face and
    // the block light follow the fuel burn.
    host.onFurnaceStateChanged();
    chestSystem_.tick();
    if (itemEntities_.tick(world, player_.position(), inventory_) > 0U) {
        host.playItemPickup(player_.position());
    }
    // The herd pushes back: Entity#pushAwayFrom moves both parties, so a pig
    // walking into the player nudges them. Difficulty is per-save (level.dat).
    const auto entityTick = worldEntities_.tick(
        world, player_.position(), PlayerController::kWidth, PlayerController::kHeight,
        difficulty_);
    player_.applyExternalPush(entityTick.playerPush);
    consumeEntityEvents(host);
    physicsCurrentPosition_ = player_.position();
    jumpPressed_ = false;
    forwardPressed_ = false;
}

void GameSession::setGameMode(GameMode mode) {
    gameMode_ = mode;
    playerInput_.flightAllowed = mode == GameMode::Creative;
}

bool GameSession::hurtPlayer(DamageSource source, float amount, SimulationHost& host) {
    if (!vitals_.hurt(amount, source)) {
        return false;
    }
    host.playPlayerHurt(player_.position());
    if (vitals_.dead()) {
        host.onPlayerDied();
    }
    return true;
}

void GameSession::killPlayer(SimulationHost& host) {
    (void)hurtPlayer(DamageSource::OutOfWorld, kInfiniteDamage, host);
}

void GameSession::respawn() {
    // PlayerManager#respawnPlayer prefers the player's personal spawn point and
    // only falls back to the world spawn when none was set. 1.16.1 also respawns
    // facing due north (yaw 0) regardless of the spawn point's stored angle.
    vitals_.reset();
    const glm::vec3 spawn = hasPlayerSpawn_ ? playerSpawnPosition_ : worldSpawnPosition_;
    player_.setPosition(spawn);
    physicsPreviousPosition_ = spawn;
    physicsCurrentPosition_ = spawn;
}

void GameSession::beginEating(const Item* kind, SimulationHost& host) {
    eating_ = true;
    eatingKind_ = kind;
    eatTicks_ = 0;
    host.onEatingStarted();
}

void GameSession::cancelEating(SimulationHost& host) {
    if (!eating_) {
        return;
    }
    eating_ = false;
    eatingKind_ = nullptr;
    eatTicks_ = 0;
    host.onEatingCancelled();
}

bool GameSession::damageHeldTool(ToolUse use, float blockHardness) {
    const auto cost = toolDurabilityCost(inventory_.selectedStack(), use, blockHardness);
    return cost > 0 && inventory_.damageSelected(cost);
}

void GameSession::spawnBlockDrops(glm::ivec3 position, world::Block block,
                                  const ItemStack& tool,
                                  world::BlockOrientation droppedOrientation) {
    // Crops store their age in the orientation byte; pass it down so the loot
    // table rolls against the stage the crop had grown to.
    const auto drops =
        minedDrops(block, tool, lootRandomState_, world::cropAge(droppedOrientation));
    std::size_t dropIndex = 0U;
    for (const auto& stack : drops.view()) {
        const float angle = static_cast<float>(dropIndex) * 2.39996323F;
        const glm::vec3 velocity = drops.count > 1U
            ? glm::vec3{std::cos(angle) * 0.08F, 0.12F, std::sin(angle) * 0.08F}
            : glm::vec3{0.0F, 0.12F, 0.0F};
        itemEntities_.spawn(glm::vec3{position} + glm::vec3{0.5F}, stack, velocity);
        ++dropIndex;
    }
}

void GameSession::onPlayerDeath() {
    // Vanilla scatters the whole inventory at the death position unless the
    // keepInventory gamerule keeps it on the respawned player.
    if (gameRules_.get<bool>(GameRuleId::KeepInventory)) {
        return;
    }
    const glm::vec3 dropOrigin = player_.position() + glm::vec3{0.0F, 0.9F, 0.0F};
    std::size_t dropIndex = 0U;
    const auto scatter = [&](const ItemStack& stack) {
        const float angle = static_cast<float>(dropIndex++) * 2.39996323F;
        itemEntities_.spawn(dropOrigin, stack,
                            {std::cos(angle) * 0.12F, 0.18F, std::sin(angle) * 0.12F});
    };
    // The cursor stack drops first; then the crafting grid stows into the
    // inventory and its contents scatter with everything else.
    if (!inventory_.cursorStack().empty()) {
        scatter(inventory_.takeCursorStack());
    }
    craftingSystem_.stowAll(inventory_);
    for (std::size_t index = 0; index < Inventory::kSlotCount; ++index) {
        const auto stack = inventory_.slot(index);
        if (stack.empty()) {
            continue;
        }
        scatter(stack);
    }
    inventory_.restore({}, inventory_.selectedHotbarSlot());
}

void GameSession::tickEating(SimulationHost& host) {
    if (!eating_) {
        return;
    }
    ++eatTicks_;
    // LivingEntity#shouldSpawnConsumptionEffects: once the eat is past its
    // first seven ticks, the chew sound (generic.eat) fires every fourth tick.
    // `remaining > 0` keeps the final tick's burst below from double-firing.
    const int remaining = kEatTicks - eatTicks_;
    if (remaining > 0 && remaining % 4 == 0 && remaining <= kEatTicks - 7) {
        host.playEat(player_.position());
    }
    if (eatTicks_ < kEatTicks) {
        return;
    }
    // The meal lands only if the same food is still in hand.
    if (inventory_.selectedStack().item != eatingKind_) {
        cancelEating(host);
        return;
    }
    // Creative players run the full meal but neither gain hunger nor spend the
    // food, exactly like Java 1.16.1 (creative never consumes food).
    if (gameMode_ != GameMode::Creative) {
        const auto food = foodValue(eatingKind_);
        vitals_.eat(food.foodLevel, food.saturationModifier);
        static_cast<void>(inventory_.consumeSelected());
    }
    // consumeItem's burst eat sound, then PlayerEntity.eatFood's burp.
    host.playEat(player_.position());
    host.playBurp(player_.position());
    cancelEating(host);
}

void GameSession::tickPlayerVitals(SimulationHost& host, const world::World& world,
                                   const glm::vec3& previousPosition, bool jumped) {
    if (gameMode_ != GameMode::Survival || vitals_.dead()) {
        return;
    }
    const glm::vec3 delta = player_.position() - previousPosition;
    VitalsInput input;
    input.horizontalDistance = glm::length(glm::vec2{delta.x, delta.z});
    input.verticalDistance = delta.y;
    input.onGround = player_.onGround();
    input.sprinting = player_.sprinting();
    input.jumped = jumped;
    input.inWater = player_.inWater();
    // The camera only catches up after the physics loop, so sample the player's
    // own eye instead of the interpolated render position.
    input.headInWater = submergedInWater(world, player_.eyePosition());
    input.flying = player_.flying();
    input.feetY = player_.position().y;
    const auto result = vitals_.tick(input);
    if (result.damageTaken > 0.0F) {
        if (result.cause == DamageSource::Fall) {
            host.playPlayerFall(player_.position(), result.damageTaken > 4.0F);
        } else {
            host.playPlayerHurt(player_.position());
        }
    }
    if (result.died) {
        host.onPlayerDied();
    }
}

void GameSession::updateMovementAudio(SimulationHost& host, const world::World& world,
                                      const glm::vec3& previousPosition,
                                      const glm::vec3& currentPosition) {
    if (!previousInWater_ && player_.inWater()) {
        host.playSplash(currentPosition, 0.65F);
    }
    previousInWater_ = player_.inWater();
    const glm::vec2 movement{currentPosition.x - previousPosition.x,
                             currentPosition.z - previousPosition.z};
    if (!player_.onGround() || glm::length(movement) < 0.0001F) {
        return;
    }
    footstepDistance_ += glm::length(movement);
    const float stride = player_.sneaking() ? 1.25F : 0.85F;
    if (footstepDistance_ < stride) {
        return;
    }
    footstepDistance_ = std::fmod(footstepDistance_, stride);
    const int blockX = static_cast<int>(std::floor(currentPosition.x));
    const int blockY = static_cast<int>(std::floor(currentPosition.y - 0.05F));
    const int blockZ = static_cast<int>(std::floor(currentPosition.z));
    const auto groundBlock = world.block(blockX, blockY, blockZ);
    if (world::isRenderable(groundBlock)) {
        host.playFootstep(groundBlock, currentPosition,
                          player_.sneaking() ? 0.18F : 0.5F);
    }
}

void GameSession::consumeEntityEvents(SimulationHost& host) {
    for (const auto& [position, died] : worldEntities_.pendingSounds()) {
        if (died) {
            host.playCreatureDeath(position);
        } else {
            host.playCreatureHurt(position);
        }
    }
    worldEntities_.clearPendingSounds();
    for (const auto& [position, drops] : worldEntities_.pendingDrops()) {
        if (gameMode_ != GameMode::Survival) {
            continue;
        }
        std::size_t dropIndex = 0U;
        for (const auto& stack : drops.view()) {
            const float angle = static_cast<float>(dropIndex) * 2.39996323F;
            itemEntities_.spawn(position, stack,
                                {std::cos(angle) * 0.08F, 0.12F, std::sin(angle) * 0.08F});
            ++dropIndex;
        }
    }
    worldEntities_.clearPendingDrops();
}

bool GameSession::submergedInWater(const world::World& world, glm::vec3 position) const {
    return world.block(static_cast<int>(std::floor(position.x)),
                       static_cast<int>(std::floor(position.y)),
                       static_cast<int>(std::floor(position.z))) == world::Block::Water;
}

} // namespace mc::gameplay

#include "gameplay/GameSession.hpp"

#include "gameplay/GameplayMutationSink.hpp"

#include "world/DayNightCycle.hpp"
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

GameSession::GameSession() : player_({24.0F, 78.0F - PlayerController::kEyeHeight, 24.0F}) {
    // A fresh world opens at morning, the same tick the old single clock seeded
    // itself with.
    clocks_.setTotalTicks(world::ClockId::Overworld,
                          static_cast<std::uint64_t>(world::DayNightCycle::kNewWorldTick));
}

// Every public entry that takes a SimulationHost binds it before doing
// anything, because the events raised inside must reach *that* host — the
// per-call host argument is the contract these methods have always had. Once
// P3 Step 3 replaces the bridge with a queue there is only ever one consumer,
// and the parameter can go away entirely.
void GameSession::tick(world::World& world, SimulationHost& host) {
    hostBridge_.setHost(&host);
    // The server tick is unconditional: no gamerule, command or pause reaches
    // it, so everything timed against it (mining, cooldowns, scheduled work)
    // keeps running even when the sun is frozen.
    ++serverTick_;
    // The action timeline (swing arc, ongoing use) advances once per tick, so
    // an action consumes the same ticks at any frame rate.
    playerActions_.tick();
    // doDaylightCycle now means exactly "pause the overworld clock" rather than
    // "stop the one clock everything shares" — 26.1 gates ServerClockManager
    // the same way, with a per-clock paused flag under a global rule.
    clocks_.setPaused(world::ClockId::Overworld,
                      !gameRules_.get<bool>(GameRuleId::DoDaylightCycle));
    clocks_.tick();
    // ServerWorld.tick runs its weather section first, before the world and
    // entities; the auto-cycle is gated on the doWeatherCycle gamerule the same
    // way doDaylightCycle gates the day.
    weatherSystem_.tick(gameRules_.get<bool>(GameRuleId::DoWeatherCycle));
    // Level#updateSkyBrightness, right after the clock and the weather that
    // feed it and before anything reads light. Resolved once here and handed
    // down as a POD: growth, spreading and spawning all read the same fields
    // for the same tick instead of each deriving its own idea of how dark it
    // is. 26.1 does the same thing through EnvironmentAttributes, with
    // invalidateTickCache standing where this single call does.
    environment_ = EnvironmentSnapshot::resolve(static_cast<double>(dayTimeTicks()),
                                                weatherSystem_.rainGradient(),
                                                weatherSystem_.thunderGradient());
    worldSimulation_.setEnvironment(environment_);
    // Take the published input once, at the top of the tick, so the whole tick
    // sees one consistent keyboard state rather than whatever the main thread
    // happened to be writing partway through.
    {
        const std::lock_guard<std::mutex> guard{inputMutex_};
        playerInput_ = sharedInput_;
        playerInput_.jumpPressed = jumpPressed_;
        jumpPressed_ = false;
        forwardPressed_ = false;
    }
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
            // Trampling farmland is an ordinary world edit, so it goes through
            // the service: the section is dirtied and the neighbours (a crop
            // standing on the soil) are told, which the hand-written version
            // never did.
            GameplayMutationSink sink{world, *this};
            static_cast<void>(worldMutations_.setBlock(
                world, {trampleX, trampleY, trampleZ}, world::BlockState{world::Block::Dirt},
                world::MutationFlags::All, world::MutationCause::Gravity, sink));
            events_.publish(SoundEvent{SoundEventKind::BlockBreak,
                                      {static_cast<float>(trampleX) + 0.5F,
                                       static_cast<float>(trampleY) + 0.5F,
                                       static_cast<float>(trampleZ) + 0.5F},
                                      soil});
            events_.publish(
                ParticleEvent{ParticleEventKind::BlockBreak, {trampleX, trampleY, trampleZ}, soil});
            // The crop above loses its farmland and pops.
            worldSimulation_.notifyNeighborChanged(world, {trampleX, trampleY, trampleZ});
        }
    }
    updateMovementAudio(world, physicsPreviousPosition_, player_.position());
    tickPlayerVitals(host, world, physicsPreviousPosition_, player_.jumpedThisTick());
    tickEating(host);
    // The fluid phase runs once per accumulator drain; the renderer never lets
    // more than one batch of overdue water updates stack up across frames.
    bool fluidUpdatePhaseConsumed = false;
    for (const auto& change : worldSimulation_.tick(world, !fluidUpdatePhaseConsumed)) {
        // A simulated break previews too (it used to do so further down, just
        // before its sound), so the edit's immediacy is decided once, here.
        const bool simulatedBreak =
            change.dropped.block() != world::Block::Air && change.worldChanged;
        if (change.worldChanged) {
            events_.publish(WorldEditEvent{
                change.position.x, change.position.y, change.position.z, change.state,
                change.immediateRenderUpdate || simulatedBreak});
        }
        // A simulated break — an attached block that lost its support, a
        // decoration a fluid washed away, or a leaf that decayed — is a real
        // block break: vanilla plays the break sound, throws the break particles
        // and rolls the loot table through World#breakBlock(pos, true), which is
        // game-mode independent (a wall torch drops in creative too). A falling
        // block that could not be placed also rolls its item here, but carries
        // worldChanged == false so it produces no fake break effects or edit.
        // Fluid spread changes carry dropped == Air, so the thousand-cell flows
        // never pay for this pass.
        if (change.dropped.block() != world::Block::Air) {
            if (change.worldChanged) {
                events_.publish(SoundEvent{SoundEventKind::BlockBreak,
                                          {static_cast<float>(change.position.x) + 0.5F,
                                           static_cast<float>(change.position.y) + 0.5F,
                                           static_cast<float>(change.position.z) + 0.5F},
                                          change.dropped.block()});
                events_.publish(ParticleEvent{
                    ParticleEventKind::BlockBreak,
                    {change.position.x, change.position.y, change.position.z},
                    change.dropped.block()});
            }
            // Nobody swung a tool at these, so they roll the same loot table an
            // empty hand would. The dropped *state* travels, so a popped crop
            // rolls its loot from the age it had reached.
            spawnBlockDrops({change.position.x, change.position.y, change.position.z},
                            change.dropped, ItemStack{});
        }
    }
    fluidUpdatePhaseConsumed = true;
    // Every placed furnace smelts on its own now, screen open or not, so a lit
    // furnace left behind keeps cooking. onFurnaceStateChanged swaps each one's
    // block to its lit state so the front face and block light follow the burn.
    furnaceSystem_.tick();
    host.onFurnaceStateChanged();
    chestSystem_.tick();
    if (itemEntities_.tick(world, player_.position(), inventory_) > 0U) {
        events_.publish(SoundEvent{SoundEventKind::ItemPickup, player_.position()});
    }
    // The herd pushes back: Entity#pushAwayFrom moves both parties, so a pig
    // walking into the player nudges them. Difficulty is per-save (level.dat).
    const auto entityTick = worldEntities_.tick(
        world, player_.position(), PlayerController::kWidth, PlayerController::kHeight,
        difficulty_, !vitals_.dead(), gameMode_ == GameMode::Creative,
        simulationRadiusBlocks_);
    player_.applyExternalPush(entityTick.playerPush);
    for (const auto& attack : entityTick.mobAttacks) {
        if (attack.target == ActorReference::player()) {
            // The raw swing. The difficulty scaling is the damage type's own
            // rule now (DamageScaling::WhenCausedByLivingNonPlayer), applied
            // inside the pipeline against the unscaled amount the way
            // LivingEntity#hurt does — the call site used to apply it here,
            // which put it on the wrong side of the invulnerability window.
            static_cast<void>(
                hurtPlayer(DamageType::EntityAttack, attack.amount, host, true));
        }
    }
    // NaturalSpawner: creatures and monsters settle inside the simulation
    // radius, respecting each category's spawnCap and the biome's spawn table.
    // It reads the tick's ambient darkness off the same snapshot the growth
    // checks use, so "dark enough for a monster" and "too dark for grass to
    // spread" can no longer disagree about the time of day.
    naturalSpawner_.tick(world, worldEntities_, player_.position(), simulationRadiusBlocks_,
                         difficulty_, environment_);
    consumeEntityEvents();
    // The authoritative interaction: consume the render thread's queued commands
    // and apply the dig/use decisions once per tick, after every other system
    // has settled (the old renderer applied them per frame between ticks, which
    // is the same ordering — the edits land on the next tick's processing).
    playerInteraction_.tick(*this, world, host, commandQueue_.drain());
    physicsCurrentPosition_ = player_.position();
    // Last, once every system has settled: what the renderer will draw from
    // until the next tick replaces it.
    entitySnapshot_.capture(worldEntities_.entities(), itemEntities_.entities(),
                            worldSimulation_.fallingBlocks());
}

void GameSession::commitInput() {
    const std::lock_guard<std::mutex> guard{inputMutex_};
    sharedInput_ = stagedInput_;
}

void GameSession::setJumpPressed() {
    const std::lock_guard<std::mutex> guard{inputMutex_};
    jumpPressed_ = true;
}

void GameSession::setForwardPressed() {
    const std::lock_guard<std::mutex> guard{inputMutex_};
    forwardPressed_ = true;
}

bool GameSession::forwardPressed() const {
    const std::lock_guard<std::mutex> guard{inputMutex_};
    return forwardPressed_;
}

void GameSession::clearInputEdges() {
    const std::lock_guard<std::mutex> guard{inputMutex_};
    jumpPressed_ = false;
    forwardPressed_ = false;
}

void GameSession::setGameMode(GameMode mode) {
    gameMode_ = mode;
    stagedInput_.flightAllowed = mode == GameMode::Creative;
    const std::lock_guard<std::mutex> guard{inputMutex_};
    sharedInput_.flightAllowed = stagedInput_.flightAllowed;
}

bool GameSession::hurtPlayer(DamageType source, float amount, SimulationHost& host,
                             bool causedByLivingNonPlayer) {
    hostBridge_.setHost(&host);
    if (!vitals_.hurt(amount, source, causedByLivingNonPlayer)) {
        return false;
    }
    events_.publish(SoundEvent{SoundEventKind::PlayerHurt, player_.position()});
    if (vitals_.dead()) {
        die(source, host);
    }
    return true;
}

void GameSession::killPlayer(SimulationHost& host) {
    hostBridge_.setHost(&host);
    (void)hurtPlayer(DamageType::OutOfWorld, kInfiniteDamage, host);
}

bool GameSession::die(DamageType source, SimulationHost& host) {
    hostBridge_.setHost(&host);
    // PlayerEntity#onDeath: the shared beginDeath guard is the `dead` field
    // that keeps onDeath from firing twice, so a tick that both falls and
    // drowns raises the death screen exactly once.
    static_cast<void>(source);
    if (!beginDeath(vitals_.damage())) {
        return false;
    }
    events_.publish(PlayerDiedEvent{});
    return true;
}

void GameSession::respawn() {
    // PlayerManager#respawnPlayer prefers the player's personal spawn point and
    // only falls back to the world spawn when none was set. 1.16.1 also respawns
    // facing due north (yaw 0) regardless of the spawn point's stored angle.
    vitals_.reset();
    const glm::vec3 spawn = hasPlayerSpawn_ ? playerSpawnPosition_ : worldSpawnPosition_;
    player_.resetForRespawn(spawn);
    physicsPreviousPosition_ = spawn;
    physicsCurrentPosition_ = spawn;
}

void GameSession::beginEating(const Item* kind, SimulationHost& host) {
    hostBridge_.setHost(&host);
    eating_ = true;
    eatingKind_ = kind;
    eatTicks_ = 0;
    // The meal is just UseAnimation::Eat on the shared item-use timeline; the
    // renderer reads the countdown from playerActions(), not a private eat state.
    playerActions_.startUsing(InteractionHand::Main, UseAnimation::Eat, kEatTicks);
    host.onEatingStarted();
}

void GameSession::cancelEating(SimulationHost& host) {
    hostBridge_.setHost(&host);
    if (!eating_) {
        return;
    }
    eating_ = false;
    eatingKind_ = nullptr;
    eatTicks_ = 0;
    playerActions_.stopUsing();
    host.onEatingCancelled();
}

bool GameSession::damageHeldTool(ToolUse use, float blockHardness) {
    const auto cost = toolDurabilityCost(inventory_.selectedStack(), use, blockHardness);
    return cost > 0 && inventory_.damageSelected(cost);
}

void GameSession::spawnBlockDrops(glm::ivec3 position, world::BlockState removed,
                                  const ItemStack& tool) {
    // The whole state arrives, so the loot table can roll against the stage a
    // crop had grown to rather than against the bare block.
    const auto drops =
        minedDrops(removed.block(), tool, lootRandomState_, removed.age());
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
        events_.publish(SoundEvent{SoundEventKind::Eat, player_.position()});
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
    events_.publish(SoundEvent{SoundEventKind::Eat, player_.position()});
    events_.publish(SoundEvent{SoundEventKind::Burp, player_.position()});
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
        if (result.cause == DamageType::Fall) {
            events_.publish(SoundEvent{SoundEventKind::PlayerFall, player_.position(),
                                      world::Block::Air, nullptr, 1.0F,
                                      result.damageTaken > 4.0F});
        } else {
            events_.publish(SoundEvent{SoundEventKind::PlayerHurt, player_.position()});
        }
    }
    if (result.died) {
        die(result.cause, host);
    }
}

void GameSession::updateMovementAudio(const world::World& world,
                                      const glm::vec3& previousPosition,
                                      const glm::vec3& currentPosition) {
    if (!previousInWater_ && player_.inWater()) {
        events_.publish(
            SoundEvent{SoundEventKind::Splash, currentPosition, world::Block::Air, nullptr, 0.65F});
    }
    previousInWater_ = player_.inWater();
    const glm::vec2 movement{currentPosition.x - previousPosition.x,
                             currentPosition.z - previousPosition.z};
    if (!player_.onGround() || glm::length(movement) < 0.0001F) {
        return;
    }
    // Entity#move accumulates 0.6 units of step distance per block travelled
    // and plays a sound whenever that accumulator crosses the next integer.
    // Keeping the multiplier here (rather than inventing separate walk/sprint
    // strides) makes sprint cadence rise naturally with its real movement
    // speed while a normal walk stays at the 1.16.1 rhythm.
    footstepDistance_ += glm::length(movement) * 0.6F;
    constexpr float kStepSoundDistance = 1.0F;
    if (footstepDistance_ < kStepSoundDistance) {
        return;
    }
    footstepDistance_ = std::fmod(footstepDistance_, kStepSoundDistance);
    const int blockX = static_cast<int>(std::floor(currentPosition.x));
    const int blockY = static_cast<int>(std::floor(currentPosition.y - 0.05F));
    const int blockZ = static_cast<int>(std::floor(currentPosition.z));
    const auto groundBlock = world.block(blockX, blockY, blockZ);
    if (world::isRenderable(groundBlock)) {
        events_.publish(SoundEvent{SoundEventKind::Footstep, currentPosition, groundBlock,
                                  nullptr, player_.sneaking() ? 0.18F : 0.5F});
    }
}

void GameSession::consumeEntityEvents() {
    for (const auto& sound : worldEntities_.pendingSounds()) {
        // The species rides along so the host plays the right clip for the
        // right creature — a cow's hurt is not a zombie's hurt.
        const auto& type = *sound.type;
        switch (sound.event) {
        case MobSoundEvent::Hurt:
            events_.publish(SoundEvent{SoundEventKind::CreatureHurt, sound.position,
                                      world::Block::Air, &type});
            break;
        case MobSoundEvent::Death:
            events_.publish(SoundEvent{SoundEventKind::CreatureDeath, sound.position,
                                      world::Block::Air, &type});
            break;
        case MobSoundEvent::Ambient:
            events_.publish(SoundEvent{SoundEventKind::CreatureAmbient, sound.position,
                                      world::Block::Air, &type});
            break;
        case MobSoundEvent::Step:
            events_.publish(SoundEvent{SoundEventKind::CreatureStep, sound.position,
                                      world::Block::Air, &type});
            break;
        }
    }
    worldEntities_.clearPendingSounds();
    // A dead mob drops its loot whatever the player's game mode is — vanilla's
    // LivingEntity loot is rolled at death, never gated on the killer's mode.
    for (const auto& [position, drops] : worldEntities_.pendingDrops()) {
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

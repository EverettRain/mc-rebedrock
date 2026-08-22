#include "gameplay/GameSession.hpp"

#include "gameplay/BlockEntityTicker.hpp"
#include "gameplay/GameplayMutationSink.hpp"

#include "world/DayNightCycle.hpp"
#include "world/World.hpp"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace mc::gameplay {

namespace {
constexpr float kInfiniteDamage = std::numeric_limits<float>::infinity();
} // namespace

GameSession::GameSession() {
    auto initialSnapshots = std::make_shared<RenderSnapshots>();
    snapshotPool_.push_back(initialSnapshots);
    storeSnapshotBundle(
        std::shared_ptr<const RenderSnapshots>{std::move(initialSnapshots)});
    // The single local player lives in the slot map; the primary id is always
    // present. (The initializer-list form would call primaryPlayer() before the
    // map was constructed, so the body emplaces the player instead.)
    players_.emplace(kPrimaryPlayerId,
                     ServerPlayer{glm::vec3{24.0F, 78.0F - PlayerController::kEyeHeight, 24.0F}});
    // A fresh world opens at morning, the same tick the old single clock seeded
    // itself with.
    clocks_.setTotalTicks(world::ClockId::Overworld,
                          static_cast<std::uint64_t>(world::DayNightCycle::kNewWorldTick));
    attachGameRuleHandlers();
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
    primaryPlayer().actions.tick();
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
        primaryPlayer().playerInput = primaryPlayer().sharedInput;
        // The jump and sprint-double-tap edges come from their accumulators, not
        // sharedInput's level copy — a press ORed in by any input between two ticks
        // reaches this tick exactly once, then is cleared.
        primaryPlayer().playerInput.jumpPressed = jumpPressed_;
        primaryPlayer().playerInput.forwardPressed = forwardPressed_;
        jumpPressed_ = false;
        forwardPressed_ = false;
    }
    primaryPlayer().physicsPrevious = primaryPlayer().physicsCurrent;
    primaryPlayer().controller.tick(world, primaryPlayer().playerInput);
    // FarmlandBlock#onLandedUpon: on a landing, the player's fall distance
    // (Entity.fallDistance, tracked across frames in PlayerController) decides
    // whether the tilled soil under the feet tramples back to dirt. Vanilla
    // 1.16.1 rolls nextFloat() < fallDistance - 0.5f, so a one-block fall breaks
    // farmland half the time and a taller one almost always; walking never
    // tramples.
    if (primaryPlayer().controller.onGround() && primaryPlayer().controller.fallDistance() > 0.5F) {
        const auto feet = primaryPlayer().controller.position();
        const int trampleX = static_cast<int>(std::floor(feet.x));
        const int trampleY = static_cast<int>(std::floor(feet.y - 0.001F));
        const int trampleZ = static_cast<int>(std::floor(feet.z));
        const auto soil = world.block(trampleX, trampleY, trampleZ);
        lootRandomState_ = lootRandomState_ * 1664525U + 1013904223U;
        const float roll =
            static_cast<float>(lootRandomState_ >> 8) / static_cast<float>(1U << 24);
        if (world::isFarmland(soil) && roll < primaryPlayer().controller.fallDistance() - 0.5F) {
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
    updateMovementAudio(world, primaryPlayer().physicsPrevious, primaryPlayer().controller.position());
    tickPlayerVitals(host, world, primaryPlayer().physicsPrevious, primaryPlayer().controller.jumpedThisTick());
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
    // Block entities advance through the ticker behaviour table, not a hand-list
    // of system tick() calls: each type with a ticker steps its container once,
    // in ascending BlockEntityTypeId order, and a tickless type is skipped by the
    // pre-filter. Chest lids and furnace burns are both driven here.
    tickBlockEntities(BlockEntityTickContext{chestSystem_, trappedChestSystem_, furnaceSystem_});
    // Every placed furnace smelts on its own now, screen open or not. Mirror its
    // authoritative LIT state — after the furnace ticker ran this tick — while
    // this tick owns the server-world write section; the mutation event carries
    // the client mesh/light update later.
    syncFurnaceLitStates(world);
    if (itemEntities_.tick(world, primaryPlayer().controller.position(), primaryPlayer().inventory) > 0U) {
        events_.publish(SoundEvent{SoundEventKind::ItemPickup, primaryPlayer().controller.position()});
    }
    // The herd pushes back: Entity#pushAwayFrom moves both parties, so a pig
    // walking into the player nudges them. Difficulty is per-save (level.dat).
    const auto entityTick = worldEntities_.tick(
        world, primaryPlayer().controller.position(), PlayerController::kWidth, PlayerController::kHeight,
        difficulty_, !primaryPlayer().vitals.dead(), primaryPlayer().gameMode == GameMode::Creative,
        simulationRadiusBlocks_);
    primaryPlayer().controller.applyExternalPush(entityTick.playerPush);
    for (const auto& attack : entityTick.mobAttacks) {
        if (attack.target == ActorReference::player()) {
            // The raw swing. The difficulty scaling is the damage type's own
            // rule now (DamageScaling::WhenCausedByLivingNonPlayer), applied
            // inside the pipeline against the unscaled amount the way
            // LivingEntity#hurt does — the call site used to apply it here,
            // which put it on the wrong side of the invulnerability window.
            static_cast<void>(
                hurtPlayer(kPrimaryPlayerId, DamageType::EntityAttack, attack.amount, host, true));
        }
    }
    // NaturalSpawner: creatures and monsters settle inside the simulation
    // radius, respecting each category's spawnCap and the biome's spawn table.
    // It reads the tick's ambient darkness off the same snapshot the growth
    // checks use, so "dark enough for a monster" and "too dark for grass to
    // spread" can no longer disagree about the time of day.
    naturalSpawner_.tick(world, worldEntities_, primaryPlayer().controller.position(), simulationRadiusBlocks_,
                         difficulty_, environment_);
    consumeEntityEvents();
    // The authoritative interaction: consume the render thread's queued commands
    // and apply the dig/use decisions once per tick, after every other system
    // has settled (the old renderer applied them per frame between ticks, which
    // is the same ordering — the edits land on the next tick's processing).
    playerInteraction_.tick(*this, world, host, commandQueue_.drain());
    // Publish the per-tick snapshots under the caller's world write lock, so
    // the render thread interpolates a coherent frame from them instead of
    // reading live gameplay objects the tick may be mid-mutation on.
    publishSnapshots();
}

void GameSession::publishSnapshots() {
    // The authoritative current position, then everything the renderer reads
    // this frame. Called at the end of tick() and once right after a world
    // load; a cold start restores the player's saved coordinates into the live
    // controller and the physics endpoints before this runs, so the published
    // snapshot carries the real position — never the default (0,0,0).
    primaryPlayer().physicsCurrent = primaryPlayer().controller.position();
    // The per-tick player snapshot, published under the caller's world write
    // lock so the render thread interpolates a coherent frame from it instead
    // of reading live gameplay objects the tick may be mid-mutation on. It is
    // built into a pooled bundle that has no readers and atomically published
    // at the end, so the render thread pins a complete frame without a lock.
    auto snapshots = acquireSnapshotWriteBundle();
    auto& playerTickSnapshot_ = snapshots->player;
    auto& worldSnapshot_ = snapshots->world;
    auto& entitySnapshot_ = snapshots->entities;
    playerTickSnapshot_.serverTick = serverTick_;
    playerTickSnapshot_.swing = primaryPlayer().actions.swing;
    playerTickSnapshot_.use = primaryPlayer().actions.use;
    playerTickSnapshot_.physicsPrevious = primaryPlayer().physicsPrevious;
    playerTickSnapshot_.physicsCurrent = primaryPlayer().physicsCurrent;
    playerTickSnapshot_.previousStride = primaryPlayer().controller.previousStrideDistance();
    playerTickSnapshot_.stride = primaryPlayer().controller.strideDistance();
    playerTickSnapshot_.previousSpeed = primaryPlayer().controller.previousHorizontalSpeed();
    playerTickSnapshot_.speed = primaryPlayer().controller.horizontalSpeed();
    playerTickSnapshot_.sneaking = primaryPlayer().controller.sneaking();
    playerTickSnapshot_.flying = primaryPlayer().controller.flying();
    playerTickSnapshot_.sprinting = primaryPlayer().controller.sprinting();
    playerTickSnapshot_.inWater = primaryPlayer().controller.inWater();
    playerTickSnapshot_.onGround = primaryPlayer().controller.onGround();
    playerTickSnapshot_.previousFieldOfViewMultiplier =
        primaryPlayer().controller.previousFieldOfViewMultiplier();
    playerTickSnapshot_.fieldOfViewMultiplier =
        primaryPlayer().controller.fieldOfViewMultiplier();
    playerTickSnapshot_.heldStack = primaryPlayer().inventory.selectedStack();
    // The dig the interaction pass is mid-way through, so the crack overlay
    // reads the published snapshot instead of the live PlayerInteraction.
    const auto dig = playerInteraction_.digSnapshot();
    playerTickSnapshot_.digging = {dig.active, dig.target, dig.startedTick};
    playerTickSnapshot_.health = primaryPlayer().vitals.health();
    playerTickSnapshot_.foodLevel = primaryPlayer().vitals.foodLevel();
    playerTickSnapshot_.airTicks = primaryPlayer().vitals.airTicks();
    playerTickSnapshot_.ticksSinceDamage = primaryPlayer().vitals.ticksSinceDamage();
    playerTickSnapshot_.gameMode = primaryPlayer().gameMode;
    playerTickSnapshot_.eating = primaryPlayer().eating;
    playerTickSnapshot_.selectedHotbarSlot = primaryPlayer().inventory.selectedHotbarSlot();
    // The render-visible world state, captured under the same lock.
    worldSnapshot_.serverTick = serverTick_;
    worldSnapshot_.previousRainGradient = weatherSystem_.previousRainGradient();
    worldSnapshot_.rainGradient = weatherSystem_.rainGradient();
    worldSnapshot_.previousThunderGradient = weatherSystem_.previousThunderGradient();
    worldSnapshot_.thunderGradient = weatherSystem_.thunderGradient();
    // The gradient-derived flags, matching what the renderer's rain/sky reads.
    worldSnapshot_.raining = weatherSystem_.isRaining();
    worldSnapshot_.thundering = weatherSystem_.isThundering();
    worldSnapshot_.dayTimeTicks = static_cast<double>(dayTimeTicks());
    for (std::size_t index = 0; index < world::kClockCount; ++index) {
        worldSnapshot_.clocks[index] = clocks_.state(static_cast<world::ClockId>(index));
    }
    worldSnapshot_.doDaylightCycle = gameRules_.get<bool>(GameRuleId::DoDaylightCycle);
    worldSnapshot_.doWeatherCycle = gameRules_.get<bool>(GameRuleId::DoWeatherCycle);
    worldSnapshot_.worldSpawnPosition = worldSpawnPosition_;
    worldSnapshot_.playerSpawnPosition = primaryPlayer().spawnPosition;
    worldSnapshot_.playerSpawnYaw = primaryPlayer().spawnYaw;
    worldSnapshot_.hasPlayerSpawn = primaryPlayer().hasSpawn;
    worldSnapshot_.openContainerScreen = openContainerScreen_;
    worldSnapshot_.openChest = openChest_;
    worldSnapshot_.openFurnace = openFurnace_;
    // The chest lid render state, so the world renderer draws lids from the
    // snapshot instead of the live chest system.
    worldSnapshot_.chests.clear();
    worldSnapshot_.chests.reserve(chestSystem_.entities().size());
    for (const auto& chest : chestSystem_.entities()) {
        worldSnapshot_.chests.push_back(
            {chest.position, chest.previousLidAngle, chest.lidAngle});
    }
    // The container screen's display state: the player's inventory and cursor
    // plus the open container's contents, all values so no reference into a
    // gameplay vector survives the tick boundary.
    const auto& primary = primaryPlayer();
    for (std::size_t i = 0; i < Inventory::kSlotCount; ++i) {
        worldSnapshot_.inventorySlots[i] = primary.inventory.slot(i);
    }
    worldSnapshot_.cursorStack = primary.inventory.cursorStack();
    for (std::size_t i = 0; i < 4; ++i) {
        worldSnapshot_.playerCraftingGrid[i] = primary.crafting.playerSlot(i);
    }
    worldSnapshot_.playerCraftingOutput = primary.crafting.playerOutput();
    for (std::size_t i = 0; i < 9; ++i) {
        worldSnapshot_.tableCraftingGrid[i] = primary.crafting.tableSlot(i);
    }
    worldSnapshot_.tableCraftingOutput = primary.crafting.tableOutput();
    if (openChest_.has_value()) {
        if (const auto* chest = chestSystem_.find(*openChest_); chest != nullptr) {
            for (std::size_t i = 0; i < ChestBlockEntity::kSlotCount; ++i) {
                worldSnapshot_.chestItems[i] = chest->items[i];
            }
        }
    }
    if (openFurnace_.has_value()) {
        const gameplay::FurnacePosition furnace{openFurnace_->x, openFurnace_->y,
                                                openFurnace_->z};
        if (const auto* entity = furnaceSystem_.find(furnace); entity != nullptr) {
            worldSnapshot_.furnaceInput = entity->input;
            worldSnapshot_.furnaceFuel = entity->fuel;
            worldSnapshot_.furnaceOutput = entity->output;
        }
        worldSnapshot_.furnaceFuelProgress = furnaceSystem_.fuelProgress(furnace);
        worldSnapshot_.furnaceCookProgress = furnaceSystem_.cookProgress(furnace);
    }
    // Last, once every system has settled: what the renderer will draw from
    // until the next tick replaces it.
    entitySnapshot_.capture(worldEntities_.entities(), itemEntities_.entities(),
                            worldSimulation_.fallingBlocks());
    // Stamp the publish time so the render thread derives the interpolation alpha
    // from this very bundle. Written just before the atomic publish, so a reader
    // that pins this bundle sees a timestamp that belongs to the endpoints it
    // carries — the alpha and the endpoints can never be a tick out of step.
    snapshots->tickPublishRep =
        std::chrono::steady_clock::now().time_since_epoch().count();
    // Publish all three views together. Readers that already loaded the previous
    // shared bundle keep it alive until their copies finish.
    publishSnapshotBundle(snapshots);
}

std::shared_ptr<GameSession::RenderSnapshots> GameSession::acquireSnapshotWriteBundle() {
    const auto current = loadSnapshotBundle();
    for (const auto& candidate : snapshotPool_) {
        if (candidate.get() != current.get() && candidate.use_count() == 1) {
            return candidate;
        }
    }
    auto candidate = std::make_shared<RenderSnapshots>();
    snapshotPool_.push_back(candidate);
    return candidate;
}

void GameSession::publishSnapshotBundle(const std::shared_ptr<RenderSnapshots>& snapshots) {
    storeSnapshotBundle(std::shared_ptr<const RenderSnapshots>{snapshots});
}

std::shared_ptr<const GameSession::RenderSnapshots> GameSession::loadSnapshotBundle() const {
#if defined(__cpp_lib_atomic_shared_ptr) && !defined(__APPLE__)
    return publishedSnapshots_.load(std::memory_order_acquire);
#else
    return std::atomic_load_explicit(&publishedSnapshots_, std::memory_order_acquire);
#endif
}

void GameSession::storeSnapshotBundle(
    std::shared_ptr<const RenderSnapshots> snapshots) {
#if defined(__cpp_lib_atomic_shared_ptr) && !defined(__APPLE__)
    publishedSnapshots_.store(std::move(snapshots), std::memory_order_release);
#else
    std::atomic_store_explicit(
        &publishedSnapshots_, std::move(snapshots), std::memory_order_release);
#endif
}

namespace {
// The interpolation alpha for a bundle published at `tickPublishRep` (a
// steady_clock::duration rep): how far now sits past that publish, in [0,1]. A
// zero rep means nothing has been published yet, so the frame sits exactly on
// the (default) snapshot and the alpha is 0.
[[nodiscard]] float alphaFromPublishRep(std::int64_t tickPublishRep) {
    if (tickPublishRep == 0) {
        return 0.0F;
    }
    const std::chrono::steady_clock::time_point published{
        std::chrono::steady_clock::duration{tickPublishRep}};
    const float elapsed =
        std::chrono::duration<float>{std::chrono::steady_clock::now() - published}.count();
    return std::clamp(elapsed / PlayerController::kTickSeconds, 0.0F, 1.0F);
}
}  // namespace

float GameSession::interpolationAlpha() const {
    return alphaFromPublishRep(loadSnapshotBundle()->tickPublishRep);
}

GameSession::InterpolatedEntities GameSession::entityRenderFrame() const {
    const auto bundle = loadSnapshotBundle();
    return {bundle->entities, alphaFromPublishRep(bundle->tickPublishRep)};
}

void GameSession::commitInput() {
    const std::lock_guard<std::mutex> guard{inputMutex_};
    primaryPlayer().sharedInput = primaryPlayer().stagedInput;
}

void GameSession::applyMovementInput(const MovementInput& intent) {
    // The gated fields are the server's to decide, not the client's: creative
    // flight follows the authoritative game mode, and sprinting needs a food
    // level above six (unless flight is allowed). Deriving them here — instead of
    // trusting the client's copy of gameMode/foodLevel — is the authority the
    // pre-split renderer used to hold and a networked client must not.
    const bool flightAllowed = primaryPlayer().gameMode == GameMode::Creative;
    const bool sprintAllowed = flightAllowed || primaryPlayer().vitals.foodLevel() > 6;
    const std::lock_guard<std::mutex> guard{inputMutex_};
    // Stage the raw intent onto the published input the tick reads at its top.
    // Both stagedInput and sharedInput are set so any residual staged reader sees
    // the same state; the tick takes sharedInput.
    PlayerInput& staged = primaryPlayer().stagedInput;
    staged.forward = intent.forward;
    staged.strafe = intent.strafe;
    staged.lookDirection = intent.lookDirection;
    staged.jumpHeld = intent.jumpHeld;
    staged.descendHeld = intent.descendHeld;
    staged.sneakHeld = intent.sneakHeld;
    staged.sprintHeld = intent.sprintHeld;
    staged.autoJump = intent.autoJump;
    staged.flightAllowed = flightAllowed;
    staged.sprintAllowed = sprintAllowed;
    primaryPlayer().sharedInput = staged;
    // The two edges are consumed once by the next tick and cleared there, so they
    // must be OR-accumulated — not written level-triggered into sharedInput, which
    // a later frame's send in the same between-tick interval would overwrite back
    // to false (losing the press). The tick reads jumpPressed_/forwardPressed_,
    // not sharedInput's copies. This is what makes the sprint double-tap survive
    // when several frames land between two ticks.
    if (intent.jumpPressed) {
        jumpPressed_ = true;
    }
    if (intent.forwardPressed) {
        forwardPressed_ = true;
    }
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
    primaryPlayer().gameMode = mode;
    primaryPlayer().stagedInput.flightAllowed = mode == GameMode::Creative;
    const std::lock_guard<std::mutex> guard{inputMutex_};
    primaryPlayer().sharedInput.flightAllowed = primaryPlayer().stagedInput.flightAllowed;
}

bool GameSession::hurtPlayer(PlayerId playerId, DamageType source, float amount,
                             SimulationHost& host, bool causedByLivingNonPlayer) {
    hostBridge_.setHost(&host);
    auto& player = players_.at(playerId);
    if (!player.vitals.hurt(amount, source, causedByLivingNonPlayer)) {
        return false;
    }
    events_.publish(SoundEvent{SoundEventKind::PlayerHurt, player.controller.position()});
    if (player.vitals.dead()) {
        die(playerId, source, host);
    }
    return true;
}

void GameSession::killPlayer(PlayerId playerId, SimulationHost& host) {
    hostBridge_.setHost(&host);
    (void)hurtPlayer(playerId, DamageType::OutOfWorld, kInfiniteDamage, host);
}

bool GameSession::die(PlayerId playerId, DamageType source, SimulationHost& host) {
    hostBridge_.setHost(&host);
    auto& player = players_.at(playerId);
    // PlayerEntity#onDeath: the shared beginDeath guard is the `dead` field
    // that keeps onDeath from firing twice, so a tick that both falls and
    // drowns raises the death screen exactly once.
    static_cast<void>(source);
    if (!beginDeath(player.vitals.damage())) {
        return false;
    }
    // Closing stows the cursor/crafting grid first, preserving the old death
    // ordering, then inventory scattering completes on the simulation thread
    // before the presentation-only death event is queued.
    closeContainerMenu();
    onPlayerDeath(playerId);
    events_.publish(PlayerDiedEvent{});
    return true;
}

void GameSession::respawn(PlayerId playerId) {
    // PlayerManager#respawnPlayer prefers the player's personal spawn point and
    // only falls back to the world spawn when none was set. 1.16.1 also respawns
    // facing due north (yaw 0) regardless of the spawn point's stored angle.
    auto& player = players_.at(playerId);
    player.vitals.reset();
    const glm::vec3 spawn = player.hasSpawn ? player.spawnPosition : worldSpawnPosition_;
    player.controller.resetForRespawn(spawn);
    player.physicsPrevious = spawn;
    player.physicsCurrent = spawn;
    // Mirror into a fresh published snapshot. resetForRespawn clears sneaking,
    // and the camera derives its eye height from the snapshot's sneaking, so the
    // fresh standing body must publish a standing eye height too.
    const auto current = loadSnapshotBundle();
    auto updated = acquireSnapshotWriteBundle();
    *updated = *current;
    updated->player.physicsPrevious = spawn;
    updated->player.physicsCurrent = spawn;
    updated->player.sneaking = false;
    publishSnapshotBundle(updated);
}

void GameSession::beginEating(PlayerId playerId, const Item* kind, SimulationHost& host) {
    hostBridge_.setHost(&host);
    auto& player = players_.at(playerId);
    player.eating = true;
    player.eatingKind = kind;
    player.eatTicks = 0;
    // The meal is just UseAnimation::Eat on the shared item-use timeline; the
    // renderer reads the countdown from playerActions(), not a private eat state.
    player.actions.startUsing(InteractionHand::Main, UseAnimation::Eat, kEatTicks);
    events_.publish(ClientActionEvent{ClientActionEventKind::EatingStarted});
}

void GameSession::cancelEating(PlayerId playerId, SimulationHost& host) {
    hostBridge_.setHost(&host);
    auto& player = players_.at(playerId);
    if (!player.eating) {
        return;
    }
    player.eating = false;
    player.eatingKind = nullptr;
    player.eatTicks = 0;
    player.actions.stopUsing();
    events_.publish(ClientActionEvent{ClientActionEventKind::EatingCancelled});
}

void GameSession::teleportPlayer(PlayerId playerId, const glm::vec3& feet) {
    auto& player = players_.at(playerId);
    player.controller.setPosition(feet);
    player.physicsPrevious = feet;
    player.physicsCurrent = feet;
    // The renderer's camera reads the published snapshot, not the live
    // controller. Mirror the snapped endpoints into a fresh publish so a
    // teleport that happens between ticks is visible the same frame instead of
    // one tick later.
    const auto current = loadSnapshotBundle();
    auto updated = acquireSnapshotWriteBundle();
    *updated = *current;
    updated->player.physicsPrevious = feet;
    updated->player.physicsCurrent = feet;
    publishSnapshotBundle(updated);
}

void GameSession::setWorldSpawn(const glm::vec3& feet) {
    worldSpawnPosition_ = feet;
}

void GameSession::openContainer(ContainerScreen screen, std::optional<ChestPosition> chest,
                                std::optional<glm::ivec3> furnace) {
    openContainerScreen_ = screen;
    openChest_ = chest;
    openFurnace_ = furnace;
}

void GameSession::closeContainer() {
    openContainerScreen_ = ContainerScreen::PlayerInventory;
    openChest_.reset();
    openFurnace_.reset();
}

bool GameSession::openChestContainer(ChestPosition position) {
    if (!chestSystem_.open(position)) {
        return false;
    }
    openContainer(ContainerScreen::Chest, position);
    return true;
}

void GameSession::closeContainerMenu() {
    auto& inventory = primaryPlayer().inventory;
    inventory.stowCursorStack();
    primaryPlayer().crafting.stowAll(inventory);
    if (openChest_.has_value()) {
        chestSystem_.close(*openChest_);
    }
    closeContainer();
}

void GameSession::resetWorldState() {
    // The crafting grid is per-player (each player carries one); a world switch
    // empties every player's grid, not just the primary's.
    for (auto& [playerId, player] : players_) {
        player.crafting = {};
    }
    worldSimulation_ = {};
    itemEntities_ = {};
    worldEntities_.clear();
    closeContainer();
    chestSystem_ = {};
    trappedChestSystem_ = {};
    furnaceSystem_ = {};
    // Drop the previous world's published state. Readers that pinned it may
    // finish normally; the fresh pool starts with one empty immutable bundle.
    snapshotPool_.clear();
    auto initialSnapshots = std::make_shared<RenderSnapshots>();
    snapshotPool_.push_back(initialSnapshots);
    storeSnapshotBundle(
        std::shared_ptr<const RenderSnapshots>{std::move(initialSnapshots)});
}

void GameSession::syncFurnaceLitStates(world::World& world) {
    // Every furnace block entity carries its own burn, and each one smelts
    // whether or not its screen is open, so the lit state is synced per furnace
    // rather than only for the one the player is looking at. The early-out on an
    // unchanged LIT keeps this to a world write only on the ticks a furnace
    // actually ignites or dies.
    for (const auto& furnace : furnaceSystem_.entities()) {
        const auto& position = furnace.position;
        const auto current = world.state(position.x, position.y, position.z);
        if (current.block() != world::Block::Furnace) {
            continue; // the furnace was mined or replaced out from under its entity
        }
        if (current.lit() == furnace.burning()) {
            continue;
        }
        // Lighting a furnace is a state change on the same block, so the cell
        // keeps its facing and its block entity (and thus its smelt). Through
        // the service like every other edit: because the block is unchanged,
        // onBlockEntityReplaced does not fire and the furnace keeps its entity.
        const auto desired = current.withLit(furnace.burning());
        GameplayMutationSink sink{world, *this};
        static_cast<void>(worldMutations_.setBlock(
            world, {position.x, position.y, position.z}, desired,
            world::MutationFlags::All, world::MutationCause::ScheduledTick, sink));
    }
}

void GameSession::createChestBlockEntity(ChestPosition position) {
    static_cast<void>(chestSystem_.place(position));
}

void GameSession::spawnItemEntity(const glm::vec3& position, ItemStack stack,
                                  const glm::vec3& velocity) {
    static_cast<void>(itemEntities_.spawn(position, stack, velocity));
}

void GameSession::dropCursorStack(const glm::vec3& lookDirection) {
    spawnItemDrop(lookDirection, primaryPlayer().inventory.takeCursorStack());
}

void GameSession::dropSelectedStack(bool wholeStack, const glm::vec3& lookDirection) {
    spawnItemDrop(lookDirection, primaryPlayer().inventory.takeSelected(wholeStack));
}

void GameSession::spawnItemDrop(const glm::vec3& lookDirection, ItemStack stack) {
    if (stack.empty()) {
        return;
    }
    const float lengthSquared = glm::dot(lookDirection, lookDirection);
    const glm::vec3 direction =
        lengthSquared < 1e-9F ? glm::vec3{0.0F, 0.0F, -1.0F} : glm::normalize(lookDirection);
    const glm::vec3 eye = primaryPlayer().controller.eyePosition();
    spawnItemEntity(eye + direction * 0.45F, stack,
                    direction * 0.28F + glm::vec3{0.0F, 0.12F, 0.0F});
}

void GameSession::attachGameRuleHandlers() {
    // randomTickSpeed is the one rule with a runtime mirror (the simulation
    // reads it every tick); doDaylightCycle and keepInventory are read straight
    // from gameRules_ at their use sites instead.
    gameRules_.setChangeHandler(
        [this](GameRuleId id, const GameRuleValueData& value) {
            if (id == GameRuleId::RandomTickSpeed) {
                worldSimulation_.setRandomTickSpeed(std::get<std::int32_t>(value));
            }
        });
}


bool GameSession::damageHeldTool(PlayerId playerId, ToolUse use, float blockHardness) {
    auto& player = players_.at(playerId);
    const auto cost = toolDurabilityCost(player.inventory.selectedStack(), use, blockHardness);
    return cost > 0 && player.inventory.damageSelected(cost);
}

void GameSession::spawnBlockDrops(glm::ivec3 position, world::BlockState removed,
                                  const ItemStack& tool) {
    // The whole state arrives, so the loot table can roll against the stage a
    // crop had grown to rather than against the bare block.
    const auto drops =
        minedDrops(removed.block(), tool, lootRandomState_, removed.age(),
                   world::isSlab(removed.block()) &&
                       removed.slabPortion() == world::SlabPortion::Double);
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

void GameSession::onPlayerDeath(PlayerId playerId) {
    // Vanilla scatters the whole inventory at the death position unless the
    // keepInventory gamerule keeps it on the respawned player.
    if (gameRules_.get<bool>(GameRuleId::KeepInventory)) {
        return;
    }
    auto& player = players_.at(playerId);
    const glm::vec3 dropOrigin = player.controller.position() + glm::vec3{0.0F, 0.9F, 0.0F};
    std::size_t dropIndex = 0U;
    const auto scatter = [&](const ItemStack& stack) {
        const float angle = static_cast<float>(dropIndex++) * 2.39996323F;
        itemEntities_.spawn(dropOrigin, stack,
                            {std::cos(angle) * 0.12F, 0.18F, std::sin(angle) * 0.12F});
    };
    // The cursor stack drops first; then the crafting grid stows into the
    // inventory and its contents scatter with everything else.
    if (!player.inventory.cursorStack().empty()) {
        scatter(player.inventory.takeCursorStack());
    }
    player.crafting.stowAll(player.inventory);
    for (std::size_t index = 0; index < Inventory::kSlotCount; ++index) {
        const auto stack = player.inventory.slot(index);
        if (stack.empty()) {
            continue;
        }
        scatter(stack);
    }
    player.inventory.restore({}, player.inventory.selectedHotbarSlot());
}

void GameSession::tickEating(SimulationHost& host) {
    if (!primaryPlayer().eating) {
        return;
    }
    ++primaryPlayer().eatTicks;
    // LivingEntity#shouldSpawnConsumptionEffects: once the eat is past its
    // first seven ticks, the chew sound (generic.eat) fires every fourth tick.
    // `remaining > 0` keeps the final tick's burst below from double-firing.
    const int remaining = kEatTicks - primaryPlayer().eatTicks;
    if (remaining > 0 && remaining % 4 == 0 && remaining <= kEatTicks - 7) {
        events_.publish(SoundEvent{SoundEventKind::Eat, primaryPlayer().controller.position()});
    }
    if (primaryPlayer().eatTicks < kEatTicks) {
        return;
    }
    // The meal lands only if the same food is still in hand.
    if (primaryPlayer().inventory.selectedStack().item != primaryPlayer().eatingKind) {
        cancelEating(kPrimaryPlayerId, host);
        return;
    }
    // Creative players run the full meal but neither gain hunger nor spend the
    // food, exactly like Java 1.16.1 (creative never consumes food).
    if (primaryPlayer().gameMode != GameMode::Creative) {
        const auto food = foodValue(primaryPlayer().eatingKind);
        primaryPlayer().vitals.eat(food.foodLevel, food.saturationModifier);
        static_cast<void>(primaryPlayer().inventory.consumeSelected());
    }
    // consumeItem's burst eat sound, then PlayerEntity.eatFood's burp.
    events_.publish(SoundEvent{SoundEventKind::Eat, primaryPlayer().controller.position()});
    events_.publish(SoundEvent{SoundEventKind::Burp, primaryPlayer().controller.position()});
    cancelEating(kPrimaryPlayerId, host);
}

void GameSession::tickPlayerVitals(SimulationHost& host, const world::World& world,
                                   const glm::vec3& previousPosition, bool jumped) {
    if (primaryPlayer().gameMode != GameMode::Survival || primaryPlayer().vitals.dead()) {
        return;
    }
    const glm::vec3 delta = primaryPlayer().controller.position() - previousPosition;
    VitalsInput input;
    input.horizontalDistance = glm::length(glm::vec2{delta.x, delta.z});
    input.verticalDistance = delta.y;
    input.onGround = primaryPlayer().controller.onGround();
    input.sprinting = primaryPlayer().controller.sprinting();
    input.jumped = jumped;
    input.inWater = primaryPlayer().controller.inWater();
    // The camera only catches up after the physics loop, so sample the player's
    // own eye instead of the interpolated render position.
    input.headInWater = submergedInWater(world, primaryPlayer().controller.eyePosition());
    input.flying = primaryPlayer().controller.flying();
    input.feetY = primaryPlayer().controller.position().y;
    const auto result = primaryPlayer().vitals.tick(input);
    if (result.damageTaken > 0.0F) {
        if (result.cause == DamageType::Fall) {
            events_.publish(SoundEvent{SoundEventKind::PlayerFall, primaryPlayer().controller.position(),
                                      world::Block::Air, nullptr, 1.0F,
                                      result.damageTaken > 4.0F});
        } else {
            events_.publish(SoundEvent{SoundEventKind::PlayerHurt, primaryPlayer().controller.position()});
        }
    }
    if (result.died) {
        die(kPrimaryPlayerId, result.cause, host);
    }
}

void GameSession::updateMovementAudio(const world::World& world,
                                      const glm::vec3& previousPosition,
                                      const glm::vec3& currentPosition) {
    if (!primaryPlayer().previousInWater && primaryPlayer().controller.inWater()) {
        events_.publish(
            SoundEvent{SoundEventKind::Splash, currentPosition, world::Block::Air, nullptr, 0.65F});
    }
    primaryPlayer().previousInWater = primaryPlayer().controller.inWater();
    const glm::vec2 movement{currentPosition.x - previousPosition.x,
                             currentPosition.z - previousPosition.z};
    if (!primaryPlayer().controller.onGround() || glm::length(movement) < 0.0001F) {
        return;
    }
    // Entity#move accumulates 0.6 units of step distance per block travelled
    // and plays a sound whenever that accumulator crosses the next integer.
    // Keeping the multiplier here (rather than inventing separate walk/sprint
    // strides) makes sprint cadence rise naturally with its real movement
    // speed while a normal walk stays at the 1.16.1 rhythm.
    primaryPlayer().footstepDistance += glm::length(movement) * 0.6F;
    constexpr float kStepSoundDistance = 1.0F;
    if (primaryPlayer().footstepDistance < kStepSoundDistance) {
        return;
    }
    primaryPlayer().footstepDistance = std::fmod(primaryPlayer().footstepDistance, kStepSoundDistance);
    const int blockX = static_cast<int>(std::floor(currentPosition.x));
    const int blockY = static_cast<int>(std::floor(currentPosition.y - 0.05F));
    const int blockZ = static_cast<int>(std::floor(currentPosition.z));
    const auto groundBlock = world.block(blockX, blockY, blockZ);
    if (world::isRenderable(groundBlock)) {
        events_.publish(SoundEvent{SoundEventKind::Footstep, currentPosition, groundBlock,
                                  nullptr, primaryPlayer().controller.sneaking() ? 0.18F : 0.5F});
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

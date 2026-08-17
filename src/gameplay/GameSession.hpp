#pragma once

#include "gameplay/ChestSystem.hpp"
#include "gameplay/CraftingSystem.hpp"
#include "gameplay/Damage.hpp"
#include "gameplay/Difficulty.hpp"
#include "gameplay/EntityRenderSnapshot.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/FurnaceSystem.hpp"
#include "gameplay/GameMode.hpp"
#include "gameplay/GameEvents.hpp"
#include "gameplay/GameRules.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/ItemEntitySystem.hpp"
#include "gameplay/MiningSystem.hpp"
#include "gameplay/NaturalSpawner.hpp"
#include "gameplay/PlayerActionState.hpp"
#include "gameplay/PlayerController.hpp"
#include "gameplay/PlayerInteraction.hpp"
#include "gameplay/PlayerTickSnapshot.hpp"
#include "gameplay/PlayerVitals.hpp"
#include "gameplay/ServerPlayer.hpp"
#include "gameplay/ScreenHandler.hpp"
#include "gameplay/SimulationHostBridge.hpp"
#include "gameplay/WeatherSystem.hpp"
#include "gameplay/WorldSimulation.hpp"
#include "gameplay/WorldSnapshot.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/WorldMutationService.hpp"
#include "world/WorldClock.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace mc::world {
class World;
}

namespace mc::gameplay {

// The render-side reactions the simulation drives but does not own. The Vulkan
// renderer implements it; a headless test provides a recording stub. Kept
// deliberately small: only audio/particle playbacks, the world-edit pipeline
// and a few UI notifications — never gameplay state.
struct SimulationHost {
    virtual ~SimulationHost() = default;

    // A block the simulation changed (fluid, leaf decay, crop growth...). The
    // simulation has already written it to the gameplay world; the host queues
    // the mesh/light rebuild and persists the edit.
    virtual void submitWorldEdit(int x, int y, int z, world::Block block, std::uint8_t fluidLevel,
                                 std::optional<world::BlockOrientation> orientation) = 0;
    // The same edit, carrying the whole block state. The triple above cannot
    // express a furnace's LIT, so an edit that only lights or extinguishes one
    // would arrive at the render streamer as an unlit furnace. Every mutation
    // routed through WorldMutationService uses this form; submitWorldEdit stays
    // for the simulation's BlockChange stream, which is still triple-shaped.
    virtual void submitWorldStateEdit(int x, int y, int z, world::BlockState state) = 0;
    // Rebuilds the light preview for one changed block, so the edit renders
    // with correct light instead of stale stored values.
    virtual void previewBlockEdit(int worldX, int y, int worldZ) = 0;

    // Audio playbacks the fixed-tick loop drives.
    virtual void playBlockBreak(world::Block block, glm::vec3 position) = 0;
    virtual void playItemPickup(glm::vec3 position) = 0;
    // The chewing loop of an ongoing meal: LivingEntity#spawnConsumptionEffects
    // plays the generic.eat sound every fourth tick of the eat.
    virtual void playEat(glm::vec3 position) = 0;
    virtual void playPlayerHurt(glm::vec3 position) = 0;
    virtual void playPlayerFall(glm::vec3 position, bool heavy) = 0;
    virtual void playBurp(glm::vec3 position) = 0;
    // A creature sound event. `type` is the species that owns the clip, so the
    // host plays the right hurt/death/ambient/step sound per species.
    virtual void playCreatureHurt(const entities::EntityType& type, glm::vec3 position) = 0;
    virtual void playCreatureDeath(const entities::EntityType& type, glm::vec3 position) = 0;
    virtual void playCreatureAmbient(const entities::EntityType& type, glm::vec3 position) = 0;
    virtual void playCreatureStep(const entities::EntityType& type, glm::vec3 position) = 0;
    virtual void playFootstep(world::Block ground, glm::vec3 position, float volume) = 0;
    virtual void playSplash(glm::vec3 position, float volume) = 0;

    // Particles the fixed-tick loop throws.
    virtual void spawnBlockBreakParticles(glm::ivec3 position, world::Block block) = 0;

    // The player died this tick. The host raises the death screen and closes
    // the inventory, then the session scatters the drops (onPlayerDeath).
    virtual void onPlayerDied() = 0;
    // A furnace the player has open started/stopped burning; the host swaps its
    // block state and light.
    virtual void onFurnaceStateChanged() = 0;
    // The held-item Eat animation and its cancels.
    virtual void onEatingStarted() = 0;
    virtual void onEatingCancelled() = 0;

    // The interaction side effects gameplay drives but a headless run does not
    // need, so these default to no-ops and only the renderer overrides them.
    // A block being dug makes its periodic hit sound.
    virtual void playBlockHit(world::Block block, glm::vec3 position) {
        static_cast<void>(block);
        static_cast<void>(position);
    }
    // A placed block makes its placement sound.
    virtual void playBlockPlace(world::Block block, glm::vec3 position) {
        static_cast<void>(block);
        static_cast<void>(position);
    }
    // A tool broke from use; the item-break sound plays.
    virtual void playItemBreak(glm::vec3 position) { static_cast<void>(position); }
    // A bucket scooped water; the splash particles play.
    virtual void spawnWaterSplash(glm::vec3 position) { static_cast<void>(position); }
    // A container the interaction decided to open (a crafting table, furnace or
    // chest). The host raises the matching UI; `position` is the container cell.
    virtual void onOpenContainer(ContainerScreen screen, std::optional<glm::ivec3> position) {
        static_cast<void>(screen);
        static_cast<void>(position);
    }
};

// Owns every gameplay system the 20 TPS simulation drives and the fixed-tick
// orchestration between them. This is the piece of the old VulkanRenderer::Impl
// that was pure game logic; the renderer keeps Vulkan, streaming, camera and UI
// and calls tick() once per frame.
class GameSession final {
  public:
    static constexpr int kEatTicks = 32;

    GameSession();
    // GameSession is heavy (inventory, entities, world simulation); no copies.
    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;
    GameSession(GameSession&&) = delete;
    GameSession& operator=(GameSession&&) = delete;

    // Advances the simulation one fixed 20 TPS tick. `world` is the gameplay
    // world the systems simulate into; `host` receives the render-side
    // reactions. Caller repeats it as many times as its accumulator allows.
    void tick(world::World& world, SimulationHost& host);

    // The tick-owned action timeline: the swing arc and the ongoing item use.
    // Advanced once per tick inside tick(); the interaction layer drives it with
    // the semantic actions (swingHand / startUsing / stopUsing), and renderers
    // read the state as a snapshot.
    [[nodiscard]] PlayerActionState& playerActions() { return primaryPlayer().actions; }
    [[nodiscard]] const PlayerActionState& playerActions() const { return primaryPlayer().actions; }
    // The player state published once per tick under the world write lock. The
    // render thread first pins the immutable published bundle, then copies the
    // requested member. A pooled bundle is never reused while a reader still
    // owns it, so this remains lock-free for readers without relying on an
    // unsafe seqlock over ordinary C++ objects.
    [[nodiscard]] PlayerTickSnapshot playerTickSnapshot() const {
        const auto snapshots = loadSnapshotBundle();
        return snapshots->player;
    }
    // The render-visible world state, published once per tick under the world
    // write lock (weather, time of day, clocks, rules). It belongs to the same
    // immutable bundle as the player and entity snapshots.
    [[nodiscard]] WorldSnapshot worldSnapshot() const {
        const auto snapshots = loadSnapshotBundle();
        return snapshots->world;
    }
    // Captures the current authoritative state into the player/world/entity
    // snapshots without advancing the simulation. Called at the end of tick()
    // and once right after a world load, so the renderer's first reads see the
    // restored state instead of the snapshots' default (0,0,0) until the first
    // tick runs.
    void publishSnapshots();

    // The render thread enqueues input intents here; GameSession::tick drains
    // them into PlayerInteraction, which applies them once per tick.
    void enqueueCommand(GameCommand command) { commandQueue_.enqueue(std::move(command)); }

    // The container the player has open, 26.1's AbstractContainerMenu state.
    // Gameplay owns it so the container interaction (ClickSlot/SwapSlot) and a
    // future remote player know what is open without the renderer telling them.
    // `screen` is the container kind; `chest`/`furnace` name the block entity a
    // block-backed container is bound to.
    // Teleports a player to a feet position, snapping the physics interpolation
    // endpoints so the renderer's camera follows without a frame of drift.
    void teleportPlayer(PlayerId playerId, const glm::vec3& feet);
    void setWorldSpawn(const glm::vec3& feet);

    void openContainer(ContainerScreen screen, std::optional<ChestPosition> chest = std::nullopt,
                       std::optional<glm::ivec3> furnace = std::nullopt);
    void closeContainer();
    // Opens a chest block entity and its container in one step; returns false
    // when the chest has no entity. Gameplay owns the chest's open/lid state.
    bool openChestContainer(ChestPosition position);
    // Menu#removed: closes the open container and returns everything the cursor
    // and crafting grid were holding to the inventory. The renderer's inventory
    // close and world switch both end here, so it never reaches into the
    // inventory or crafting systems directly.
    void closeContainerMenu();
    [[nodiscard]] const ContainerScreen& openContainerScreen() const {
        return openContainerScreen_;
    }
    [[nodiscard]] const std::optional<ChestPosition>& openChest() const { return openChest_; }
    [[nodiscard]] const std::optional<glm::ivec3>& openFurnace() const { return openFurnace_; }
    // The current dig (for the renderer's crack overlay).
    [[nodiscard]] const PlayerInteraction& interaction() const { return playerInteraction_; }

    // ---- World lifecycle and the session-driven world writes ----
    // Tears down every per-world system when the renderer switches worlds: the
    // simulation, the item/creature entities, the open container and the
    // block-entity registries. The renderer calls one method instead of reaching
    // into the individual systems.
    void resetWorldState();
    // The furnace lit state is a property of the furnace system mirrored into the
    // world's LIT flag; gameplay owns that mirror so the renderer never reads the
    // furnace entities. Written through the mutation service like every edit.
    void syncFurnaceLitStates(world::World& world);
    // A chest block entity for a test scene that placed the chest block directly
    // into the world (the normal placement path creates it through the mutation
    // sink's onBlockEntityReplaced).
    void createChestBlockEntity(ChestPosition position);
    // Spawns a dropped item entity with an initial velocity — the drop-cursor
    // path, now gameplay-owned instead of the renderer calling itemEntities().
    void spawnItemEntity(const glm::vec3& position, ItemStack stack,
                         const glm::vec3& velocity);
    // Throws the whole cursor stack in front of the player (the click-outside
    // drop), and the selected hotbar stack (the Q drop). `lookDirection` is the
    // renderer's camera direction; gameplay picks the spawn point and velocity.
    void dropCursorStack(const glm::vec3& lookDirection);
    void dropSelectedStack(bool wholeStack, const glm::vec3& lookDirection);
    // The one game rule with a runtime mirror (randomTickSpeed -> simulation) is
    // mirrored by the session itself. The constructor attaches it; a save load
    // replaces gameRules_ with a null-handler copy, so the loader re-attaches it.
    void attachGameRuleHandlers();

    // ---- Actions (the interactive layer calls these) ----
    void setGameMode(GameMode mode);
    // Per-save difficulty, driven by the level.dat value the renderer loads.
    // PlayerVitals owns hunger/damage scaling while GameSession passes the same
    // value to mob despawning and natural spawning, so update both together.
    void setDifficulty(Difficulty difficulty) {
        difficulty_ = difficulty;
        primaryPlayer().vitals.setDifficulty(difficulty);
    }
    // Simulation distance (blocks, horizontal): creatures beyond it are frozen
    // every tick but stay rendered and targetable. Default 64 (4 chunks) keeps
    // the simulated herd close to the player; 0 disables the gate.
    void setSimulationRadius(float blocks) { simulationRadiusBlocks_ = blocks; }
    [[nodiscard]] float simulationRadius() const { return simulationRadiusBlocks_; }
    // The world seed drives the spawner's biome map; a new save or /reload
    // rebuilds it so natural spawns follow the terrain being generated.
    void setWorldSeed(std::uint64_t seed) { naturalSpawner_.setSeed(seed); }
    // 1.16.1 entity.kill(): OutOfWorld damage at infinite magnitude.
    void killPlayer(PlayerId playerId, SimulationHost& host);
    // `causedByLivingNonPlayer` gates the damage type's difficulty scaling: a
    // mob's swing scales with difficulty, the world's does not.
    [[nodiscard]] bool hurtPlayer(PlayerId playerId, DamageType source, float amount,
                                  SimulationHost& host, bool causedByLivingNonPlayer = false);
    // PlayerEntity#onDeath: the one-time death event shared by every lethal
    // source. The beginDeath guard guarantees the death screen fires once even
    // if two sources kill the player in the same tick; the inventory scatter
    // runs through the host's onPlayerDied → onPlayerDeath. Returns false if
    // death was already claimed.
    bool die(PlayerId playerId, DamageType source, SimulationHost& host);
    // ServerPlayerEntity#respawn: restores a respawning player to full health
    // and food at the personal (or world) spawn point, and clears the death
    // momentum/flying/sneaking state so the new body starts clean. The renderer
    // repositions the camera and re-centres streaming after this.
    void respawn(PlayerId playerId);
    void beginEating(PlayerId playerId, const Item* kind, SimulationHost& host);
    void cancelEating(PlayerId playerId, SimulationHost& host);
    // ItemStack#damage on the selected stack; returns true when the tool broke,
    // which is when the renderer plays the break sound.
    [[nodiscard]] bool damageHeldTool(PlayerId playerId, ToolUse use, float blockHardness);
    // Rolls and scatters the loot a broken block drops (mined or simulated).
    // Takes the removed *state*, not just its block: a crop's loot depends on
    // the age it had reached.
    void spawnBlockDrops(glm::ivec3 position, world::BlockState removed, const ItemStack& tool);
    // The gameplay half of death: scatter the inventory (unless keepInventory)
    // and reset the player's stacks. The host raises the death screen first.
    void onPlayerDeath(PlayerId playerId);
    // The interactive layer's per-frame input edges.
    // Key *edges*, set by the main thread's key callback between frames. They
    // ride along with commitInput() rather than being read directly by the
    // tick, for the same reason the rest of the input does.
    void setJumpPressed();
    void setForwardPressed();
    void clearInputEdges();

    // The single local player's authoritative state. N2 packs every player
    // into the slot map; the render accessors below are single-player shortcuts
    // that keep the existing call sites working until N3's snapshots replace
    // them.
    [[nodiscard]] ServerPlayer& primaryPlayer() { return players_.at(kPrimaryPlayerId); }
    [[nodiscard]] const ServerPlayer& primaryPlayer() const {
        return players_.at(kPrimaryPlayerId);
    }
    // Every connected player's authoritative state. N2's multi-player uses this;
    // today it holds the single local player (kPrimaryPlayerId).
    [[nodiscard]] std::unordered_map<PlayerId, ServerPlayer>& players() { return players_; }
    [[nodiscard]] const std::unordered_map<PlayerId, ServerPlayer>& players() const {
        return players_;
    }

    // ---- Render accessors (inline so hot render paths pay nothing) ----
    // Mutable references: the interactive layer stays in the renderer for now
    // and reads/writes the session's systems through these. The simulation's
    // own code uses the private fields directly.
    [[nodiscard]] PlayerController& player() { return primaryPlayer().controller; }
    [[nodiscard]] const PlayerController& player() const { return primaryPlayer().controller; }
    // The input the *main thread* writes each frame. It is staged, not live:
    // the simulation reads its own copy, published by commitInput() under the
    // mutex. Once the tick runs on its own thread this is what stops a
    // half-written keyboard state from being read mid-tick.
    [[nodiscard]] PlayerInput& input() { return primaryPlayer().stagedInput; }
    [[nodiscard]] const PlayerInput& input() const { return primaryPlayer().stagedInput; }
    // Publishes the staged input to the simulation. Called once a frame by the
    // renderer, after the keyboard has been sampled.
    void commitInput();
    [[nodiscard]] PlayerVitals& vitals() { return primaryPlayer().vitals; }
    [[nodiscard]] const PlayerVitals& vitals() const { return primaryPlayer().vitals; }
    [[nodiscard]] Inventory& inventory() { return primaryPlayer().inventory; }
    [[nodiscard]] const Inventory& inventory() const { return primaryPlayer().inventory; }
    [[nodiscard]] CraftingSystem& craftingSystem() { return primaryPlayer().crafting; }
    [[nodiscard]] const CraftingSystem& craftingSystem() const { return primaryPlayer().crafting; }
    [[nodiscard]] GameMode& gameMode() { return primaryPlayer().gameMode; }
    [[nodiscard]] GameMode gameMode() const { return primaryPlayer().gameMode; }
    [[nodiscard]] GameRules& gameRules() { return gameRules_; }
    [[nodiscard]] const GameRules& gameRules() const { return gameRules_; }
    // Where the simulation publishes its four event classes. Subscribers do the
    // reacting; the built-in SimulationHostBridge below is one of them, so the
    // existing host keeps working unchanged.
    [[nodiscard]] GameEventBus& events() { return events_; }
    [[nodiscard]] const GameEventBus& events() const { return events_; }
    // Binds the host that events forward to, for emissions that happen outside
    // tick() — the renderer's interaction path publishes world edits through the
    // mutation sink. tick() binds it too, so a headless caller need not.
    void setEventHost(SimulationHost& host) { hostBridge_.setHost(&host); }
    // Runs everything the simulation queued since the last call. The renderer
    // does this once a frame, after the tick and the interaction pass and
    // before drawing, so an edit made this frame is applied before it is drawn.
    std::size_t drainEvents() { return hostBridge_.drain(); }
    // What the renderer draws creatures from. Rebuilt at the end of every tick,
    // so the draw pass never walks the live entity vector — which the tick is
    // free to reorder, compact and resize. Returned by value after pinning the
    // immutable published bundle, so the read needs no world lock and a later
    // publish cannot rewrite its storage mid-copy.
    [[nodiscard]] EntityRenderSnapshot entitySnapshot() const {
        const auto snapshots = loadSnapshotBundle();
        return snapshots->entities;
    }
    [[nodiscard]] std::size_t pendingEvents() const { return hostBridge_.pending(); }

    // The one path block changes take. Exposed so a caller that already holds
    // the session (the renderer's interaction loop) edits through the same
    // service the session's own systems do.
    [[nodiscard]] world::WorldMutationService& worldMutations() { return worldMutations_; }
    [[nodiscard]] WorldSimulation& worldSimulation() { return worldSimulation_; }
    [[nodiscard]] const WorldSimulation& worldSimulation() const { return worldSimulation_; }
    [[nodiscard]] ItemEntitySystem& itemEntities() { return itemEntities_; }
    [[nodiscard]] const ItemEntitySystem& itemEntities() const { return itemEntities_; }
    [[nodiscard]] EntitySystem& worldEntities() { return worldEntities_; }
    [[nodiscard]] const EntitySystem& worldEntities() const { return worldEntities_; }
    [[nodiscard]] ChestSystem& chestSystem() { return chestSystem_; }
    [[nodiscard]] const ChestSystem& chestSystem() const { return chestSystem_; }
    [[nodiscard]] FurnaceSystem& furnaceSystem() { return furnaceSystem_; }
    [[nodiscard]] const FurnaceSystem& furnaceSystem() const { return furnaceSystem_; }
    [[nodiscard]] WeatherSystem& weatherSystem() { return weatherSystem_; }
    [[nodiscard]] const WeatherSystem& weatherSystem() const { return weatherSystem_; }
    // The environment resolved for the tick in progress. Anything that needs to
    // know how dark it is should read this rather than sampling the clock.
    [[nodiscard]] const EnvironmentSnapshot& environment() const { return environment_; }

    // The time sources that replaced the single frame-driven gameTimeSeconds this
    // class used to hold.
    //
    // serverTick is the world's own clock: it advances once per tick() and is
    // reachable by no gamerule, no command and no pause, which is exactly what
    // mining progress, use cooldowns and every other gameplay timer want. The
    // named clocks (ClockId::Overworld and whatever dimensions follow) carry
    // the sun instead, and only they answer to doDaylightCycle.
    //
    // Frame-local time — chat expiry, the cursor blink, animation
    // interpolation — belongs to the renderer, not here: those advance with
    // real frames even when the simulation is paused.
    [[nodiscard]] std::uint64_t serverTick() const { return serverTick_; }
    void setServerTick(std::uint64_t value) { serverTick_ = value; }
    [[nodiscard]] world::ClockManager& clocks() { return clocks_; }
    [[nodiscard]] const world::ClockManager& clocks() const { return clocks_; }
    // Shorthand for the sun's clock, which is what almost every caller wants.
    [[nodiscard]] std::uint64_t dayTimeTicks() const {
        return clocks_.totalTicks(world::ClockId::Overworld);
    }
    [[nodiscard]] std::uint32_t& lootRandomState() { return lootRandomState_; }
    [[nodiscard]] std::uint32_t lootRandomState() const { return lootRandomState_; }
    [[nodiscard]] glm::vec3& worldSpawnPosition() { return worldSpawnPosition_; }
    [[nodiscard]] const glm::vec3& worldSpawnPosition() const { return worldSpawnPosition_; }
    // The player's personal spawn point (ServerPlayerEntity#spawnPointPosition):
    // /spawnpoint sets it, death respawns there before the world spawn, and it
    // is persisted with the save.
    [[nodiscard]] glm::vec3& playerSpawnPosition() { return primaryPlayer().spawnPosition; }
    [[nodiscard]] const glm::vec3& playerSpawnPosition() const { return primaryPlayer().spawnPosition; }
    [[nodiscard]] float& playerSpawnYaw() { return primaryPlayer().spawnYaw; }
    [[nodiscard]] float playerSpawnYaw() const { return primaryPlayer().spawnYaw; }
    [[nodiscard]] bool& hasPlayerSpawn() { return primaryPlayer().hasSpawn; }
    [[nodiscard]] bool hasPlayerSpawn() const { return primaryPlayer().hasSpawn; }
    [[nodiscard]] glm::vec3& physicsPreviousPosition() { return primaryPlayer().physicsPrevious; }
    [[nodiscard]] const glm::vec3& physicsPreviousPosition() const {
        return primaryPlayer().physicsPrevious;
    }
    [[nodiscard]] glm::vec3& physicsCurrentPosition() { return primaryPlayer().physicsCurrent; }
    [[nodiscard]] const glm::vec3& physicsCurrentPosition() const {
        return primaryPlayer().physicsCurrent;
    }
    [[nodiscard]] bool& eating() { return primaryPlayer().eating; }
    [[nodiscard]] bool eating() const { return primaryPlayer().eating; }
    [[nodiscard]] const Item*& eatingKind() { return primaryPlayer().eatingKind; }
    [[nodiscard]] const Item* eatingKind() const { return primaryPlayer().eatingKind; }
    [[nodiscard]] int& eatTicks() { return primaryPlayer().eatTicks; }
    [[nodiscard]] int eatTicks() const { return primaryPlayer().eatTicks; }
    [[nodiscard]] float& footstepDistance() { return primaryPlayer().footstepDistance; }
    [[nodiscard]] float footstepDistance() const { return primaryPlayer().footstepDistance; }
    [[nodiscard]] bool& previousInWater() { return primaryPlayer().previousInWater; }
    [[nodiscard]] bool previousInWater() const { return primaryPlayer().previousInWater; }
    // The forward double-tap edge is sampled while staging input; access stays
    // under the same mutex used by the tick that consumes it.
    [[nodiscard]] bool forwardPressed() const;

  private:
    void tickEating(SimulationHost& host);
    void tickPlayerVitals(SimulationHost& host, const world::World& world,
                          const glm::vec3& previousPosition, bool jumped);
    void updateMovementAudio(const world::World& world,
                             const glm::vec3& previousPosition, const glm::vec3& currentPosition);
    void consumeEntityEvents();
    [[nodiscard]] bool submergedInWater(const world::World& world, glm::vec3 position) const;
    // The shared item-drop: throws a stack out of the player's eye along
    // `lookDirection`, the way vanilla's PlayerInventory#dropAll does.
    void spawnItemDrop(const glm::vec3& lookDirection, ItemStack stack);

    // Every connected player's authoritative state, keyed by stable PlayerId.
    // ReBedrock today has one local player (kPrimaryPlayerId); remote players
    // land here at N2's LAN tier.
    std::unordered_map<PlayerId, ServerPlayer> players_;
    mutable std::mutex inputMutex_;
    gameplay::Difficulty difficulty_ = gameplay::Difficulty::Normal;
    // ServerWorld#tickChunks simulation distance, in blocks (horizontal).
    float simulationRadiusBlocks_ = 64.0F;
    // All render-visible state is published as one immutable bundle. The writer
    // retains a small pool to reuse vector capacity, but only selects a bundle
    // whose shared ownership has returned to the pool alone. A render reader's
    // atomic load therefore pins the exact allocation it copies until the copy
    // finishes, including across any number of later publications.
    struct RenderSnapshots final {
        PlayerTickSnapshot player;
        WorldSnapshot world;
        EntityRenderSnapshot entities;
    };
    [[nodiscard]] std::shared_ptr<const RenderSnapshots> loadSnapshotBundle() const;
    void storeSnapshotBundle(std::shared_ptr<const RenderSnapshots> snapshots);
#if defined(__cpp_lib_atomic_shared_ptr) && !defined(__APPLE__)
    std::atomic<std::shared_ptr<const RenderSnapshots>> publishedSnapshots_;
#else
    // Older Apple libc++ exposes the C++11 shared_ptr atomic free functions but
    // not atomic<shared_ptr>'s C++20 class specialization.
    std::shared_ptr<const RenderSnapshots> publishedSnapshots_;
#endif
    std::vector<std::shared_ptr<RenderSnapshots>> snapshotPool_;
    GameEventBus events_;
    SimulationHostBridge hostBridge_{events_};
    world::WorldMutationService worldMutations_;
    gameplay::WorldSimulation worldSimulation_;
    gameplay::GameRules gameRules_;
    gameplay::ItemEntitySystem itemEntities_;
    gameplay::EntitySystem worldEntities_;
    gameplay::NaturalSpawner naturalSpawner_{0U};
    gameplay::ChestSystem chestSystem_;
    gameplay::FurnaceSystem furnaceSystem_;
    gameplay::WeatherSystem weatherSystem_;
    // Resolved at the top of every tick from the clock and the weather above.
    EnvironmentSnapshot environment_{};

    glm::vec3 worldSpawnPosition_{24.0F, 76.38F, 24.0F};
    bool jumpPressed_ = false;
    bool forwardPressed_ = false;
    std::uint64_t serverTick_ = 0U;
    world::ClockManager clocks_;
    std::uint32_t lootRandomState_ = 0x9E3779B9U;
    // The authoritative interaction, run at the end of each tick.
    PlayerInteraction playerInteraction_;
    GameCommandQueue commandQueue_;
    [[nodiscard]] std::shared_ptr<RenderSnapshots> acquireSnapshotWriteBundle();
    void publishSnapshotBundle(const std::shared_ptr<RenderSnapshots>& snapshots);
    // The container the player has open (26.1's AbstractContainerMenu).
    ContainerScreen openContainerScreen_ = ContainerScreen::PlayerInventory;
    std::optional<ChestPosition> openChest_;
    std::optional<glm::ivec3> openFurnace_;
};

} // namespace mc::gameplay

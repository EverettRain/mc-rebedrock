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
#include "gameplay/Level.hpp"
#include "gameplay/MiningSystem.hpp"
#include "gameplay/NaturalSpawner.hpp"
#include "gameplay/PlayerActionState.hpp"
#include "gameplay/PlayerController.hpp"
#include "gameplay/PlayerExperience.hpp"
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
#include "world/gen/JavaRandom.hpp"

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
    [[nodiscard]] Difficulty difficulty() const { return difficulty_; }
    // Simulation distance (blocks, horizontal): creatures beyond it are frozen
    // every tick but stay rendered and targetable. Default 64 (4 chunks) keeps
    // the simulated herd close to the player; 0 disables the gate.
    void setSimulationRadius(float blocks) { simulationRadiusBlocks_ = blocks; }
    [[nodiscard]] float simulationRadius() const { return simulationRadiusBlocks_; }
    // The world seed drives the spawner's biome map; a new save or /reload
    // rebuilds it so natural spawns follow the terrain being generated.
    // It also reseeds the experience orb scatter stream (XP-1) — every world
    // gets its own deterministic orb-velocity sequence, the same way the
    // enchantment seed roll and the weather RNG are each salted off this seed
    // but kept in their own independent stream.
    void setWorldSeed(std::uint64_t seed) {
        primaryLevel().spawner.setSeed(seed);
        experienceOrbRandom_.setSeed(seed ^ 0xE3B0C44298FC1C14ULL);
    }
    // XP-1's spawnExperienceOrbs(pos, amount): denomination-splits `amount` into
    // vanilla's fixed orb values and places each one, drawing every scatter
    // velocity from this session's own JavaRandom stream — never the wall
    // clock — so the same save replayed with the same call sequence always
    // spawns the same orbs. XP-2's future source hookups (mob kill / mining /
    // smelting / breeding) all funnel through this one entry point.
    void spawnExperienceOrbs(glm::vec3 position, std::int32_t amount) {
        primaryLevel().experienceOrbs.spawnMany(position, amount, experienceOrbRandom_);
    }
    [[nodiscard]] world::gen::JavaRandom& experienceOrbRandom() { return experienceOrbRandom_; }
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
    // Applies a client's MovementInput to the authoritative player: stages the
    // raw intent and publishes it (as commitInput does), derives the gated fields
    // (flightAllowed/sprintAllowed) from authoritative state rather than the
    // client's copy, and ORs in the jump edge. The server calls this from the
    // channel drain before the tick, so a cross-process client with no session
    // of its own steers the player exactly as the in-process renderer did.
    void applyMovementInput(const MovementInput& intent);
    [[nodiscard]] PlayerVitals& vitals() { return primaryPlayer().vitals; }
    [[nodiscard]] const PlayerVitals& vitals() const { return primaryPlayer().vitals; }
    // XP-0: the level/points/total/enchantmentSeed currency state.
    [[nodiscard]] PlayerExperience& experience() { return primaryPlayer().experience; }
    [[nodiscard]] const PlayerExperience& experience() const { return primaryPlayer().experience; }
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
    // Takes the tick's queued events without applying them, so the transport can
    // encode and send each on the channel (C-1b-3); the client applies them with
    // applyGameEvent. drainEvents stays for the headless tests that apply events
    // to their own host directly.
    [[nodiscard]] std::vector<GameEvent> takeEvents() { return hostBridge_.takeQueued(); }
    // What the renderer draws creatures from. Rebuilt at the end of every tick,
    // so the draw pass never walks the live entity vector — which the tick is
    // free to reorder, compact and resize. Returned by value after pinning the
    // immutable published bundle, so the read needs no world lock and a later
    // publish cannot rewrite its storage mid-copy.
    [[nodiscard]] EntityRenderSnapshot entitySnapshot() const {
        const auto snapshots = loadSnapshotBundle();
        return snapshots->entities;
    }
    // How far the current frame sits past the tick that produced the snapshot the
    // renderer is about to read, in [0,1] — the interpolation alpha, derived from
    // the published bundle's own timestamp rather than a separately clocked
    // accumulator. Using it keeps the alpha in step with the endpoints; the
    // previous SimulationDriver alpha could be a tick ahead of the snapshot for a
    // frame, which is what made moving drops and the swung hand jitter.
    [[nodiscard]] float interpolationAlpha() const;
    // The entity snapshot paired with the interpolation alpha from the *same*
    // published bundle. A moving drop or creature must be interpolated with an
    // alpha that matches its endpoints exactly; taking both from one immutable
    // bundle in a single load makes a tick landing mid-frame impossible to catch
    // half-applied. This is the jitter-free path the entity draws use.
    struct InterpolatedEntities final {
        EntityRenderSnapshot snapshot;
        float alpha = 0.0F;
    };
    [[nodiscard]] InterpolatedEntities entityRenderFrame() const;
    [[nodiscard]] std::size_t pendingEvents() const { return hostBridge_.pending(); }

    // The one path block changes take. Exposed so a caller that already holds
    // the session (the renderer's interaction loop) edits through the same
    // service the session's own systems do.
    [[nodiscard]] world::WorldMutationService& worldMutations() { return worldMutations_; }
    [[nodiscard]] WorldSimulation& worldSimulation() { return worldSimulation_; }
    [[nodiscard]] const WorldSimulation& worldSimulation() const { return worldSimulation_; }
    // The per-dimension simulation systems now live in the primary Level (the
    // dimension the player is in — always the Overworld while the world is
    // single-dimension). These accessors route through it so every existing
    // caller reaches the same systems it always did, one indirection later.
    [[nodiscard]] ItemEntitySystem& itemEntities() { return primaryLevel().items; }
    [[nodiscard]] const ItemEntitySystem& itemEntities() const { return primaryLevel().items; }
    [[nodiscard]] EntitySystem& worldEntities() { return primaryLevel().entities; }
    [[nodiscard]] const EntitySystem& worldEntities() const { return primaryLevel().entities; }
    // XP-1: the experience orb pool, same routing-through-primary-Level shape as
    // items/entities above.
    [[nodiscard]] ExperienceOrbSystem& experienceOrbs() { return primaryLevel().experienceOrbs; }
    [[nodiscard]] const ExperienceOrbSystem& experienceOrbs() const {
        return primaryLevel().experienceOrbs;
    }
    [[nodiscard]] ChestSystem& chestSystem() { return chestSystem_; }
    [[nodiscard]] const ChestSystem& chestSystem() const { return chestSystem_; }
    // The trapped chest's storage (BE3). A separate ChestSystem instance — the
    // whole class is reused, keyed by position, so a trapped chest and a chest
    // never collide even though both are ChestBlockEntity.
    [[nodiscard]] ChestSystem& trappedChestSystem() { return trappedChestSystem_; }
    [[nodiscard]] const ChestSystem& trappedChestSystem() const { return trappedChestSystem_; }
    [[nodiscard]] FurnaceSystem& furnaceSystem() { return furnaceSystem_; }
    [[nodiscard]] const FurnaceSystem& furnaceSystem() const { return furnaceSystem_; }
    [[nodiscard]] WeatherSystem& weatherSystem() { return primaryLevel().weather; }
    [[nodiscard]] const WeatherSystem& weatherSystem() const { return primaryLevel().weather; }
    // CS-4: the generation-time population pass (NaturalSpawner::
    // spawnForChunkGeneration) needs the same per-dimension spawner tick()
    // already reads, so the runtime's chunk-loaded hook can reach it the same
    // routing-through-primary-Level way as worldEntities()/weatherSystem().
    [[nodiscard]] NaturalSpawner& naturalSpawner() { return primaryLevel().spawner; }
    [[nodiscard]] const NaturalSpawner& naturalSpawner() const { return primaryLevel().spawner; }

    // The per-dimension simulation bundle for a dimension (DIM-1). A subscript
    // into the level table, not a lookup. primaryLevel() is the dimension the
    // player is in — the Overworld while the world is single-dimension; DIM-5's
    // dimension transfer will move the player and repoint it.
    [[nodiscard]] Level& level(world::DimensionId id) {
        return levels_[static_cast<std::size_t>(id)];
    }
    [[nodiscard]] const Level& level(world::DimensionId id) const {
        return levels_[static_cast<std::size_t>(id)];
    }
    [[nodiscard]] Level& primaryLevel() { return level(primaryDimension_); }
    [[nodiscard]] const Level& primaryLevel() const { return level(primaryDimension_); }
    [[nodiscard]] world::DimensionId primaryDimension() const { return primaryDimension_; }

    // Wires GameRuntime's World into the level table. GameRuntime owns the World
    // (its lifetime is braided through the world lock / streamer / persist
    // worker), so it binds the reference here once the world is constructed;
    // every per-dimension system reaches its blocks through level(id).world().
    // While single-dimension, the one World backs the Overworld level.
    void bindPrimaryWorld(world::World& world) {
        primaryLevel().id = primaryDimension_;
        primaryLevel().bindWorld(world);
        // The player is in the primary dimension; only it streams (DIM-3).
        primaryLevel().hasPlayer = true;
        primaryLevel().generationSeed = world::dimensionSeed(worldSeed_, primaryDimension_);
        pinFixedTimeClocks();
    }

    // Binds a world to a secondary (non-primary) dimension. DIM-2 has no real
    // second-dimension terrain (that is DIM-3), but the cross-dimension tick loop
    // and its skip/no-force-load semantics are validated by binding a world to,
    // say, the Nether and hand-loading a chunk. Sets the level's id so its clock
    // and DimensionType resolve correctly, and derives its per-dimension terrain
    // seed (DIM-3).
    void bindWorld(world::DimensionId id, world::World& world) {
        level(id).id = id;
        level(id).bindWorld(world);
        level(id).generationSeed = world::dimensionSeed(worldSeed_, id);
    }

    // The world seed, which drives every dimension's derived terrain seed
    // (DIM-3). Set on world load/create before the levels stream; re-derives each
    // bound level's generationSeed so a /reload follows the new seed.
    void setWorldGenerationSeed(std::uint64_t seed) {
        worldSeed_ = seed;
        for (std::size_t i = 0; i < world::kDimensionCount; ++i) {
            const auto dim = static_cast<world::DimensionId>(i);
            level(dim).generationSeed = world::dimensionSeed(seed, dim);
        }
    }
    [[nodiscard]] std::uint64_t worldGenerationSeed() const { return worldSeed_; }

    // DIM-2's cross-dimension tick loop: after the primary level has ticked in
    // full (GameSession::tick), advance every *other* active dimension's passive
    // simulation, in ascending DimensionId order (deterministic — never map
    // iteration order). A dormant dimension (no world, or no loaded chunks) is
    // skipped for the cost of one branch. Reports are accumulated for metering.
    void tickSecondaryLevels();

    // A cross-dimension block read that never forces a chunk to load or generate
    // (JE getChunk(create=false)): if the target dimension has that chunk
    // resident it returns the block, otherwise it records an async load request
    // and returns Air. This is the one legal shape of a cross-dimension query
    // inside a tick — synchronous generation here is the [[lowframe-chunk-unload-
    // io]] long-tail root cause and is forbidden.
    [[nodiscard]] world::Block blockAcrossDimensions(world::DimensionId id, int x, int y, int z);

    // The chunk coordinates a cross-dimension query found unloaded this session,
    // for the streamer to satisfy asynchronously (DIM-3 wires the consumer). Read
    // by the DIM-2 tests to prove a query recorded a request instead of loading.
    struct PendingCrossDimLoad final {
        world::DimensionId dimension = world::DimensionId::Overworld;
        world::ChunkPosition chunk{};

        [[nodiscard]] bool operator==(const PendingCrossDimLoad&) const = default;
    };
    [[nodiscard]] const std::vector<PendingCrossDimLoad>& pendingCrossDimLoads() const {
        return pendingCrossDimLoads_;
    }
    void clearPendingCrossDimLoads() { pendingCrossDimLoads_.clear(); }

    // Records a cross-dimension load request, deduped by (dimension, chunk) so a
    // per-tick query of the same unloaded chunk cannot grow the deferred list
    // without bound (DIM-3 leftover #1, fixed in WG-4). Both the block read and the
    // queued-transfer path route through here.
    void recordPendingCrossDimLoad(PendingCrossDimLoad request);

    // DIM-3: routes the recorded cross-dimension load requests (DIM-2) through the
    // per-dimension generator hook, partitioning them into requests a streamer
    // could satisfy (the dimension has a real terrain generator) and requests
    // that must be deferred (the Nether/End generator seam is not yet filled by
    // worldgen — a streamer must not fabricate their terrain). Draining is
    // asynchronous by construction: it decides routing, it never generates a chunk
    // in the tick. Returns the number of requests routed to a live streamer; the
    // deferred ones stay in pendingCrossDimLoads_ for a future worldgen delivery.
    struct CrossDimLoadRouting final {
        std::size_t routableToStreamer = 0;  // dimension has a generator
        std::size_t deferredNoGenerator = 0; // Nether/End seam, held for worldgen
    };
    CrossDimLoadRouting resolvePendingCrossDimLoads();

    // DIM-5: dimension transfer -----------------------------------------------
    //
    // The result of trying to move a creature to another dimension.
    enum class TransferResult : std::uint8_t {
        Moved,          // detached from the source, re-created in the target
        QueuedAwaitingChunk,  // target chunk not loaded: queued + async requested
        NoTargetWorld,  // the target dimension has no world bound
        SourceMissing,  // no such creature in the source dimension
    };

    // Moves a creature from one dimension's Level to another's, mirroring JE
    // Entity.changeDimension: the creature is detached from the source (no death,
    // no loot), its X/Z scaled by the dimensions' coordinateScale ratio (DIM-0),
    // and re-created in the target Level preserving its state and RNG stream. If
    // the destination chunk is not loaded the transfer is *queued* and an async
    // load request recorded — never a synchronous generate in the tick
    // ([[lowframe-chunk-unload-io]]). Returns what happened.
    TransferResult transferEntity(std::uint64_t entityId, world::DimensionId from,
                                  world::DimensionId to);

    // A transfer waiting on its destination chunk to stream in. Held here (not
    // fabricated) until the streamer delivers the chunk; drainQueuedTransfers
    // retries them.
    struct QueuedTransfer final {
        SimpleEntity entity;              // the detached creature, state intact
        world::DimensionId to = world::DimensionId::Overworld;
        world::ChunkPosition destinationChunk{};
    };
    [[nodiscard]] const std::vector<QueuedTransfer>& queuedTransfers() const {
        return queuedTransfers_;
    }
    // Retries every queued transfer whose destination chunk is now loaded, moving
    // those creatures into their target Level. Returns how many landed. Ones still
    // awaiting their chunk stay queued. Never loads/generates a chunk itself.
    std::size_t drainQueuedTransfers();

    // Moves the player to another dimension: repoints primaryDimension_, hands the
    // hasPlayer flag from the old Level to the new one, and returns the scaled
    // landing position for the runtime to re-anchor the camera/streaming at (the
    // render/stream re-anchor itself lives in GameRuntime, mirroring respawn). The
    // player's actual world binding is GameRuntime's to move; this owns the
    // authoritative dimension identity and the scaled coordinate.
    glm::vec3 transferPlayer(world::DimensionId to);

    // The last cross-dimension tick pass's per-dimension reports, indexed by
    // DimensionId. Metering for the "empty dimension is free" assertions.
    [[nodiscard]] const std::array<LevelTickReport, world::kDimensionCount>&
    secondaryLevelReports() const {
        return secondaryLevelReports_;
    }
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
        // The steady_clock time this bundle was published (end of the tick), as a
        // steady_clock::duration rep (a raw count in the clock's own period, not
        // necessarily nanoseconds — the same encoding SimulationDriver uses). The
        // render thread derives the interpolation alpha from this, so the alpha
        // comes from the very bundle it is interpolating rather than from a
        // separate clock the tick thread updates a moment later. Reading the
        // endpoints and the alpha from one immutable bundle removes the phase race
        // that made moving entities jitter at tick boundaries. 0 before the first
        // publish (alpha then reads 0).
        std::int64_t tickPublishRep = 0;
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
    // The per-dimension simulation bundles (DIM-1): what used to be the exclusive
    // singletons itemEntities_/worldEntities_/weatherSystem_/naturalSpawner_ now
    // live one-per-dimension inside these Levels, reached through level(id) /
    // primaryLevel(). The world is single-dimension for now, so only the Overworld
    // level is active — the array is sized for the full set so DIM-2's cross-
    // dimension tick can walk it without another hoist. World is bound into the
    // level by GameRuntime (bindPrimaryWorld) since GameRuntime owns the World.
    std::array<Level, world::kDimensionCount> levels_{};
    // The dimension the player is in — the Overworld until DIM-5 lets the player
    // change dimension. primaryLevel() routes through this.
    world::DimensionId primaryDimension_ = world::DimensionId::Overworld;
    // DIM-2 metering + cross-dimension bookkeeping.
    std::array<LevelTickReport, world::kDimensionCount> secondaryLevelReports_{};
    std::vector<PendingCrossDimLoad> pendingCrossDimLoads_;
    // DIM-3: the world seed every dimension derives its terrain seed from.
    std::uint64_t worldSeed_ = 0U;
    // DIM-5: transfers waiting on their destination chunk to stream in.
    std::vector<QueuedTransfer> queuedTransfers_;

    // Pins the fixed-time dimensions' clocks (Nether/End: DimensionType.fixedTime)
    // to their fixed value and pauses them, so ClockManager::tick never advances
    // their day. DIM-0 says the Nether sits at 18000 and the End at 6000; a
    // fixed-time clock reads that constant forever.
    void pinFixedTimeClocks() {
        for (std::size_t i = 0; i < world::kDimensionCount; ++i) {
            const auto dim = static_cast<world::DimensionId>(i);
            const auto& type = world::dimensionType(dim);
            if (type.fixedTime.has_value()) {
                clocks_.setTotalTicks(world::clockOf(dim), *type.fixedTime);
                clocks_.setPaused(world::clockOf(dim), true);
            }
        }
    }
    gameplay::ChestSystem chestSystem_;
    gameplay::ChestSystem trappedChestSystem_;
    gameplay::FurnaceSystem furnaceSystem_;
    // Resolved at the top of every tick from the clock and the primary level's
    // weather.
    EnvironmentSnapshot environment_{};

    glm::vec3 worldSpawnPosition_{24.0F, 76.38F, 24.0F};
    bool jumpPressed_ = false;
    bool forwardPressed_ = false;
    std::uint64_t serverTick_ = 0U;
    world::ClockManager clocks_;
    std::uint32_t lootRandomState_ = 0x9E3779B9U;
    // AR-B3: the pressure-plate press/release diff state — see
    // PlayerInteraction.hpp's tickPressurePlates for why this lives here
    // (caller-owned, not hidden static state) rather than inside that
    // function.
    std::vector<glm::ivec3> pressedPlates_;
    // XP-1: the experience orb scatter stream (spawnExperienceOrbs' initial
    // velocities), reseeded from the world seed in setWorldSeed — its own
    // independent JavaRandom stream, salted differently from the enchantment
    // seed roll and the weather/loot RNGs so none of them perturb each other.
    world::gen::JavaRandom experienceOrbRandom_;
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

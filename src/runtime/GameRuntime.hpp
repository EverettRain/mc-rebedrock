#pragma once

#include "gameplay/CommandResult.hpp"
#include "gameplay/DataPackStack.hpp"
#include "gameplay/FunctionManager.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/SimulationDriver.hpp"
#include "gameplay/command/CommandDispatcher.hpp"
#include "gameplay/command/EntitySelector.hpp"
#include "net/LoopbackTransport.hpp"
#include "persistence/SaveRepository.hpp"
#include "world/World.hpp"
#include "world/WorldLock.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mc::world {
class ChunkStreamer;
}

namespace mc::gameplay {
class GameplayMutationSink;
}

namespace mc::assets {
class ResourceProvider;
}

namespace mc::runtime {

// The authoritative world runtime: everything a dedicated server needs, with no
// GLFW or Vulkan anywhere in the dependency chain. It owns the world, the save
// repository, the game session, the simulation driver and the world lock, and
// it is where world loading, saving and switching live.
//
// The renderer is one SimulationHost implementation (GameSession.hpp); a headless
// test supplies a recording stub. The renderer keeps its presentation state —
// audio, menus, the camera, GPU resources, commands, block interaction — and
// reaches the runtime's objects through its accessors.
//
// The member declaration order below is a destruction contract, inherited from
// the renderer layout this class was extracted from: gameSession_ is declared
// before simulationDriver_ so the driver's thread join lands ahead of the
// session's destruction, and the destructor calls stopSimulation() before any
// member the tick touches is torn down. chunkStreamer_ is a reference — the
// application constructs it and it outlives this runtime.
class GameRuntime final {
  public:
    // PACK-1: `dataBase` is the built-in resource floor (Application's
    // `bundled`, or the equivalent the renderer already holds as
    // `resourceProvider` — its `data/` half falls through to the same
    // built-in floor a resource pack never overrides) that per-save data
    // packs layer over. Null keeps every data-driven gameplay table
    // (BlockTagTable, RecipeTable, LootTable, EntityAttributeOverlay,
    // BiomeSpawnTables) exactly as before this card — a caller (existing
    // tests, a headless tool that never touches those tables) that passes
    // nothing gets the pre-PACK-1 behaviour verbatim: the tables keep
    // whichever state their own lazy built-in-default first-use left them in,
    // and loadWorld/unloadWorld do not rebuild them at all. Passing a real
    // provider is what makes loadWorld scan and rebuild
    // `<save>/datapacks/`; the dedicated server and the renderer both do.
    GameRuntime(gameplay::SimulationHost& host, world::ChunkStreamer& chunkStreamer,
                std::filesystem::path saveRoot,
                const assets::ResourceProvider* dataBase = nullptr);
    ~GameRuntime();

    GameRuntime(const GameRuntime&) = delete;
    GameRuntime& operator=(const GameRuntime&) = delete;
    GameRuntime(GameRuntime&&) = delete;
    GameRuntime& operator=(GameRuntime&&) = delete;

    // Simulation control. startSimulation() ticks at 20 TPS on a background
    // thread while simulationActive() is true; MC_REBEDROCK_SYNC_TICK keeps the
    // synchronous fallback (tick() called from the render thread). Headless
    // tests use the synchronous form.
    void startSimulation();
    void stopSimulation();
    void tick();

    // Command dispatch: the chat line the render thread submits is enqueued
    // here and executed on the runtime's dispatcher during the tick, so chat
    // commands are server-authoritative too. The result is read back with
    // takeChatResult() and displayed in the renderer's chat log.
    void enqueueChat(std::string line);
    [[nodiscard]] gameplay::command::CommandDispatcher& commandDispatcher() {
        return commandDispatcher_;
    }

    // Stage C, slice 1b: the client's intents travel over the in-process loopback
    // channel instead of straight into the session's command queue. The renderer
    // (the client end) calls enqueueClientCommand between ticks; the tick drains
    // the server end into the session, so single-player runs the same
    // client→server message path a networked client will. It adds no latency —
    // the frame lands in the server queue immediately and the next tick consumes
    // it, exactly as the direct enqueue did — only a byte encode/decode.
    void enqueueClientCommand(gameplay::GameCommand command);
    // The client's continuous movement intent for the coming tick (D0). The
    // renderer samples the keyboard/look each frame and sends it here instead of
    // writing gameSession().input() directly; the server drains it before the
    // tick and stages it on the authoritative player. Unlike a GameCommand it is
    // not queued for the late interaction drain — movement must be published
    // before the tick reads it, exactly as commitInput() did.
    void sendClientMovement(gameplay::MovementInput input);
    // A session-level client intent (D0): respawn from the death screen, a
    // game-mode switch. Sent on the client end; the server applies it
    // authoritatively when it drains the channel. Respawn is issued while the
    // simulation is paused (the death screen), which no tick would drain — so the
    // caller follows it with applyClientCommandsNow() to apply it at once.
    void sendClientSessionCommand(gameplay::SessionCommand command);
    // Drains and applies the client channel now, outside the tick, then
    // republishes the player/world snapshots — for a synchronous client action
    // taken while the simulation is paused (respawn), so the change lands and the
    // mirror reflects it this frame instead of never (no tick runs while paused).
    // Takes the world write section itself; the caller must not already hold it.
    void applyClientCommandsNow();
    // The client end of the loopback channel. The renderer pumps it each frame
    // to drain the server's per-tick player/world snapshot frames into its
    // ClientMirror (C-1b-2), the same end it sent commands on.
    [[nodiscard]] net::MessageChannel& clientChannel() { return *loopback_.client; }

    // D7: redirect the server's client-facing channel from the internal loopback
    // to an external one — the server end of a TCP connection the dedicated server
    // accepted. After this the tick drains client intents from, and publishes
    // snapshots to, `channel` instead of the loopback; the integrated single-player
    // renderer never calls this and keeps the loopback. Discards anything stale on
    // the newly attached channel (the previous world's frames must not leak in).
    void attachConnection(net::MessageChannel& channel) {
        attachedServerChannel_ = &channel;
        clearChannels();
    }
    // Return to the internal loopback (a connection dropped).
    void detachConnection() { attachedServerChannel_ = nullptr; }
    // Pushes the current player/world snapshots onto the channel now, outside a
    // tick — for a client-initiated synchronous change (respawn, teleport) so the
    // mirror can be pumped to reflect it this frame instead of a tick later. The
    // caller must not be racing a tick (respawn runs while the simulation is
    // paused).
    void publishStateToChannel() { publishSnapshotsToChannel(); }
    // Bytes the last tick's player+world snapshot frames encoded to — the real
    // per-tick snapshot serialization cost the loopback path first pays. Kept so
    // a test or the HUD can pin it and catch a regression that would drag the
    // tick (§13.3#4: every-tick full snapshot encoding).
    [[nodiscard]] std::size_t lastSnapshotEncodedBytes() const { return snapshotEncodedBytes_; }
    // The most recently executed chat command's result, consumed (cleared) by
    // the renderer to append to the chat history.
    [[nodiscard]] std::optional<gameplay::CommandResult> takeChatResult();

    // World lifecycle (the authoritative half; the renderer adds presentation
    // around it). loadWorld restores the session, resets the chunk streamer to
    // the world's seed and edits and requests the spawn area. createWorld makes
    // and persists a fresh save. unloadWorld drops the save and unloads the
    // chunks.
    void loadWorld(persistence::SaveGame save, int viewDistanceChunks);
    // allowCommands (Allow Cheats, CMD-8) rides on the new world. It defaults
    // true so a headless / dedicated caller keeps the historical op4 host; the
    // create screen passes its toggle (vanilla default off) explicitly.
    [[nodiscard]] persistence::SaveGame createWorld(std::string name, std::uint64_t seed,
                                                    gameplay::GameMode mode,
                                                    bool allowCommands = true);
    void unloadWorld();

    // Persists the open world. saveLocked() assumes the caller already holds the
    // world's write section (a command handler saves from inside one); save()
    // takes a read section itself. Returns false when there is no save open.
    [[nodiscard]] bool saveLocked();
    void save();

    // M-3 C5: a chunk left the simulation radius (streamed out). Persist its
    // edits and creatures to the chunk's region file and drop the creatures from
    // the simulation, so a herd outside the radius lives on disk until its chunk
    // streams back in — vanilla's chunk-owned entity lifecycle. The caller holds
    // the world write section (the render thread does when applying an unload
    // batch). The chunk is remembered so a later restoreLoadedChunk brings its
    // herd back. CS-5: also stamps the region record's `populated` marker
    // (unconditionally true — reaching unload means restoreLoadedChunk already
    // ran on this chunk this session, in either branch), so the write survives
    // even when this chunk carries neither edits nor entities right now — a
    // herd that fully wandered off before it unloaded is exactly the case the
    // marker exists to distinguish from "never generated".
    void persistUnloadedChunk(world::ChunkPosition position);
    // A chunk streamed in. Two cases, told apart by unloadedChunks_ (this
    // session's own unload bookkeeping — see persistUnloadedChunk):
    //  - This session already unloaded it: restore the creatures the unload
    //    path persisted for it (the original M-3 C5 behaviour, unchanged).
    //  - Otherwise: CS-4's world-generation-time population pass —
    //    NaturalSpawner::spawnForChunkGeneration — runs once, but only the
    //    first time this chunk is ever seen with no prior record: no persisted
    //    edits, no persisted region entities, and (CS-5) no persisted
    //    `populated` marker (SaveRepository::isChunkPopulated) — the marker is
    //    what catches a chunk that was visited and populated, then had its
    //    entire generation-time herd wander off with no edit ever made in it,
    //    which would otherwise leave no disk trace and look unvisited again.
    //    A chunk that already carries any of these three kinds of record is
    //    never re-populated on top of what survived. The caller holds the
    //    world write section.
    void restoreLoadedChunk(world::ChunkPosition position);
    // STRUCT-2: create the chest block entities a structure placed into a
    // just-generated chunk and fill their loot. Runs once, alongside the CS-4
    // generation-time pass in restoreLoadedChunk, by deterministically replaying
    // the structure placement (structureChestsForChunk) — nothing is threaded out
    // of the generation worker. A no-op with no structure sets, or a structure
    // that places no chests.
    void createStructureChests(world::ChunkPosition position);
    // Block until every queued chunk-unload write has reached disk. Chunk-unload
    // persistence is asynchronous (a background worker batches the region
    // rewrites off the render thread); save, world switch and restore flush
    // internally, so ordinary play never needs this. It exists for callers that
    // must observe the write synchronously — a test reading a region file right
    // after an unload, or a host forcing a hard checkpoint.
    void flushChunkPersistence() { flushAllChunkWrites(); }

    // Accessors.
    [[nodiscard]] gameplay::GameSession& gameSession() { return gameSession_; }
    [[nodiscard]] const gameplay::GameSession& gameSession() const { return gameSession_; }
    [[nodiscard]] world::World& world() { return serverWorld_; }
    [[nodiscard]] const world::World& world() const { return serverWorld_; }
    [[nodiscard]] world::WorldLock& lock() { return worldLock_; }
    [[nodiscard]] world::ChunkStreamer& chunkStreamer() { return chunkStreamer_; }
    [[nodiscard]] persistence::SaveRepository& saveRepository() { return saveRepository_; }
    [[nodiscard]] persistence::SaveGame& currentSave() { return *currentSave_; }
    [[nodiscard]] std::optional<persistence::SaveGame>& currentSaveSlot() {
        return currentSave_;
    }
    [[nodiscard]] std::uint64_t& worldEpoch() { return worldEpoch_; }
    [[nodiscard]] std::atomic_bool& simulationActive() { return simulationActive_; }
    [[nodiscard]] gameplay::SimulationDriver& simulationDriver() { return simulationDriver_; }

    // PACK-1: the open world's per-save data-pack stack — `/datapack
    // list|enable|disable` and PACK-2's `/reload` read and mutate this
    // directly, then call rebuildDataPacks() to apply it. Reflects whatever
    // loadWorld's scan (and any persisted DPKS enable/order) found; empty
    // (nothing discovered/enabled) when no world is open or dataBase was
    // never supplied.
    [[nodiscard]] gameplay::PerSaveDataStack& dataPackStack() { return dataPackStack_; }
    // Re-applies dataPackStack_'s current enable/order onto dataBase_ — the
    // single reusable "rebuild the data-driven gameplay tables" entry point
    // loadWorld, `/datapack enable|disable`, and `/reload` all share.
    // No-op when this runtime was built with no dataBase (dataBase_ is
    // null) — the pre-PACK-1 callers this constructor default keeps working.
    void rebuildDataPacks();

    // PACK-2: the open world's compiled function set — `/function <id>` reads
    // this to run a function by hand; tick()/loadWorld read it to fire
    // `#minecraft:tick`/`#minecraft:load`. Exposed mainly for the test fixture
    // (a headless caller can inspect functionCount()/tickFunctions() without a
    // full world) — GameRuntime itself is the only caller that mutates it via
    // rebuildFunctions().
    [[nodiscard]] gameplay::FunctionManager& functionManager() { return functionManager_; }
    // Recompiles every `.mcfunction` from the current data-pack stack (reusing
    // whatever rebuildDataPacks() just applied — /reload calls both in
    // sequence) and then runs `#minecraft:load` once. Called by loadWorld (a
    // fresh world's own one-time #load) and by `/reload` (a rebuild's one-time
    // #load) — never by tick(). No-op when dataBase_ is null, matching
    // rebuildDataPacks()'s existing opt-in contract.
    void rebuildFunctions();

    // N-Mem, first piece: the server-side resident bytes, measurable headless
    // because no render allocation exists here. Sums each chunk's section state
    // storage plus the light nibble arrays.
    [[nodiscard]] std::size_t serverResidentBytes() const;

  private:
    void registerAuthoritativeCommands();
    void processChatQueue();
    // Pushes the two command-budget game rules (max_command_forks,
    // max_command_sequence_length) onto the dispatcher and the function manager.
    // Called at every entry point that is about to dispatch commands.
    void applyCommandLimitRules();
    // Builds the command source for a chat line run this tick: the primary player
    // (op4 owner) with its current position/rotation and a feedback sink that
    // routes the result to the chat HUD, gated by the sendCommandFeedback
    // gamerule. Rebuilt per line so a moving command updates the next `~`.
    [[nodiscard]] gameplay::command::CommandSource makeCommandSource();
    // Flattens the live players and world entities into the candidate list a
    // target selector runs over (players first, in id order, then entities in
    // vector order — a deterministic sequence for @r/random sort).
    [[nodiscard]] std::vector<gameplay::command::SelectorCandidate> gatherSelectorCandidates() const;
    // Advances the world-seeded command RNG one step and returns it — the
    // deterministic stream @r/random draws from.
    [[nodiscard]] std::uint64_t nextCommandRandom();
    // Resolves a selector against the live pools and kills each target (players
    // through killPlayer, entities through WorldEntities::kill).
    [[nodiscard]] gameplay::CommandResult killSelector(
        const gameplay::command::EntitySelector& selector,
        const gameplay::command::CommandSource& source);
    // CMD-4 content commands, each thin wiring onto an existing system. setblock
    // and fill write through the authoritative mutation path (commandSetBlock);
    // summon goes through WorldEntities::spawn. `mode` picks the setblock/fill
    // variant (replace/keep/destroy, plus outline/hollow for fill).
    [[nodiscard]] bool commandSetBlock(glm::ivec3 cell, world::BlockState state, bool drop,
                                       gameplay::GameplayMutationSink& sink);
    [[nodiscard]] gameplay::CommandResult runSetblock(
        const gameplay::command::CommandContext& context, std::string_view mode);
    [[nodiscard]] gameplay::CommandResult runFill(
        const gameplay::command::CommandContext& context, std::string_view mode);
    [[nodiscard]] gameplay::CommandResult runSummon(
        const gameplay::command::CommandContext& context);
    // XP-3: /experience (/xp) add|set|query <targets> <amount> [points|levels].
    // A thin selector-driven wrapper over PlayerExperience's existing add/set
    // API (XP-0) — no experience-state logic lives here. `mode` is "add"/"set"/
    // "query" and `unit` is "points"/"levels"; query ignores non-player targets
    // by construction (the selector candidates already carry which pool a
    // target came from, but query additionally requires exactly one player
    // target the way vanilla's single-value report does).
    [[nodiscard]] gameplay::CommandResult runExperience(
        const gameplay::command::CommandContext& context, std::string_view mode,
        std::string_view unit);
    // execute (CMD-7): the clause chain is a real redirect subtree on the
    // dispatcher (each `as/at/positioned/…` clause is a node whose SourceModifier
    // forks/gates the source set; `run` redirects to the root). registerExecute
    // wires it, so the runtime holds no hand parser — completion and value types
    // come from the tree like every other command.
    // 26.1's `/time [of <clock>] set|add|pause|resume|rate|query`. Registered
    // apart from the rest because its clause set is built twice — bare and under
    // `of <clock>` — off one shared builder.
    void registerTimeCommand();
    // `/effect give|clear` and `/enchant`, over the shared selector resolution.
    // Free of any per-target-kind effect code: players and creatures hold the
    // same ActiveEffects store.
    gameplay::CommandResult applyEffect(const gameplay::command::EntitySelector& selector,
                                        const gameplay::command::CommandSource& source,
                                        core::StatusEffectId effect, std::int32_t durationTicks,
                                        std::uint8_t amplifier);
    gameplay::CommandResult clearEffect(const gameplay::command::EntitySelector& selector,
                                        const gameplay::command::CommandSource& source,
                                        core::StatusEffectId effect);
    gameplay::CommandResult applyEnchant(const gameplay::command::EntitySelector& selector,
                                         const gameplay::command::CommandSource& source,
                                         gameplay::EnchantmentId enchantment, std::uint8_t level);
    // `/setworldspawn`, the world-wide counterpart to applySpawnPoint's
    // per-player one.
    gameplay::CommandResult applyWorldSpawn(const glm::vec3& position);
    void registerExecute(std::size_t executeNode);
    // A command's missing-argument feedback (CMD6 R1): the usage generated from
    // the node tree, so it never drifts from the command's actual shape.
    [[nodiscard]] gameplay::CommandResult usageError(
        std::string_view command, const gameplay::command::CommandSource& source);
    // Drains the client→server channel into the session's command queue at the
    // start of a tick. Safe to call inside the world write section: the channel
    // and the command queue each carry their own mutex, so this depends on
    // neither the world lock nor the SimulationHostBridge write-section invariant.
    void drainClientCommands();
    // The channel the tick drains client intents from and publishes snapshots to:
    // an attached connection (the dedicated server's TCP socket) when one is set,
    // else the internal loopback's server end (single-player integrated).
    [[nodiscard]] net::MessageChannel& serverChannel() {
        return attachedServerChannel_ != nullptr ? *attachedServerChannel_ : *loopback_.server;
    }
    // Applies one drained session intent to the authoritative session (respawn,
    // game-mode switch).
    void applySessionCommand(const gameplay::SessionCommand& command);
    // Discards any frames still in the loopback channels — a world switch must
    // not let the previous world's queued intents reach the new one (the channel
    // analogue of the streaming epoch).
    void clearChannels();
    // Encodes the tick's player and world snapshots and sends them on the server
    // end of the loopback channel, for the client (renderer/test) to decode into
    // its mirror. Called at the end of tick(), after publishSnapshots, inside the
    // world write section. Records the encoded size for metering.
    void publishSnapshotsToChannel();
    // Background chunk-unload persistence. persistUnloadedChunk does the
    // in-memory extraction (edits + creatures) on the caller's thread and hands
    // the disk write to this worker, so a chunk-unload storm no longer blocks
    // the render thread inside the world write lock. The worker drains the whole
    // queue at once and batches region rewrites (SaveRepository::saveChunks).
    void persistenceWorkerLoop();
    void startPersistenceWorker();
    void stopPersistenceWorker();
    // Block until a specific chunk's queued writes have hit disk (restore reads
    // it back) / until the whole queue is drained (save and world switch must
    // not race the worker on the same region files).
    void flushChunkWrites(world::ChunkPosition position);
    void flushAllChunkWrites();
    // Keeps editsByChunk_ current with currentSave_->edits. edits is append-only
    // (savedEditIndices already relies on this — it stores edit vector indices),
    // so the index is grown incrementally from editsIndexed_; a shrink (world
    // switch replaced the vector) rebuilds from scratch. All reads/writes of
    // edits happen inside the world write section, so this needs no extra lock.
    void refreshEditIndex();
    [[nodiscard]] gameplay::CommandResult applySpawnPoint(
        const std::optional<glm::vec3>& position);

    gameplay::SimulationHost& host_;
    // The in-process loopback pair: the integrated client sends intents and reads
    // snapshots/events on its end, while the server tick drains and publishes on
    // the other. Same-process, no latency; the boundary is the byte codec, not a
    // socket.
    net::LoopbackPair loopback_ = net::makeLoopbackPair();
    // When non-null, the server drains/publishes on this external channel (a TCP
    // connection) instead of loopback_.server — set by attachConnection (D7).
    net::MessageChannel* attachedServerChannel_ = nullptr;
    // Bytes the last tick's player+world snapshot frames encoded to (C-1b-2
    // metering).
    std::size_t snapshotEncodedBytes_ = 0;
    persistence::SaveRepository saveRepository_;
    // PACK-1: the built-in resource floor per-save data packs layer over.
    // Non-owning (Application/DedicatedServer/VulkanRenderer own the real
    // provider, exactly like chunkStreamer_ below); null when the caller
    // passed none, which keeps rebuildDataPacks()/loadWorld's scan a no-op.
    const assets::ResourceProvider* dataBase_ = nullptr;
    gameplay::PerSaveDataStack dataPackStack_;
    // PACK-2: this world's compiled `.mcfunction` set + tag membership,
    // rebuilt by rebuildFunctions() every time rebuildDataPacks() is (loadWorld,
    // /reload) — a function is data-pack content, so it shares the data
    // stack's rebuild timing exactly.
    gameplay::FunctionManager functionManager_;
    world::ChunkStreamer& chunkStreamer_;
    world::World serverWorld_;
    gameplay::GameSession gameSession_;
    gameplay::SimulationDriver simulationDriver_;
    world::WorldLock worldLock_;
    std::atomic_bool simulationActive_{false};
    std::optional<persistence::SaveGame> currentSave_;
    std::uint64_t worldEpoch_ = 0U;
    // Chunks the unload path wrote to region files this session and has not yet
    // restored. Their creatures are on disk and out of the simulation — the only
    // region records a save must merge, because every other disk record is either
    // a mirror the fresh gather replaces or a stale copy of a creature that moved
    // or despawned. A later stream of one of them restores its herd
    // (restoreLoadedChunk). Cleared on world load.
    std::unordered_set<world::ChunkPosition, world::ChunkPositionHash> unloadedChunks_;
    // CS-4: chunks this session has already run the generation-time population
    // pass for. Session-scoped dedup — the cheap, common case (a chunk
    // unloaded and re-streamed while the world stays open, e.g. the player
    // walks away and back) never re-populates, without touching disk. Cleared
    // on world load alongside unloadedChunks_. This does not by itself cover a
    // *new process* reopening the same save; restoreLoadedChunk additionally
    // consults editsByChunk_/loadChunkEntities for that case (see its comment).
    std::unordered_set<world::ChunkPosition, world::ChunkPositionHash> populatedChunks_;
    // Derived per-chunk index into currentSave_->edits, so persisting an unloaded
    // chunk collects its edits in O(chunk's edits) instead of scanning the whole
    // flat edit list per chunk. It is a cache (the flat SaveGame DTO stays the
    // authoritative store, per §2.3), rebuilt on world load. editsIndexed_ is the
    // prefix of currentSave_->edits already folded in.
    std::unordered_map<world::ChunkPosition, std::vector<std::size_t>, world::ChunkPositionHash>
        editsByChunk_;
    std::size_t editsIndexed_ = 0;
    // The server-authoritative command tree. The renderer registers its
    // client-only commands on it through commandDispatcher().
    gameplay::command::CommandDispatcher commandDispatcher_;
    // The chat line the render thread enqueues and the command result it reads
    // back are exchanged across the sim/render boundary, so the queue and the
    // single-slot result are guarded together.
    std::mutex chatMutex_;
    std::vector<std::string> chatQueue_;
    std::optional<gameplay::CommandResult> chatResult_;
    // The deterministic stream `@r`/random selectors draw from, seeded from the
    // world seed on load (see loadWorld). Transient — never saved; a fixed world
    // plus a fixed command sequence reproduces the same picks.
    std::uint64_t commandRandomState_ = 0x9E3779B97F4A7C15ULL;
    // Whether cheats are allowed in the loaded world (CMD-8), mirrored from the
    // save on loadWorld. makeCommandSource reads it to pick the host's op level:
    // true → Owners (op4), false → All (client-side commands only). Defaults true
    // so a runtime with no world loaded (or a pre-CMD-8 world) behaves as before.
    bool commandsAllowed_ = true;

    // Background chunk-unload persistence worker and its queue. The worker lives
    // for the whole runtime; the destructor stops and joins it after the
    // simulation thread. persistIdentifier_ names the save the queued records
    // belong to — the worker never touches currentSave_, and a world switch
    // flushes the queue first so it never mixes two saves. persistPending_ counts
    // a chunk's outstanding writes so flushChunkWrites can wait on exactly one.
    std::thread persistenceThread_;
    std::mutex persistMutex_;
    std::condition_variable persistWakeCv_;
    std::condition_variable persistDoneCv_;
    std::deque<persistence::ChunkPersistRecord> persistQueue_;
    std::string persistIdentifier_;
    std::unordered_map<world::ChunkPosition, int, world::ChunkPositionHash> persistPending_;
    bool persistStopping_ = false;
    bool persistBusy_ = false;
};

} // namespace mc::runtime

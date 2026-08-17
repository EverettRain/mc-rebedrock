#include "runtime/GameRuntime.hpp"

#include "gameplay/ContentRegistry.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/command/GameplayArguments.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"
#include "world/ChunkStreamer.hpp"
#include "world/DayNightCycle.hpp"
#include "world/WorldConstants.hpp"

#include "core/FrameTrace.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace mc::runtime {

namespace {
// The spawn area stays loaded for the whole session, vanilla-style. Mirrors the
// constant the renderer used before this class owned world loading.
constexpr int kSpawnChunkRadius = 4;

// A live creature, reduced to the fields a region record and the save both
// carry. Shared by the save point and the chunk-unload path.
[[nodiscard]] persistence::PersistentEntity toPersistentEntity(const gameplay::SimpleEntity& entity) {
    persistence::PersistentEntity record;
    record.species = std::string{entity.type->id().path};
    record.x = entity.position.x;
    record.y = entity.position.y;
    record.z = entity.position.z;
    record.yaw = entity.yaw;
    record.vx = entity.velocity.x;
    record.vy = entity.velocity.y;
    record.vz = entity.velocity.z;
    record.health = entity.damage.health;
    record.angerTicks = entity.angerTicks;
    record.ageTicks = entity.ageTicks;
    record.rngState = entity.rngState;
    return record;
}
}  // namespace

GameRuntime::GameRuntime(gameplay::SimulationHost& host, world::ChunkStreamer& chunkStreamer,
                         std::filesystem::path saveRoot)
    : host_(host), saveRepository_(std::move(saveRoot)), chunkStreamer_(chunkStreamer) {
    // Bind the event host up front so the world-edit events a mutation publishes
    // reach the host even before the first tick (tick() re-binds it anyway).
    gameSession_.setEventHost(host_);
    registerAuthoritativeCommands();
    startPersistenceWorker();
}

GameRuntime::~GameRuntime() {
    // Joins the simulation thread before any member the tick touches is torn
    // down. Idempotent: the owner may stop it explicitly (the renderer does,
    // ahead of its Vulkan teardown) and this is then a no-op.
    stopSimulation();
    // Drain and join the persistence worker last, so every chunk the unload path
    // queued this session actually reaches disk before the save repository dies.
    stopPersistenceWorker();
}

void GameRuntime::startSimulation() {
    if (std::getenv("MC_REBEDROCK_SYNC_TICK") != nullptr) {
        std::cout << "[sim] MC_REBEDROCK_SYNC_TICK set: ticking on the render thread\n";
        return;
    }
    simulationDriver_.start([this] { tick(); }, [this] {
        return simulationActive_.load(std::memory_order_acquire);
    });
    std::cout << "[sim] simulation thread started (20 TPS)\n";
}

void GameRuntime::stopSimulation() {
    simulationDriver_.stop();
}

void GameRuntime::tick() {
    const auto tickWrite = worldLock_.write();
    gameSession_.tick(serverWorld_, host_);
    processChatQueue();
}

void GameRuntime::enqueueChat(std::string line) {
    const std::lock_guard<std::mutex> guard{chatMutex_};
    chatQueue_.push_back(std::move(line));
}

std::optional<gameplay::CommandResult> GameRuntime::takeChatResult() {
    const std::lock_guard<std::mutex> guard{chatMutex_};
    auto result = chatResult_;
    chatResult_.reset();
    return result;
}

void GameRuntime::processChatQueue() {
    // Swap the queue out under the lock so a line enqueued during execution is
    // not lost, then run the commands without holding it (a command may reach
    // back into the runtime). The single-slot result is written back under it.
    std::vector<std::string> lines;
    {
        const std::lock_guard<std::mutex> guard{chatMutex_};
        lines = std::move(chatQueue_);
        chatQueue_.clear();
    }
    for (auto& line : lines) {
        const auto result = commandDispatcher_.execute(line);
        const std::lock_guard<std::mutex> guard{chatMutex_};
        chatResult_ = result;
    }
}

void GameRuntime::loadWorld(persistence::SaveGame save, int viewDistanceChunks) {
    // Drain any writes still queued for a previous world before switching saves,
    // so the worker never writes an outgoing world's records under the new
    // identifier.
    flushAllChunkWrites();
    // The incoming world replaces the edit list; drop the derived per-chunk index
    // so refreshEditIndex rebuilds it from the new edits.
    editsByChunk_.clear();
    editsIndexed_ = 0;
    currentSave_ = std::move(save);
    // No chunk has unloaded yet in the fresh world; the unload-then-restore
    // bookkeeping starts empty.
    unloadedChunks_.clear();
    gameSession_.inventory().restore(currentSave_->inventory, currentSave_->selectedHotbarSlot);
    gameSession_.chestSystem().restore(currentSave_->chests);
    gameSession_.furnaceSystem().restore(currentSave_->furnaces);
    // Restore the herd a saved world carried, resolving species by their
    // registered id so a species this build no longer knows is skipped instead
    // of failing to open the world.
    for (const auto& record : currentSave_->entities) {
        const auto* type = gameplay::entities::entityTypeRegistry().byId(record.species);
        if (type == nullptr) {
            continue;
        }
        gameSession_.worldEntities().restore({record.x, record.y, record.z}, *type, record.yaw,
                                             {record.vx, record.vy, record.vz}, record.health,
                                             record.angerTicks, record.ageTicks, record.rngState);
    }
    // Format 16: dropped items and blocks mid-fall. Before it, everything a
    // player had thrown or mined but not picked up vanished on reload.
    for (const auto& drop : currentSave_->itemDrops) {
        gameSession_.itemEntities().restore({drop.x, drop.y, drop.z}, drop.stack,
                                            {drop.vx, drop.vy, drop.vz}, drop.ageTicks);
    }
    for (const auto& falling : currentSave_->fallingBlocks) {
        gameSession_.worldSimulation().restoreFallingBlock(
            {falling.x, falling.y, falling.z}, falling.block, falling.verticalVelocity);
    }
    gameSession_.gameMode() = currentSave_->gameMode;
    // The world owns its difficulty, the way level.dat does in vanilla.
    gameSession_.setDifficulty(currentSave_->difficulty);
    // Game rules travel with the world too. The copy from the loaded save
    // carries a null change handler, so the owner re-attaches its own and
    // applies the one rule with a runtime mirror.
    gameSession_.gameRules() = currentSave_->gameRules;
    gameSession_.attachGameRuleHandlers();
    gameSession_.worldSimulation().setRandomTickSpeed(
        gameSession_.gameRules().get<std::int32_t>(gameplay::GameRuleId::RandomTickSpeed));
    // The world tick and the named clocks restore separately, so a save made
    // with the sun frozen reopens with the sun still where it was and the world
    // tick exactly where it left off. A pre-format-13 save has both backfilled
    // from the legacy gameTimeSeconds by the loader.
    gameSession_.setServerTick(currentSave_->serverTick);
    for (std::size_t index = 0; index < world::kClockCount; ++index) {
        gameSession_.clocks().setState(static_cast<world::ClockId>(index),
                                       currentSave_->clocks[index]);
    }
    // The weather travels with the save too; restore() also fades the gradients
    // straight to their flags (World#initWeatherGradients), so a world saved
    // mid-rain reopens raining instead of fading up.
    gameSession_.weatherSystem().restore(currentSave_->weather);
    const glm::vec3 initialFeet =
        currentSave_->hasPlayerPosition
            ? glm::vec3{currentSave_->playerX, currentSave_->playerY, currentSave_->playerZ}
            : glm::vec3{24.0F, 76.38F, 24.0F};
    gameSession_.player() = gameplay::PlayerController{initialFeet};
    gameSession_.vitals().restore(currentSave_->playerHealth, currentSave_->playerFoodLevel,
                                  currentSave_->playerSaturation, currentSave_->playerAirTicks);
    // A world saved with an empty health bar reopens with a live player.
    if (gameSession_.vitals().dead()) {
        gameSession_.vitals().reset();
    }
    gameSession_.worldSpawnPosition() = initialFeet;
    gameSession_.physicsPreviousPosition() = initialFeet;
    gameSession_.physicsCurrentPosition() = initialFeet;
    // The /spawnpoint result, if the save carried one; death respawns there.
    gameSession_.hasPlayerSpawn() = currentSave_->hasSpawnPoint;
    gameSession_.playerSpawnPosition() =
        glm::vec3{currentSave_->spawnX, currentSave_->spawnY, currentSave_->spawnZ};
    gameSession_.playerSpawnYaw() = currentSave_->spawnYaw;
    // Keep the loaded spawn's chunks loaded for the session, vanilla-style.
    chunkStreamer_.protectChunks(world::chunkPositionFromWorld(initialFeet.x, initialFeet.z),
                                 kSpawnChunkRadius);
    worldEpoch_ = chunkStreamer_.resetWorld(currentSave_->summary.seed, currentSave_->edits);
    // Natural spawning reads the biome map from the same seed that drives the
    // terrain, so spawns follow the biome being generated.
    gameSession_.setWorldSeed(currentSave_->summary.seed);
    gameSession_.lootRandomState() =
        static_cast<std::uint32_t>(currentSave_->summary.seed) ^
        static_cast<std::uint32_t>(currentSave_->summary.seed >> 32U) ^ 0x9E3779B9U;
    // The weather auto-cycle's RNG is seeded from the world the same way the
    // loot RNG is; the timers themselves come from the save above.
    gameSession_.weatherSystem().seedRandom(
        static_cast<std::uint32_t>(currentSave_->summary.seed) ^
        static_cast<std::uint32_t>(currentSave_->summary.seed >> 32U) ^ 0x57E4F10AU);
    // Two-phase load: ask for a small area around the player first so the world
    // opens quickly, then widen to the full view distance once the load screen
    // clears and let the rest stream in during play. The unload radius stays at
    // the full view distance so nothing wrongly evicts while the area is small.
    const int spawnRadius = std::min(viewDistanceChunks, kSpawnChunkRadius);
    chunkStreamer_.setRadii(spawnRadius, viewDistanceChunks);
    chunkStreamer_.request(world::chunkPositionFromWorld(initialFeet.x, initialFeet.z));
    // Publish a complete snapshot of the just-restored state. The simulation
    // thread has not started, so nothing would refresh the snapshots otherwise —
    // the renderer's first reads (camera, F3, held item) must see the saved
    // position, not the default (0,0,0) the snapshot holds until the first tick.
    gameSession_.publishSnapshots();
}

persistence::SaveGame GameRuntime::createWorld(std::string name, std::uint64_t seed,
                                               gameplay::GameMode mode) {
    auto save = saveRepository_.create(name, seed);
    save.gameMode = mode;
    // A new world starts on Normal difficulty, exactly like vanilla; each world
    // then owns the setting from here on.
    save.difficulty = gameplay::Difficulty::Normal;
    gameplay::Inventory initialInventory;
    save.inventory = initialInventory.slots();
    save.selectedHotbarSlot = initialInventory.selectedHotbarSlot();
    saveRepository_.save(save);
    return save;
}

void GameRuntime::unloadWorld() {
    // Land every queued chunk of the outgoing world before dropping the save, so
    // its region files are complete and the queue never carries records into the
    // next world.
    flushAllChunkWrites();
    editsByChunk_.clear();
    editsIndexed_ = 0;
    currentSave_.reset();
    worldEpoch_ = chunkStreamer_.resetWorld(0U);
}

bool GameRuntime::saveLocked() {
    if (!currentSave_.has_value() || currentSave_->summary.identifier.empty()) {
        return false;
    }
    // The full save rewrites the same region files the background worker writes,
    // so drain the queue first: after this the queue is empty and, because the
    // caller holds the world write section, no new unload can enqueue while
    // save() rewrites the regions.
    flushAllChunkWrites();
    currentSave_->hasPlayerPosition = true;
    const auto position = gameSession_.player().position();
    currentSave_->playerX = position.x;
    currentSave_->playerY = position.y;
    currentSave_->playerZ = position.z;
    // gameTimeSeconds is a legacy field now that the server tick and clocks
    // carry the time; keep it filled with the elapsed-seconds equivalent of the
    // world tick so a downgrade to a pre-format-13 reader still sees a sane
    // time of day.
    currentSave_->gameTimeSeconds =
        static_cast<double>(gameSession_.serverTick()) / world::DayNightCycle::kTicksPerSecond;
    currentSave_->serverTick = gameSession_.serverTick();
    for (std::size_t index = 0; index < world::kClockCount; ++index) {
        currentSave_->clocks[index] =
            gameSession_.clocks().state(static_cast<world::ClockId>(index));
    }
    // The weather timers and flags ride along like game time; the gradients are
    // recomputed from them on load.
    currentSave_->weather = gameSession_.weatherSystem().state();
    currentSave_->gameMode = gameSession_.gameMode();
    currentSave_->gameRules = gameSession_.gameRules();
    // The /spawnpoint result rides along like the player's own position.
    currentSave_->hasSpawnPoint = gameSession_.hasPlayerSpawn();
    const auto spawnPosition = gameSession_.playerSpawnPosition();
    currentSave_->spawnX = spawnPosition.x;
    currentSave_->spawnY = spawnPosition.y;
    currentSave_->spawnZ = spawnPosition.z;
    currentSave_->spawnYaw = gameSession_.playerSpawnYaw();
    currentSave_->inventory = gameSession_.inventory().slots();
    currentSave_->selectedHotbarSlot = gameSession_.inventory().selectedHotbarSlot();
    currentSave_->playerHealth = gameSession_.vitals().health();
    currentSave_->playerFoodLevel = gameSession_.vitals().foodLevel();
    currentSave_->playerSaturation = gameSession_.vitals().saturation();
    currentSave_->playerAirTicks = gameSession_.vitals().airTicks();
    currentSave_->chests.assign(gameSession_.chestSystem().entities().begin(),
                                gameSession_.chestSystem().entities().end());
    currentSave_->furnaces.assign(gameSession_.furnaceSystem().entities().begin(),
                                  gameSession_.furnaceSystem().entities().end());
    // The live creatures ride along like the chests: a world saved mid-session
    // reopens with its herd where it was. Species are stored by their registered
    // id path and resolved through the registry on load.
    currentSave_->entities.clear();
    currentSave_->entities.reserve(gameSession_.worldEntities().entities().size());
    for (const auto& entity : gameSession_.worldEntities().entities()) {
        if (entity.type == nullptr) {
            continue;
        }
        currentSave_->entities.push_back(toPersistentEntity(entity));
    }
    currentSave_->itemDrops.clear();
    currentSave_->itemDrops.reserve(gameSession_.itemEntities().entities().size());
    for (const auto& drop : gameSession_.itemEntities().entities()) {
        currentSave_->itemDrops.push_back({drop.position.x, drop.position.y, drop.position.z,
                                           drop.velocity.x, drop.velocity.y, drop.velocity.z,
                                           drop.stack, drop.ageTicks});
    }
    currentSave_->fallingBlocks.clear();
    for (const auto& falling : gameSession_.worldSimulation().fallingBlocks()) {
        // A landed entity is already back in the chunk; saving it would
        // duplicate the block on reload.
        if (falling.removed) {
            continue;
        }
        currentSave_->fallingBlocks.push_back({falling.position.x, falling.position.y,
                                              falling.position.z, falling.verticalVelocity,
                                              falling.block});
    }
    // M-3: chunks unloaded but not yet restored have their herd on disk and out
    // of the simulation — the only region records the save must preserve. Every
    // other disk record is replaced by the fresh gather (or dropped if stale).
    std::set<std::pair<int, int>> unloadedChunkCoords;
    for (const auto& position : unloadedChunks_) {
        unloadedChunkCoords.emplace(position.x, position.z);
    }
    saveRepository_.save(*currentSave_, unloadedChunkCoords);
    return true;
}

void GameRuntime::save() {
    const auto saveRead = worldLock_.read();
    static_cast<void>(saveLocked());
}

void GameRuntime::persistUnloadedChunk(world::ChunkPosition position) {
    if (!currentSave_.has_value() || currentSave_->summary.identifier.empty()) {
        return;
    }
    const auto persistStart = std::chrono::steady_clock::now();
    const int chunkX = position.x;
    const int chunkZ = position.z;
    // The chunk's edits, bucketed by the same floor division the region writer
    // uses. They stay in currentSave_->edits as well — a same-session reload
    // regenerates from the streamer's copy, and the next save rewrites them —
    // but the region file becomes their durable home now, not later. Collected
    // through the per-chunk index (O(this chunk's edits)) instead of scanning the
    // whole flat edit list per chunk.
    refreshEditIndex();
    std::vector<world::PersistentBlockEdit> edits;
    if (const auto found = editsByChunk_.find(position); found != editsByChunk_.end()) {
        edits.reserve(found->second.size());
        for (const auto index : found->second) {
            edits.push_back(currentSave_->edits[index]);
        }
    }
    if (diag::traceEnabled()) {
        diag::frameTrace().editScan += edits.size();
    }
    // The creatures inside the chunk leave the simulation and are written to the
    // same region file, so a herd outside the radius survives on disk instead of
    // ticking in a chunk that no longer exists.
    std::vector<persistence::PersistentEntity> entities;
    for (const auto& entity : gameSession_.worldEntities().removeInChunk(chunkX, chunkZ)) {
        if (entity.type == nullptr) {
            continue;
        }
        entities.push_back(toPersistentEntity(entity));
    }
    // Hand the disk write to the background worker instead of doing a
    // synchronous region read-modify-write here on the render thread inside the
    // world write lock. The extraction above (edits + removeInChunk) has to stay
    // on this thread because it reads the save and mutates the simulation's
    // entity store, but the actual file I/O — the 25–150ms spike — moves off the
    // critical path. persistIdentifier_ is the save the queued records belong to;
    // a world switch flushes the queue first so it never mixes two saves.
    persistence::ChunkPersistRecord record;
    record.chunkX = chunkX;
    record.chunkZ = chunkZ;
    record.edits = std::move(edits);
    record.entities = std::move(entities);
    {
        const std::lock_guard<std::mutex> guard{persistMutex_};
        persistIdentifier_ = currentSave_->summary.identifier;
        ++persistPending_[position];
        persistQueue_.push_back(std::move(record));
    }
    persistWakeCv_.notify_one();
    if (diag::traceEnabled()) {
        ++diag::frameTrace().saveChunkCalls;
        diag::frameTrace().persistMs += diag::msSince(persistStart);
    }
    unloadedChunks_.insert(position);
}

void GameRuntime::refreshEditIndex() {
    if (!currentSave_.has_value()) {
        editsByChunk_.clear();
        editsIndexed_ = 0;
        return;
    }
    const auto& edits = currentSave_->edits;
    if (edits.size() < editsIndexed_) {
        // The edit vector shrank — a world switch replaced it. Rebuild.
        editsByChunk_.clear();
        editsIndexed_ = 0;
    }
    for (std::size_t index = editsIndexed_; index < edits.size(); ++index) {
        const auto chunk = world::chunkPositionFromWorld(
            static_cast<float>(edits[index].x), static_cast<float>(edits[index].z));
        editsByChunk_[{chunk.x, chunk.z}].push_back(index);
    }
    editsIndexed_ = edits.size();
}

void GameRuntime::startPersistenceWorker() {
    persistenceThread_ = std::thread{[this] { persistenceWorkerLoop(); }};
}

void GameRuntime::stopPersistenceWorker() {
    if (!persistenceThread_.joinable()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> guard{persistMutex_};
        persistStopping_ = true;
    }
    persistWakeCv_.notify_all();
    persistenceThread_.join();
}

void GameRuntime::persistenceWorkerLoop() {
    std::unique_lock<std::mutex> lock{persistMutex_};
    while (true) {
        persistWakeCv_.wait(lock,
                            [this] { return persistStopping_ || !persistQueue_.empty(); });
        if (persistQueue_.empty()) {
            // Only stop once the backlog is drained, so a shutdown still lands
            // every queued chunk on disk.
            if (persistStopping_) {
                return;
            }
            continue;
        }
        // Drain the whole queue in one go; saveChunks batches region rewrites so
        // a burst of chunks sharing a region file costs one read-modify-write.
        std::vector<world::ChunkPosition> positions;
        positions.reserve(persistQueue_.size());
        for (const auto& record : persistQueue_) {
            positions.push_back({record.chunkX, record.chunkZ});
        }
        std::vector<persistence::ChunkPersistRecord> batch{
            std::make_move_iterator(persistQueue_.begin()),
            std::make_move_iterator(persistQueue_.end())};
        persistQueue_.clear();
        const std::string identifier = persistIdentifier_;
        persistBusy_ = true;
        lock.unlock();
        try {
            saveRepository_.saveChunks(identifier, std::move(batch));
        } catch (const std::exception&) {
            // A failed region write is non-fatal: the data is still in the
            // in-session simulation/save and the next full save rewrites it.
            // Swallow rather than let the worker thread terminate the process.
        }
        lock.lock();
        persistBusy_ = false;
        for (const auto position : positions) {
            const auto found = persistPending_.find(position);
            if (found != persistPending_.end() && --found->second <= 0) {
                persistPending_.erase(found);
            }
        }
        persistDoneCv_.notify_all();
    }
}

void GameRuntime::flushChunkWrites(world::ChunkPosition position) {
    std::unique_lock<std::mutex> lock{persistMutex_};
    persistDoneCv_.wait(
        lock, [this, position] { return persistPending_.find(position) == persistPending_.end(); });
}

void GameRuntime::flushAllChunkWrites() {
    std::unique_lock<std::mutex> lock{persistMutex_};
    persistDoneCv_.wait(lock, [this] { return persistQueue_.empty() && !persistBusy_; });
}

void GameRuntime::restoreLoadedChunk(world::ChunkPosition position) {
    if (!currentSave_.has_value() || currentSave_->summary.identifier.empty()) {
        return;
    }
    // Only chunks this session actually unloaded have their herd on disk; the
    // rest already have their creatures in the simulation (world load restored
    // every region record at once).
    if (unloadedChunks_.erase(position) == 0U) {
        return;
    }
    // The unload write is asynchronous, so this chunk's region file may still be
    // sitting in the persistence queue. Wait for exactly this chunk's writes to
    // land before reading it back, otherwise the herd would restore from a stale
    // (or half-written) file.
    flushChunkWrites(position);
    const auto records =
        saveRepository_.loadChunkEntities(currentSave_->summary.identifier, position.x, position.z);
    for (const auto& record : records) {
        const auto* type = gameplay::entities::entityTypeRegistry().byId(record.species);
        if (type == nullptr) {
            continue;
        }
        gameSession_.worldEntities().restore(
            {record.x, record.y, record.z}, *type, record.yaw,
            {record.vx, record.vy, record.vz}, record.health, record.angerTicks,
            record.ageTicks, record.rngState);
    }
}

void GameRuntime::registerAuthoritativeCommands() {
    // The command tree owns every command. These are the server-authoritative
    // ones — they only touch the session, the world and the save, so a headless
    // dedicated server runs them too. The renderer registers its client-only
    // commands on commandDispatcher() through the shared tree.
    commandDispatcher_.literal("gamemode")
        .argument("mode", gameplay::command::kGameModeArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            const auto mode = context.find<gameplay::GameMode>("mode");
            if (!mode.has_value()) {
                return gameplay::CommandResult{false, "Usage: /gamemode <survival|creative>"};
            }
            gameSession_.setGameMode(*mode);
            return gameplay::CommandResult{
                true, "Set own game mode to " + std::string{gameplay::gameModeName(*mode)}};
        });
    commandDispatcher_.literal("time")
        .then("set")
        .argument("time", gameplay::command::kTimeArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            const auto ticks = context.find<double>("time");
            if (!ticks.has_value()) {
                return gameplay::CommandResult{
                    false, "Usage: /time set <day|noon|night|midnight|ticks>"};
            }
            // Set the sun's clock, not the frame timer; the target is folded into
            // the current day so the calendar does not jump back to day zero.
            const auto perDay = static_cast<std::uint64_t>(world::DayNightCycle::kTicksPerDay);
            const auto target = static_cast<std::uint64_t>(std::llround(*ticks)) % perDay;
            auto& clocks = gameSession_.clocks();
            const std::uint64_t current = clocks.totalTicks(world::ClockId::Overworld);
            clocks.setTotalTicks(world::ClockId::Overworld, current - (current % perDay) + target);
            return gameplay::CommandResult{true, "Set the time to " +
                                                     std::to_string(static_cast<int>(*ticks))};
        });
    commandDispatcher_.literal("give")
        .argument("item", gameplay::command::kGiveItemArgument)
        .argument("count", gameplay::command::kIntArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            const auto itemToken = context.find<std::string>("item");
            const auto count = context.find<std::int64_t>("count");
            if (!itemToken.has_value() || !count.has_value()) {
                return gameplay::CommandResult{false, "Usage: /give <item|index> [count]"};
            }
            const bool numeric = std::all_of(itemToken->begin(), itemToken->end(),
                                             [](char c) { return c >= '0' && c <= '9'; });
            gameplay::ItemStack requested;
            std::string identifier;
            if (numeric) {
                std::size_t index = 0;
                for (const char c : *itemToken) {
                    index = index * 10U + static_cast<std::size_t>(c - '0');
                }
                const auto catalog = gameplay::contentRegistry().allCatalog();
                if (index >= catalog.size()) {
                    return gameplay::CommandResult{
                        false, "Catalog index out of range (0.." +
                                   std::to_string(catalog.size() - 1U) + ")"};
                }
                requested = catalog[index];
                if (gameplay::isBlockStack(requested)) {
                    identifier = world::blockDefinition(requested.block).identifier.toString();
                } else if (requested.item != nullptr) {
                    identifier = requested.item->identifier.toString();
                }
            } else if (const auto block = world::blockFromIdentifier(*itemToken); block.has_value()) {
                requested = {*block, 1U, gameplay::blockItemFor(*block)};
                identifier = world::blockDefinition(*block).identifier.toString();
            } else if (const auto* item = gameplay::itemFromIdentifier(*itemToken);
                       item != nullptr) {
                requested = {world::Block::Air, 1U, item};
                identifier = item->identifier.toString();
            }
            if (*count <= 0) {
                return gameplay::CommandResult{false, "Count must be positive"};
            }
            requested.count = static_cast<std::uint8_t>(std::min<std::int64_t>(*count, 255));
            // Hand over as many whole stacks as needed; overflow spills at the
            // player's feet.
            const auto maximum = gameplay::itemMaximumStackSize(requested);
            std::size_t given = 0;
            while (!requested.empty()) {
                gameplay::ItemStack stack = requested;
                stack.count = std::min(requested.count, maximum);
                const std::uint8_t intended = stack.count;
                if (gameSession_.inventory().add(stack)) {
                    given += intended;
                } else {
                    gameSession_.itemEntities().spawn(gameSession_.player().position(), stack,
                                                      {0.0F, 0.2F, 0.0F});
                }
                requested.count = static_cast<std::uint8_t>(requested.count - intended);
            }
            return gameplay::CommandResult{true,
                                           "Gave " + std::to_string(given) + "x " + identifier};
        });
    // gamerule keeps GameRules as its rule engine; the tree supplies the
    // validated rule name and the raw value string it parses.
    commandDispatcher_.literal("gamerule")
        .argument("rule", gameplay::command::kGameRuleArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            const auto rule = context.find<std::string>("rule");
            if (!rule.has_value()) {
                return gameplay::CommandResult{false, "Usage: /gamerule <rule> [<value>]"};
            }
            return gameSession_.gameRules().query(*rule);
        })
        .argument("value", gameplay::command::kStringArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            const auto rule = context.find<std::string>("rule");
            const auto value = context.find<std::string>("value");
            if (!rule.has_value() || !value.has_value()) {
                return gameplay::CommandResult{false, "Usage: /gamerule <rule> [<value>]"};
            }
            return gameSession_.gameRules().setFromCommand(*rule, *value);
        });
    commandDispatcher_.literal("kill")
        .executes([this](const gameplay::command::CommandContext&) {
            gameSession_.killPlayer(gameplay::kPrimaryPlayerId, host_);
            return gameplay::CommandResult{true, "Killed the player"};
        })
        .argument("target", gameplay::command::kEntityTargetArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            const auto target = context.find<std::string>("target");
            if (!target.has_value()) {
                return gameplay::CommandResult{false, "Usage: /kill [<entity>]"};
            }
            if (*target == "player") {
                gameSession_.killPlayer(gameplay::kPrimaryPlayerId, host_);
                return gameplay::CommandResult{true, "Killed the player"};
            }
            std::size_t killed = 0U;
            for (const auto& entity : gameSession_.worldEntities().entities()) {
                if (entity.type != nullptr && (entity.type->id().matches(*target) ||
                                               entity.type->vanillaId().matches(*target))) {
                    gameSession_.worldEntities().kill(entity.id);
                    ++killed;
                }
            }
            if (killed == 0U) {
                return gameplay::CommandResult{false, "No entities of that species are spawned"};
            }
            return gameplay::CommandResult{true,
                                           "Killed " + std::to_string(killed) + "x " + *target};
        });
    commandDispatcher_.literal("spawnpoint")
        .executes([this](const gameplay::command::CommandContext&) {
            return applySpawnPoint(std::nullopt);
        })
        .argument("pos", gameplay::command::kTeleportDestinationArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            const auto position = context.find<gameplay::command::Position3>("pos");
            if (!position.has_value()) {
                return gameplay::CommandResult{false, "Usage: /spawnpoint [<x> <y> <z>]"};
            }
            const glm::vec3 base = gameSession_.player().position();
            const glm::vec3 target{
                position->relativeX ? base.x + static_cast<float>(position->x)
                                    : static_cast<float>(position->x),
                position->relativeY ? base.y + static_cast<float>(position->y)
                                    : static_cast<float>(position->y),
                position->relativeZ ? base.z + static_cast<float>(position->z)
                                    : static_cast<float>(position->z),
            };
            return applySpawnPoint(target);
        });
    const auto setClearWeather = [this](int ticks) {
        gameSession_.weatherSystem().setWeather(ticks, 0, false, false);
        return gameplay::CommandResult{true, "Cleared the weather"};
    };
    const auto setRainWeather = [this](int ticks) {
        gameSession_.weatherSystem().setWeather(0, ticks, true, false);
        return gameplay::CommandResult{true, "It started raining"};
    };
    const auto setThunderWeather = [this](int ticks) {
        // A thunderstorm is raining and thundering; keeping both timers on the
        // requested duration mirrors WeatherCommand#setWeather.
        gameSession_.weatherSystem().setWeather(0, ticks, true, true);
        return gameplay::CommandResult{true, "It started thundering"};
    };
    commandDispatcher_.literal("weather")
        .then("clear")
        .executes([setClearWeather](const gameplay::command::CommandContext&) {
            return setClearWeather(6000);
        })
        .argument("duration", gameplay::command::kWeatherDurationArgument)
        .executes([setClearWeather](const gameplay::command::CommandContext& context) {
            const auto seconds = context.find<std::int64_t>("duration");
            return seconds.has_value()
                       ? setClearWeather(static_cast<int>(*seconds * 20))
                       : gameplay::CommandResult{false, "Usage: /weather clear [<duration>]"};
        });
    commandDispatcher_.literal("weather")
        .then("rain")
        .executes([setRainWeather](const gameplay::command::CommandContext&) {
            return setRainWeather(6000);
        })
        .argument("duration", gameplay::command::kWeatherDurationArgument)
        .executes([setRainWeather](const gameplay::command::CommandContext& context) {
            const auto seconds = context.find<std::int64_t>("duration");
            return seconds.has_value()
                       ? setRainWeather(static_cast<int>(*seconds * 20))
                       : gameplay::CommandResult{false, "Usage: /weather rain [<duration>]"};
        });
    commandDispatcher_.literal("weather")
        .then("thunder")
        .executes([setThunderWeather](const gameplay::command::CommandContext&) {
            return setThunderWeather(6000);
        })
        .argument("duration", gameplay::command::kWeatherDurationArgument)
        .executes([setThunderWeather](const gameplay::command::CommandContext& context) {
            const auto seconds = context.find<std::int64_t>("duration");
            return seconds.has_value()
                       ? setThunderWeather(static_cast<int>(*seconds * 20))
                       : gameplay::CommandResult{false, "Usage: /weather thunder [<duration>]"};
        });
}

gameplay::CommandResult GameRuntime::applySpawnPoint(const std::optional<glm::vec3>& position) {
    const glm::vec3 spawn = position.value_or(gameSession_.player().position());
    gameSession_.hasPlayerSpawn() = true;
    gameSession_.playerSpawnPosition() = spawn;
    gameSession_.playerSpawnYaw() = 0.0F;
    // The command runs inside the tick's write section, so the unlocked save is
    // correct (the mutex is not recursive).
    static_cast<void>(saveLocked());
    return gameplay::CommandResult{true, "Set the spawn point to " +
                                             std::to_string(static_cast<int>(spawn.x)) + " " +
                                             std::to_string(static_cast<int>(spawn.y)) + " " +
                                             std::to_string(static_cast<int>(spawn.z))};
}

std::size_t GameRuntime::serverResidentBytes() const {
    std::size_t total = 0U;
    for (const auto& position : serverWorld_.positions()) {
        const world::Chunk* chunk = serverWorld_.chunk(position);
        if (chunk == nullptr) {
            continue;
        }
        for (int sectionY = 0; sectionY < world::kSectionCount; ++sectionY) {
            const auto& section = chunk->section(sectionY);
            total += section.stateHeapBytes() + section.lightHeapBytes();
        }
    }
    return total;
}

} // namespace mc::runtime

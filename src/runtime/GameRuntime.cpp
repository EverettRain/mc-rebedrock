#include "runtime/GameRuntime.hpp"

#include "gameplay/ContentRegistry.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/GameplayMutationSink.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "gameplay/command/GameplayArguments.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"
#include "world/ChunkStreamer.hpp"
#include "world/DayNightCycle.hpp"
#include "world/WorldConstants.hpp"

#include "core/FrameTrace.hpp"

#include <algorithm>
#include <cmath>
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
    record.fireTicks = entity.fireTicks;
    // The active MobEffects, by name (never the per-run id). An id with no name —
    // impossible for a registered effect — is skipped rather than stored blank.
    for (std::size_t index = 0; index < entity.effects.count; ++index) {
        const gameplay::EffectInstance& live = entity.effects.entries[index];
        const std::string_view name = gameplay::statusEffectName(live.id);
        if (name.empty()) {
            continue;
        }
        record.effects.push_back(
            {std::string{name}, live.durationTicks, live.amplifier});
    }
    return record;
}

// The inverse: a save's effect list back into the live inline store, resolving
// each name through the registry. A name this build no longer knows is dropped.
[[nodiscard]] gameplay::ActiveEffects toActiveEffects(
    const std::vector<persistence::PersistentEffect>& effects) {
    gameplay::ActiveEffects live;
    for (const auto& record : effects) {
        const core::StatusEffectId id = gameplay::statusEffectByName(record.name);
        static_cast<void>(gameplay::applyEffect(live, id, record.durationTicks, record.amplifier));
    }
    return live;
}
}  // namespace

GameRuntime::GameRuntime(gameplay::SimulationHost& host, world::ChunkStreamer& chunkStreamer,
                         std::filesystem::path saveRoot)
    : host_(host), saveRepository_(std::move(saveRoot)), chunkStreamer_(chunkStreamer) {
    // Bind the event host up front so the world-edit events a mutation publishes
    // reach the host even before the first tick (tick() re-binds it anyway).
    gameSession_.setEventHost(host_);
    // Wire the runtime-owned World into the session's primary Level (DIM-1). The
    // World is a GameRuntime member, fully constructed by now; the session's
    // per-dimension systems reach their blocks through this reference. While the
    // world is single-dimension, the one World backs the Overworld level.
    gameSession_.bindPrimaryWorld(serverWorld_);
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
    drainClientCommands();
    gameSession_.tick(serverWorld_, host_);
    publishSnapshotsToChannel();
    processChatQueue();
}

void GameRuntime::publishSnapshotsToChannel() {
    // The tick's side-effect events go first, in publish order, so the client
    // applies them (world edits to its cache, sounds, particles, container
    // opens/eating) before it takes the new player/world mirror — the same order
    // the renderer's drainEvents used to run, now carried over the channel.
    for (const auto& event : gameSession_.takeEvents()) {
        serverChannel().sendFrame(net::encodeMessage(net::NetMessage{event}));
    }
    // gameSession_.tick just published fresh snapshots; encode the player and
    // world views and send them on the server end. Encoding here (rather than in
    // sendFrame) lets us record the real per-tick serialization size. The client
    // pumps the newest of these into its mirror; older frames left by a slow
    // frame are drained and discarded, so the queue never grows unbounded.
    auto playerFrame = net::encodeMessage(
        net::NetMessage{gameplay::PublishedSnapshot{gameSession_.playerTickSnapshot()}});
    auto worldFrame = net::encodeMessage(
        net::NetMessage{gameplay::PublishedSnapshot{gameSession_.worldSnapshot()}});
    auto entityFrame = net::encodeMessage(net::NetMessage{gameSession_.entitySnapshot()});
    snapshotEncodedBytes_ = playerFrame.size() + worldFrame.size() + entityFrame.size();
    serverChannel().sendFrame(std::move(playerFrame));
    serverChannel().sendFrame(std::move(worldFrame));
    serverChannel().sendFrame(std::move(entityFrame));
}

void GameRuntime::enqueueClientCommand(gameplay::GameCommand command) {
    net::sendMessage(*loopback_.client, net::NetMessage{std::move(command)});
}

void GameRuntime::sendClientMovement(gameplay::MovementInput input) {
    net::sendMessage(*loopback_.client, net::NetMessage{std::move(input)});
}

void GameRuntime::sendClientSessionCommand(gameplay::SessionCommand command) {
    net::sendMessage(*loopback_.client, net::NetMessage{std::move(command)});
}

void GameRuntime::applyClientCommandsNow() {
    const auto write = worldLock_.write();
    drainClientCommands();
    publishSnapshotsToChannel();
}

void GameRuntime::drainClientCommands() {
    std::optional<net::NetMessage> message;
    while (net::receiveMessage(serverChannel(), message)) {
        // A frame that did not decode (an unknown tag from a newer client) is
        // reported as an empty optional but already consumed — skip it and keep
        // draining. The client→server kinds expected on this end: discrete
        // GameCommands (queued for the tick's late interaction drain), the
        // continuous MovementInput (staged on the player now, before the tick
        // reads its input at the top), and SessionCommands (respawn/game mode,
        // applied to the session at once).
        if (!message.has_value()) {
            continue;
        }
        if (std::holds_alternative<gameplay::GameCommand>(*message)) {
            gameSession_.enqueueCommand(std::get<gameplay::GameCommand>(std::move(*message)));
        } else if (std::holds_alternative<gameplay::MovementInput>(*message)) {
            gameSession_.applyMovementInput(std::get<gameplay::MovementInput>(*message));
        } else if (std::holds_alternative<gameplay::SessionCommand>(*message)) {
            applySessionCommand(std::get<gameplay::SessionCommand>(*message));
        }
    }
}

void GameRuntime::applySessionCommand(const gameplay::SessionCommand& command) {
    std::visit(
        [&](const auto& specific) {
            using T = std::decay_t<decltype(specific)>;
            if constexpr (std::is_same_v<T, gameplay::Respawn>) {
                gameSession_.respawn(gameplay::kPrimaryPlayerId);
            } else if constexpr (std::is_same_v<T, gameplay::SetGameMode>) {
                gameSession_.setGameMode(specific.mode);
            }
        },
        command);
}

void GameRuntime::clearChannels() {
    std::vector<std::uint8_t> discard;
    // Drain the effective server channel (attached connection or loopback server)
    // and the integrated client's inbound; a world switch must not let the
    // previous world's queued frames reach the new one.
    while (serverChannel().receiveFrame(discard)) {
    }
    while (loopback_.client->receiveFrame(discard)) {
    }
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
        // Build the source fresh per line: a command may move the player, so a
        // following line's `~` resolves against the updated position. The source's
        // feedback sink writes chatResult_ (gated by sendCommandFeedback), so the
        // result is routed there rather than stored from the return value.
        static_cast<void>(commandDispatcher_.execute(line, makeCommandSource()));
    }
}

std::vector<gameplay::command::SelectorCandidate> GameRuntime::gatherSelectorCandidates() const {
    std::vector<gameplay::command::SelectorCandidate> candidates;
    // Players first, in a deterministic id order (the map's own order is not
    // guaranteed), then the world entities in their vector order — so `@r`/random
    // sort see a stable candidate sequence and stay reproducible.
    std::vector<gameplay::PlayerId> playerIds;
    for (const auto& entry : gameSession_.players()) {
        playerIds.push_back(entry.first);
    }
    std::sort(playerIds.begin(), playerIds.end());
    for (const gameplay::PlayerId id : playerIds) {
        gameplay::command::SelectorCandidate candidate;
        candidate.player = true;
        candidate.playerId = id;
        candidate.position = gameSession_.players().at(id).controller.position();
        candidates.push_back(candidate);
    }
    for (const auto& entity : gameSession_.worldEntities().entities()) {
        if (entity.dead()) {
            continue;
        }
        gameplay::command::SelectorCandidate candidate;
        candidate.entityId = entity.id;
        candidate.position = entity.position;
        candidate.type = entity.type;
        candidates.push_back(candidate);
    }
    return candidates;
}

std::uint64_t GameRuntime::nextCommandRandom() {
    commandRandomState_ = gameplay::command::selectorMix(commandRandomState_);
    return commandRandomState_;
}

gameplay::CommandResult GameRuntime::killSelector(const gameplay::command::EntitySelector& selector,
                                                  const gameplay::command::CommandSource& source) {
    const auto candidates = gatherSelectorCandidates();
    const auto targets = selector.resolve(source, candidates, nextCommandRandom());
    std::size_t killed = 0U;
    for (const auto& target : targets) {
        if (target.player) {
            gameSession_.killPlayer(target.playerId, host_);
        } else {
            gameSession_.worldEntities().kill(target.entityId);
        }
        ++killed;
    }
    if (killed == 0U) {
        return gameplay::CommandResult{false, "No entity was found"};
    }
    return gameplay::CommandResult{true, "Killed " + std::to_string(killed) +
                                             (killed == 1U ? " entity" : " entities")};
}

namespace {
[[nodiscard]] int floorToInt(float value) {
    return static_cast<int>(std::floor(value));
}
// 1.16.1's FillCommand fillLimit: a fill covering more cells than this is
// rejected, so an accidental huge box cannot stall the tick.
constexpr long long kFillLimit = 32768;
} // namespace

bool GameRuntime::commandSetBlock(glm::ivec3 cell, world::BlockState state, bool drop,
                                  gameplay::GameplayMutationSink& sink) {
    // Every command edit goes through the authoritative mutation path (neighbours,
    // light, block entities stay consistent) — never a raw section poke. `drop`
    // chooses destroy (the old block breaks) over a silent replace.
    auto flags = world::MutationFlags::All;
    if (!drop) {
        flags = flags | world::MutationFlags::SuppressDrops;
    }
    return gameSession_.worldMutations()
        .setBlock(serverWorld_, {cell.x, cell.y, cell.z}, state, flags,
                  world::MutationCause::Command, sink)
        .changed;
}

gameplay::CommandResult GameRuntime::runSetblock(const gameplay::command::CommandContext& context,
                                                 std::string_view mode) {
    const auto position = context.find<gameplay::command::Position3>("pos");
    const auto blockName = context.find<std::string>("block");
    if (!position.has_value() || !blockName.has_value()) {
        return usageError("setblock", context.source());
    }
    const auto block = world::blockFromIdentifier(*blockName);
    if (!block.has_value()) {
        return gameplay::CommandResult{false, "Unknown block: " + *blockName};
    }
    const glm::vec3 resolved = gameplay::command::resolve(*position, context.source());
    const glm::ivec3 cell{floorToInt(resolved.x), floorToInt(resolved.y), floorToInt(resolved.z)};
    if (mode == "keep" && serverWorld_.block(cell.x, cell.y, cell.z) != world::Block::Air) {
        return gameplay::CommandResult{false, "Could not set the block (keep: the cell is not air)"};
    }
    gameplay::GameplayMutationSink sink{serverWorld_, gameSession_};
    const world::BlockState state{*block, world::defaultOrientation(*block)};
    if (!commandSetBlock(cell, state, /*drop=*/mode == "destroy", sink)) {
        return gameplay::CommandResult{false, "Could not set the block"};
    }
    return gameplay::CommandResult{true, "Changed the block at " + std::to_string(cell.x) + " " +
                                             std::to_string(cell.y) + " " + std::to_string(cell.z)};
}

gameplay::CommandResult GameRuntime::runFill(const gameplay::command::CommandContext& context,
                                             std::string_view mode) {
    const auto from = context.find<gameplay::command::Position3>("from");
    const auto to = context.find<gameplay::command::Position3>("to");
    const auto blockName = context.find<std::string>("block");
    if (!from.has_value() || !to.has_value() || !blockName.has_value()) {
        return usageError("fill", context.source());
    }
    const auto block = world::blockFromIdentifier(*blockName);
    if (!block.has_value()) {
        return gameplay::CommandResult{false, "Unknown block: " + *blockName};
    }
    const glm::vec3 a = gameplay::command::resolve(*from, context.source());
    const glm::vec3 b = gameplay::command::resolve(*to, context.source());
    const glm::ivec3 lo{std::min(floorToInt(a.x), floorToInt(b.x)),
                        std::min(floorToInt(a.y), floorToInt(b.y)),
                        std::min(floorToInt(a.z), floorToInt(b.z))};
    const glm::ivec3 hi{std::max(floorToInt(a.x), floorToInt(b.x)),
                        std::max(floorToInt(a.y), floorToInt(b.y)),
                        std::max(floorToInt(a.z), floorToInt(b.z))};
    const long long volume = static_cast<long long>(hi.x - lo.x + 1) *
                             static_cast<long long>(hi.y - lo.y + 1) *
                             static_cast<long long>(hi.z - lo.z + 1);
    if (volume > kFillLimit) {
        return gameplay::CommandResult{
            false, "Too many blocks in the specified area (maximum " + std::to_string(kFillLimit) +
                       ")"};
    }
    const world::BlockState state{*block, world::defaultOrientation(*block)};
    const world::BlockState air{world::Block::Air};
    gameplay::GameplayMutationSink sink{serverWorld_, gameSession_};
    std::size_t changed = 0U;
    for (int x = lo.x; x <= hi.x; ++x) {
        for (int y = lo.y; y <= hi.y; ++y) {
            for (int z = lo.z; z <= hi.z; ++z) {
                const bool shell = x == lo.x || x == hi.x || y == lo.y || y == hi.y ||
                                   z == lo.z || z == hi.z;
                if ((mode == "outline" || mode == "hollow") && !shell) {
                    // outline leaves the interior untouched; hollow clears it.
                    if (mode == "hollow" && commandSetBlock({x, y, z}, air, false, sink)) {
                        ++changed;
                    }
                    continue;
                }
                if (mode == "keep" && serverWorld_.block(x, y, z) != world::Block::Air) {
                    continue;
                }
                if (commandSetBlock({x, y, z}, state, /*drop=*/mode == "destroy", sink)) {
                    ++changed;
                }
            }
        }
    }
    return gameplay::CommandResult{true, "Filled " + std::to_string(changed) + " blocks"};
}

gameplay::CommandResult GameRuntime::runSummon(const gameplay::command::CommandContext& context) {
    const auto name = context.find<std::string>("entity");
    if (!name.has_value()) {
        return usageError("summon", context.source());
    }
    const auto* type = gameplay::entities::entityTypeRegistry().byId(*name);
    if (type == nullptr) {
        return gameplay::CommandResult{false, "Unknown entity: " + *name};
    }
    glm::vec3 position = context.source().position;
    if (const auto posArg = context.find<gameplay::command::Position3>("pos"); posArg.has_value()) {
        position = gameplay::command::resolve(*posArg, context.source());
    }
    gameSession_.worldEntities().spawn(position, *type,
                                       static_cast<std::uint32_t>(nextCommandRandom()));
    return gameplay::CommandResult{true, "Summoned " + type->id().toString()};
}

namespace {
// Reads the next whitespace-delimited chunk (a selector like `@e[type=cow]`, a
// coordinate `~`, a keyword). Selectors carry punctuation outside the
// unquoted-string set, so a plain "read to the next space" is what the execute
// clause parser needs, not readString.
[[nodiscard]] std::string readWord(gameplay::command::StringReader& reader) {
    reader.skipWhitespace();
    std::string word;
    while (reader.canRead() &&
           !gameplay::command::StringReader::isWhitespace(reader.peek())) {
        word.push_back(reader.read());
    }
    return word;
}

// Parses one whitespace-delimited selector token. Returns nullopt on a parse
// error (with the message in `error`).
[[nodiscard]] std::optional<gameplay::command::EntitySelector> readSelector(
    gameplay::command::StringReader& reader, std::string& error) {
    const std::string token = readWord(reader);
    gameplay::command::StringReader inner{token};
    const auto parsed = gameplay::command::parseEntitySelector(inner);
    if (!parsed.ok()) {
        error = parsed.error->message;
        return std::nullopt;
    }
    return std::any_cast<gameplay::command::EntitySelector>(parsed.value);
}

// Reads three coordinate words into a Position3 (relative `~` axes allowed).
[[nodiscard]] bool readPosition(gameplay::command::StringReader& reader,
                                gameplay::command::Position3& out) {
    const std::string sx = readWord(reader);
    const std::string sy = readWord(reader);
    const std::string sz = readWord(reader);
    return gameplay::command::parseCoordinate(sx, out.x, out.relativeX) &&
           gameplay::command::parseCoordinate(sy, out.y, out.relativeY) &&
           gameplay::command::parseCoordinate(sz, out.z, out.relativeZ);
}

constexpr int kMaxExecuteDepth = 16;
constexpr double kRadiansToDegrees = 57.295779513082323;
} // namespace

std::string GameRuntime::applyExecuteClause(
    const std::string& clause, gameplay::command::StringReader& reader,
    std::vector<gameplay::command::CommandSource>& contexts,
    std::span<const gameplay::command::SelectorCandidate> candidates) {
    namespace cmd = gameplay::command;
    if (clause == "as" || clause == "at") {
        std::string error;
        const auto selector = readSelector(reader, error);
        if (!selector.has_value()) {
            return "execute " + clause + ": " + error;
        }
        std::vector<cmd::CommandSource> next;
        for (const auto& ctx : contexts) {
            for (const auto& target : selector->resolve(ctx, candidates, nextCommandRandom())) {
                if (clause == "as") {
                    next.push_back(target.player ? ctx.withExecutorPlayer(target.playerId)
                                                 : ctx.withExecutorEntity(target.entityId));
                } else {
                    next.push_back(ctx.withPosition(target.position));
                }
            }
        }
        contexts = std::move(next);
        return "";
    }
    if (clause == "positioned") {
        const std::string word = readWord(reader);
        if (word == "as") {
            std::string error;
            const auto selector = readSelector(reader, error);
            if (!selector.has_value()) {
                return "execute positioned as: " + error;
            }
            std::vector<cmd::CommandSource> next;
            for (const auto& ctx : contexts) {
                for (const auto& target : selector->resolve(ctx, candidates, nextCommandRandom())) {
                    next.push_back(ctx.withPosition(target.position));
                }
            }
            contexts = std::move(next);
            return "";
        }
        // `word` is the first coordinate; read the other two from the reader.
        const std::string sy = readWord(reader);
        const std::string sz = readWord(reader);
        cmd::Position3 position;
        if (!cmd::parseCoordinate(word, position.x, position.relativeX) ||
            !cmd::parseCoordinate(sy, position.y, position.relativeY) ||
            !cmd::parseCoordinate(sz, position.z, position.relativeZ)) {
            return "execute positioned: expected <x> <y> <z>";
        }
        for (auto& ctx : contexts) {
            ctx = ctx.withPosition(cmd::resolve(position, ctx));
        }
        return "";
    }
    if (clause == "rotated") {
        const std::string yaw = readWord(reader);
        if (yaw == "as") {
            return "execute rotated as is not supported yet";
        }
        const std::string pitch = readWord(reader);
        cmd::Rotation2 rotation;
        bool relative = false;
        if (!cmd::parseCoordinate(yaw, rotation.yaw, relative) ||
            !cmd::parseCoordinate(pitch, rotation.pitch, relative)) {
            return "execute rotated: expected <yaw> <pitch>";
        }
        for (auto& ctx : contexts) {
            ctx = ctx.withRotation(rotation);
        }
        return "";
    }
    if (clause == "facing") {
        const std::string word = readWord(reader);
        if (word == "entity") {
            return "execute facing entity is not supported yet";
        }
        const std::string sy = readWord(reader);
        const std::string sz = readWord(reader);
        cmd::Position3 position;
        if (!cmd::parseCoordinate(word, position.x, position.relativeX) ||
            !cmd::parseCoordinate(sy, position.y, position.relativeY) ||
            !cmd::parseCoordinate(sz, position.z, position.relativeZ)) {
            return "execute facing: expected <x> <y> <z>";
        }
        for (auto& ctx : contexts) {
            const glm::vec3 target = cmd::resolve(position, ctx);
            const glm::vec3 direction = target - ctx.position;
            cmd::Rotation2 rotation = ctx.rotation;
            if (const float length = glm::length(direction); length > 1e-4F) {
                const glm::vec3 unit = direction / length;
                rotation.yaw = std::atan2(static_cast<double>(unit.x),
                                          static_cast<double>(unit.z)) *
                               kRadiansToDegrees;
                rotation.pitch = std::asin(std::clamp(static_cast<double>(unit.y), -1.0, 1.0)) *
                                 kRadiansToDegrees;
            }
            ctx = ctx.withRotation(rotation);
        }
        return "";
    }
    if (clause == "in") {
        const std::string dimension = readWord(reader);
        if (dimension != "overworld" && dimension != "minecraft:overworld" &&
            dimension != "rebedrock:overworld") {
            return "Unknown dimension: " + dimension;
        }
        for (auto& ctx : contexts) {
            ctx = ctx.withDimension(cmd::Dimension::Overworld);
        }
        return "";
    }
    if (clause == "if" || clause == "unless") {
        const bool negate = clause == "unless";
        const std::string condition = readWord(reader);
        if (condition == "entity") {
            std::string error;
            const auto selector = readSelector(reader, error);
            if (!selector.has_value()) {
                return "execute " + clause + " entity: " + error;
            }
            std::vector<cmd::CommandSource> next;
            for (const auto& ctx : contexts) {
                const bool matches =
                    !selector->resolve(ctx, candidates, nextCommandRandom()).empty();
                if (matches != negate) {
                    next.push_back(ctx);
                }
            }
            contexts = std::move(next);
            return "";
        }
        if (condition == "block") {
            cmd::Position3 position;
            if (!readPosition(reader, position)) {
                return "execute " + clause + " block: expected <x> <y> <z> <block>";
            }
            const std::string blockName = readWord(reader);
            const auto block = world::blockFromIdentifier(blockName);
            if (!block.has_value()) {
                return "Unknown block: " + blockName;
            }
            std::vector<cmd::CommandSource> next;
            for (const auto& ctx : contexts) {
                const glm::vec3 resolved = cmd::resolve(position, ctx);
                const bool matches = serverWorld_.block(floorToInt(resolved.x),
                                                        floorToInt(resolved.y),
                                                        floorToInt(resolved.z)) == *block;
                if (matches != negate) {
                    next.push_back(ctx);
                }
            }
            contexts = std::move(next);
            return "";
        }
        return "execute " + clause + ": expected 'entity' or 'block'";
    }
    return "Unknown execute subcommand: " + clause;
}

gameplay::CommandResult GameRuntime::runExecute(const gameplay::command::CommandContext& context) {
    namespace cmd = gameplay::command;
    const auto chain = context.find<std::string>("chain");
    if (!chain.has_value() || chain->empty()) {
        return usageError("execute", context.source());
    }
    if (commandRecursionDepth_ >= kMaxExecuteDepth) {
        return gameplay::CommandResult{false, "Command recursion limit reached"};
    }
    ++commandRecursionDepth_;
    struct DepthGuard final {
        int& depth;
        ~DepthGuard() { --depth; }
    } guard{commandRecursionDepth_};

    const auto candidates = gatherSelectorCandidates();
    // The starting context is the original source with feedback stripped: the
    // inner `run` results are aggregated by execute, not each broadcast to chat.
    std::vector<cmd::CommandSource> contexts;
    {
        cmd::CommandSource base = context.source();
        base.feedback = nullptr;
        contexts.push_back(base);
    }

    cmd::StringReader reader{*chain};
    while (true) {
        const std::string clause = readWord(reader);
        if (clause.empty()) {
            return gameplay::CommandResult{false, "execute needs a 'run <command>' at the end"};
        }
        if (clause == "run") {
            reader.skipWhitespace();
            const std::string rest{reader.remaining()};
            if (rest.empty()) {
                return gameplay::CommandResult{false, "execute run needs a command"};
            }
            const std::string line = "/" + rest;
            std::size_t ran = 0U;
            for (const auto& ctx : contexts) {
                if (commandDispatcher_.execute(line, ctx).success) {
                    ++ran;
                }
            }
            if (ran == 0U) {
                return gameplay::CommandResult{false, "Executed no commands"};
            }
            return gameplay::CommandResult{true, "Executed " + std::to_string(ran) +
                                                     (ran == 1U ? " command" : " commands")};
        }
        const std::string error = applyExecuteClause(clause, reader, contexts, candidates);
        if (!error.empty()) {
            return gameplay::CommandResult{false, error};
        }
    }
}

gameplay::command::CommandSource GameRuntime::makeCommandSource() {
    gameplay::command::CommandSource source;
    source.playerId = gameplay::kPrimaryPlayerId;
    source.position = gameSession_.player().position();
    // Derive yaw/pitch from where the sender faces (Rotation2: yaw 0 faces +Z,
    // positive pitch up), so a rotation-reading command and CMD5's `^`/`facing`
    // see a real orientation rather than a placeholder.
    const glm::vec3 look = gameSession_.primaryPlayer().playerInput.lookDirection;
    constexpr double kRadiansToDegrees = 57.295779513082323;  // 180 / pi
    source.rotation.yaw =
        std::atan2(static_cast<double>(look.x), static_cast<double>(look.z)) * kRadiansToDegrees;
    source.rotation.pitch =
        std::asin(std::clamp(static_cast<double>(look.y), -1.0, 1.0)) * kRadiansToDegrees;
    source.dimension = gameplay::command::Dimension::Overworld;
    // The single-player host owns the world (op4): every command passes. The op
    // level exists so a multiplayer source below it is refused, not to gate the
    // host.
    source.permissionLevel = gameplay::command::PermissionLevel::Owners;
    // Feedback goes to the chat HUD. A successful command is silent when
    // sendCommandFeedback is off; a failure always reports — vanilla's
    // CommandSourceStack sendSuccess/sendFailure split.
    source.feedback = [this](const gameplay::CommandResult& result) {
        if (result.success &&
            !gameSession_.gameRules().get<bool>(gameplay::GameRuleId::SendCommandFeedback)) {
            return;
        }
        const std::lock_guard<std::mutex> guard{chatMutex_};
        chatResult_ = result;
    };
    return source;
}

void GameRuntime::loadWorld(persistence::SaveGame save, int viewDistanceChunks) {
    // Drain any writes still queued for a previous world before switching saves,
    // so the worker never writes an outgoing world's records under the new
    // identifier.
    flushAllChunkWrites();
    // Drop any intents still in the loopback channel so the previous world's
    // queued commands never reach the incoming one.
    clearChannels();
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
    gameSession_.trappedChestSystem().restore(currentSave_->trappedChests);
    gameSession_.furnaceSystem().restore(currentSave_->furnaces);
    // Restore the herd a saved world carried, resolving species by their
    // registered id. A species this build no longer knows resolves to an
    // UnknownEntity placeholder (not dropped), so a removed datapack/mod's
    // creature round-trips by name instead of vanishing from the world.
    for (const auto& record : currentSave_->entities) {
        const auto& type = gameplay::entities::resolveEntityTypeForRestore(record.species);
        gameSession_.worldEntities().restore({record.x, record.y, record.z}, type, record.yaw,
                                             {record.vx, record.vy, record.vz}, record.health,
                                             record.angerTicks, record.ageTicks, record.rngState,
                                             record.fireTicks, toActiveEffects(record.effects));
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
    // `@r` selectors draw from a world-seeded deterministic stream (never the
    // wall clock), so a fixed world plus a fixed command sequence always picks the
    // same target — the confidence the determinism rule wants.
    commandRandomState_ = currentSave_->summary.seed ^ 0x2545F4914F6CDD1DULL;
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
    // Seed the channel too (C-1b-2), so the client mirror's first pump — before
    // the simulation thread runs a tick — already holds the restored player/world
    // instead of the default state.
    publishSnapshotsToChannel();
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
    currentSave_->trappedChests.assign(gameSession_.trappedChestSystem().entities().begin(),
                                       gameSession_.trappedChestSystem().entities().end());
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
        const auto& type = gameplay::entities::resolveEntityTypeForRestore(record.species);
        gameSession_.worldEntities().restore(
            {record.x, record.y, record.z}, type, record.yaw,
            {record.vx, record.vy, record.vz}, record.health, record.angerTicks,
            record.ageTicks, record.rngState, record.fireTicks,
            toActiveEffects(record.effects));
    }
}

void GameRuntime::registerAuthoritativeCommands() {
    // The command tree owns every command. These are the server-authoritative
    // ones — they only touch the session, the world and the save, so a headless
    // dedicated server runs them too. The renderer registers its client-only
    // commands on commandDispatcher() through the shared tree.
    //
    // Every command below changes the world or a player and so declares op level
    // 2 (GAMEMASTERS), 1.16.1's requirement for gamemode/gamerule/kill/give/time/
    // weather/spawnpoint. Single-player runs as the owner (op4) and always
    // passes; the level exists so a multiplayer (S subtree) source below it is
    // refused server-side without any command touching the check.
    using gameplay::command::PermissionLevel;
    commandDispatcher_.literal("gamemode")
        .requiresLevel(PermissionLevel::GameMasters)
        .argument("mode", gameplay::command::kGameModeArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            const auto mode = context.find<gameplay::GameMode>("mode");
            if (!mode.has_value()) {
                return usageError("gamemode", context.source());
            }
            gameSession_.setGameMode(*mode);
            return gameplay::CommandResult{
                true, "Set own game mode to " + std::string{gameplay::gameModeName(*mode)}};
        });
    commandDispatcher_.literal("time")
        .requiresLevel(PermissionLevel::GameMasters)
        .then("set")
        .argument("time", gameplay::command::kTimeArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            const auto ticks = context.find<double>("time");
            if (!ticks.has_value()) {
                return usageError("time", context.source());
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
        .requiresLevel(PermissionLevel::GameMasters)
        .argument("item", gameplay::command::kGiveItemArgument)
        .argument("count", gameplay::command::kIntArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            const auto itemToken = context.find<std::string>("item");
            const auto count = context.find<std::int64_t>("count");
            if (!itemToken.has_value() || !count.has_value()) {
                return usageError("give", context.source());
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
        .requiresLevel(PermissionLevel::GameMasters)
        .argument("rule", gameplay::command::kGameRuleArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            const auto rule = context.find<std::string>("rule");
            if (!rule.has_value()) {
                return usageError("gamerule", context.source());
            }
            return gameSession_.gameRules().query(*rule);
        })
        .argument("value", gameplay::command::kStringArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            const auto rule = context.find<std::string>("rule");
            const auto value = context.find<std::string>("value");
            if (!rule.has_value() || !value.has_value()) {
                return usageError("gamerule", context.source());
            }
            return gameSession_.gameRules().setFromCommand(*rule, *value);
        });
    // `/kill` with no target kills the executor (@s); `/kill <selector>` resolves
    // a real target selector (@e[type=…], @a, @r, …) and kills each match.
    commandDispatcher_.literal("kill")
        .requiresLevel(PermissionLevel::GameMasters)
        .executes([this](const gameplay::command::CommandContext& context) {
            gameplay::command::EntitySelector self;
            self.variable = gameplay::command::SelectorVariable::Self;
            return killSelector(self, context.source());
        })
        .argument("targets", gameplay::command::kEntitySelectorArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            const auto selector = context.find<gameplay::command::EntitySelector>("targets");
            if (!selector.has_value()) {
                return usageError("kill", context.source());
            }
            return killSelector(*selector, context.source());
        });
    commandDispatcher_.literal("spawnpoint")
        .requiresLevel(PermissionLevel::GameMasters)
        .executes([this](const gameplay::command::CommandContext&) {
            return applySpawnPoint(std::nullopt);
        })
        .argument("pos", gameplay::command::kTeleportDestinationArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            const auto position = context.find<gameplay::command::Position3>("pos");
            if (!position.has_value()) {
                return usageError("spawnpoint", context.source());
            }
            // Relative `~` axes resolve against the source's position in the one
            // shared resolve() — no hand-written base+offset branch here anymore.
            return applySpawnPoint(gameplay::command::resolve(*position, context.source()));
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
        .requiresLevel(PermissionLevel::GameMasters)
        .then("clear")
        .executes([setClearWeather](const gameplay::command::CommandContext&) {
            return setClearWeather(6000);
        })
        .argument("duration", gameplay::command::kWeatherDurationArgument)
        .executes([this, setClearWeather](const gameplay::command::CommandContext& context) {
            const auto seconds = context.find<std::int64_t>("duration");
            return seconds.has_value() ? setClearWeather(static_cast<int>(*seconds * 20))
                                       : usageError("weather", context.source());
        });
    commandDispatcher_.literal("weather")
        .requiresLevel(PermissionLevel::GameMasters)
        .then("rain")
        .executes([setRainWeather](const gameplay::command::CommandContext&) {
            return setRainWeather(6000);
        })
        .argument("duration", gameplay::command::kWeatherDurationArgument)
        .executes([this, setRainWeather](const gameplay::command::CommandContext& context) {
            const auto seconds = context.find<std::int64_t>("duration");
            return seconds.has_value() ? setRainWeather(static_cast<int>(*seconds * 20))
                                       : usageError("weather", context.source());
        });
    commandDispatcher_.literal("weather")
        .requiresLevel(PermissionLevel::GameMasters)
        .then("thunder")
        .executes([setThunderWeather](const gameplay::command::CommandContext&) {
            return setThunderWeather(6000);
        })
        .argument("duration", gameplay::command::kWeatherDurationArgument)
        .executes([this, setThunderWeather](const gameplay::command::CommandContext& context) {
            const auto seconds = context.find<std::int64_t>("duration");
            return seconds.has_value() ? setThunderWeather(static_cast<int>(*seconds * 20))
                                       : usageError("weather", context.source());
        });

    // ---- CMD-4 content commands: thin wiring to existing systems -------------
    // setblock <pos> <block> [replace|keep|destroy]. The mode variants share one
    // base path (idempotent registration reuses its nodes); the default is
    // replace. Writes go through the authoritative mutation path (below).
    const auto setblockBase = [this]() {
        return commandDispatcher_.literal("setblock")
            .requiresLevel(PermissionLevel::GameMasters)
            .argument("pos", gameplay::command::kTeleportDestinationArgument)
            .argument("block", gameplay::command::kBlockArgument);
    };
    setblockBase().executes([this](const gameplay::command::CommandContext& context) {
        return runSetblock(context, "replace");
    });
    for (const char* mode : {"replace", "keep", "destroy"}) {
        const std::string modeName = mode;
        setblockBase().then(mode).executes(
            [this, modeName](const gameplay::command::CommandContext& context) {
                return runSetblock(context, modeName);
            });
    }

    // fill <from> <to> <block> [replace|keep|destroy|outline|hollow].
    const auto fillBase = [this]() {
        return commandDispatcher_.literal("fill")
            .requiresLevel(PermissionLevel::GameMasters)
            .argument("from", gameplay::command::kTeleportDestinationArgument)
            .argument("to", gameplay::command::kTeleportDestinationArgument)
            .argument("block", gameplay::command::kBlockArgument);
    };
    fillBase().executes([this](const gameplay::command::CommandContext& context) {
        return runFill(context, "replace");
    });
    for (const char* mode : {"replace", "keep", "destroy", "outline", "hollow"}) {
        const std::string modeName = mode;
        fillBase().then(mode).executes(
            [this, modeName](const gameplay::command::CommandContext& context) {
                return runFill(context, modeName);
            });
    }

    // summon <entity> [<x> <y> <z>].
    commandDispatcher_.literal("summon")
        .requiresLevel(PermissionLevel::GameMasters)
        .argument("entity", gameplay::command::kSummonEntityArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            return runSummon(context);
        })
        .argument("pos", gameplay::command::kTeleportDestinationArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            return runSummon(context);
        });

    // difficulty [<peaceful|easy|normal|hard>] — query with no argument, set with one.
    commandDispatcher_.literal("difficulty")
        .requiresLevel(PermissionLevel::GameMasters)
        .executes([this](const gameplay::command::CommandContext&) {
            return gameplay::CommandResult{
                true, "The difficulty is " +
                          std::string{gameplay::difficultyName(gameSession_.difficulty())}};
        })
        .argument("level", gameplay::command::kDifficultyArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            const auto level = context.find<gameplay::Difficulty>("level");
            if (!level.has_value()) {
                return usageError("difficulty", context.source());
            }
            gameSession_.setDifficulty(*level);
            return gameplay::CommandResult{
                true, "Set the difficulty to " + std::string{gameplay::difficultyName(*level)}};
        });

    // seed — report the world seed.
    commandDispatcher_.literal("seed")
        .requiresLevel(PermissionLevel::GameMasters)
        .executes([this](const gameplay::command::CommandContext&) {
            const std::uint64_t seed =
                currentSave_.has_value() ? currentSave_->summary.seed : 0U;
            return gameplay::CommandResult{true, "Seed: " + std::to_string(seed)};
        });

    // clear — empty the executor's inventory (the whole-inventory form).
    commandDispatcher_.literal("clear")
        .requiresLevel(PermissionLevel::GameMasters)
        .executes([this](const gameplay::command::CommandContext&) {
            auto& inventory = gameSession_.inventory();
            std::size_t cleared = 0U;
            for (const auto& stack : inventory.slots()) {
                cleared += stack.count;
            }
            inventory.restore({}, inventory.selectedHotbarSlot());
            return gameplay::CommandResult{true, "Removed " + std::to_string(cleared) + " items"};
        });

    // execute <subcommand...> run <command> (CMD5). The whole tail is a greedy
    // string parsed by runExecute into a clause chain: each clause transforms the
    // command source (a POD copy — as/at/positioned/rotated/facing/in), or gates
    // it (if/unless), and `run` re-enters the dispatcher on the transformed
    // source. A single command with a greedy tail (rather than a redirecting
    // subtree) keeps the token-walk dispatcher unchanged.
    commandDispatcher_.literal("execute")
        .requiresLevel(PermissionLevel::GameMasters)
        .argument("chain", gameplay::command::kGreedyStringArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            return runExecute(context);
        });

    // help (CMD6): consumes the dispatcher's introspection API. No requiresLevel,
    // so it is level 0 like vanilla — but it only lists commands the source may
    // actually use (usage() returns empty for the rest). `/help` lists every
    // available command with its smart-usage; `/help <command>` shows one.
    commandDispatcher_.literal("help")
        .executes([this](const gameplay::command::CommandContext& context) {
            std::string listing;
            commandDispatcher_.forEachRootCommand([&](std::string_view name) {
                const std::string smart = commandDispatcher_.usage(name, context.source());
                if (!smart.empty()) {
                    listing += "/" + smart + "\n";
                }
            });
            if (!listing.empty()) {
                listing.pop_back(); // drop the trailing newline
            }
            return gameplay::CommandResult{true, listing};
        })
        .argument("command", gameplay::command::kStringArgument)
        .executes([this](const gameplay::command::CommandContext& context) {
            const auto name = context.find<std::string>("command");
            if (!name.has_value()) {
                return usageError("help", context.source());
            }
            const std::string smart = commandDispatcher_.usage(*name, context.source());
            if (smart.empty()) {
                return gameplay::CommandResult{false,
                                               "No help available for command: " + *name};
            }
            return gameplay::CommandResult{true, "/" + smart};
        });
}

gameplay::CommandResult GameRuntime::usageError(std::string_view command,
                                                const gameplay::command::CommandSource& source) {
    // The single source of a command's usage: generated from the node tree, so a
    // signature change moves the usage with it (no hand-written string drifts).
    return gameplay::CommandResult{false, "Usage: /" + commandDispatcher_.usage(command, source)};
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

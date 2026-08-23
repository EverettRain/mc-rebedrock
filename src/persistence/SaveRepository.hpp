#pragma once

#include "gameplay/GameMode.hpp"
#include "gameplay/ChestSystem.hpp"
#include "gameplay/FurnaceSystem.hpp"
#include "gameplay/GameRules.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/PlayerVitals.hpp"
#include "gameplay/WeatherSystem.hpp"
#include "world/Dimension.hpp"
#include "world/PersistentBlockEdit.hpp"
#include "world/WorldClock.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mc::persistence {

struct SaveSummary final {
    std::string identifier;
    std::string displayName;
    std::uint64_t seed = 0U;
    std::int64_t lastPlayedUnixSeconds = 0;
};

// A save's self-description: which build wrote it (META-1, the equivalent of
// 1.16.1's level.dat `Version { Id, Name, Snapshot }` compound plus the top-level
// `DataVersion`). It records the VersionManifest snapshot taken at *write* time,
// so a world always reports the version that produced it — not the one reading it
// — which is exactly what an upgrade or a JC import needs to reason about the
// source. `worldVersion` is the save format number, the same value the file's
// top-level format field carries (one source, not a second fact). A save written
// before this block existed reconstructs a minimal header from that format number
// on load (`name` empty, `derived` set), so an old world still self-describes as
// well as it can without breaking.
struct SaveVersionHeader final {
    std::uint32_t worldVersion = 0U;   // = the file's save format number
    std::string versionName;           // e.g. "26.1"; empty when reconstructed
    std::uint32_t protocolVersion = 0U;
    std::string buildRef;              // git short hash, or "unknown"
    std::string buildTime;             // ISO 8601, or "unknown"
    bool stable = false;
    // True when this header was reconstructed from the format number of a save
    // that predates the self-describing version block (so the name is unknown).
    bool derived = false;
};

// One active MobEffect persisted with its carrier. The effect is stored by its
// registry *name*, never its runtime id — a dense id is a per-run value that a
// save must not bake in (an added/removed effect would renumber every record).
// A name this build no longer knows is dropped cleanly on load.
struct PersistentEffect final {
    std::string name;              // the registry path, e.g. "poison"
    std::int32_t durationTicks = 0;
    std::uint8_t amplifier = 0U;
};

// One live creature persisted with the world (format 12's ENTITY block): the
// species by its registered id path, the pose/physics the renderer interpolates
// between, and the fields a fresh spawn would not reproduce. Only creatures
// inside the loaded region are saved; the simulation never touches anything
// beyond the simulation radius anyway.
struct PersistentEntity final {
    std::string species;   // e.g. "pig" — resolved through the entity registry
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float yaw = 0.0F;
    float vx = 0.0F;
    float vy = 0.0F;
    float vz = 0.0F;
    float health = 0.0F;
    std::int32_t angerTicks = 0;
    std::uint32_t ageTicks = 0U;
    std::uint32_t rngState = 0U;
    // Entity#fireTicks: a creature saved mid-burn reopens still ablaze. Added in
    // entity block version 2; a version-1 record reads it as zero (not on fire).
    std::int32_t fireTicks = 0;
    // The creature's active MobEffects. Added in entity block version 3; earlier
    // records carry none, so an old world migrates to an unaffected herd.
    std::vector<PersistentEffect> effects;
    // AgeableMob age/love (EM-3). Added in entity block version 4; earlier records
    // read both as zero, i.e. an ordinary adult with no cooldown and no love.
    std::int32_t age = 0;
    std::int32_t loveTicks = 0;
};

// A dropped item awaiting pickup. Position and velocity are the whole physical
// state; `ageTicks` matters because it drives both the despawn timer and the
// pickup delay.
struct PersistentItemDrop final {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float vx = 0.0F;
    float vy = 0.0F;
    float vz = 0.0F;
    gameplay::ItemStack stack;
    std::uint32_t ageTicks = 0U;
};

// A block partway through falling. It exists in neither the chunk nor the drop
// list while airborne, so without this a save taken mid-collapse loses it.
struct PersistentFallingBlock final {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float verticalVelocity = 0.0F;
    world::Block block = world::Block::Sand;
};

struct SaveGame final {
    SaveSummary summary;
    // Which build wrote this world (META-1). On save() this is filled from the
    // current VersionManifest and written to the VERS block; on load() it holds
    // what the save reported (or a header reconstructed from the format number
    // for a pre-VERS world). A freshly created SaveGame carries a zeroed header
    // until save() stamps it.
    SaveVersionHeader versionHeader;
    bool hasPlayerPosition = false;
    float playerX = 0.0F;
    float playerY = 0.0F;
    float playerZ = 0.0F;
    // The player's personal spawn point, set by /spawnpoint the way 1.16.1 keeps
    // SpawnX/Y/Z on the player. Death respawns here before falling back to the
    // world spawn. Format 10 serialises it into its own self-describing block.
    bool hasSpawnPoint = false;
    float spawnX = 0.0F;
    float spawnY = 0.0F;
    float spawnZ = 0.0F;
    float spawnYaw = 0.0F;
    double gameTimeSeconds = 0.0;
    gameplay::GameMode gameMode = gameplay::GameMode::Creative;
    gameplay::Difficulty difficulty = gameplay::Difficulty::Normal;
    // Game rules travel with the world the way 1.16.1 keeps them in level.dat;
    // format 9 serialises them into a sparse, self-describing block.
    gameplay::GameRules gameRules;
    std::size_t selectedHotbarSlot = 0U;
    float playerHealth = gameplay::PlayerVitals::kMaximumHealth;
    std::int32_t playerFoodLevel = gameplay::PlayerVitals::kMaximumFood;
    float playerSaturation = 5.0F;
    std::int32_t playerAirTicks = gameplay::PlayerVitals::kMaximumAirTicks;
    std::array<gameplay::ItemStack, gameplay::Inventory::kSlotCount> inventory{};
    std::vector<world::PersistentBlockEdit> edits;
    std::vector<gameplay::ChestBlockEntity> chests;
    // The trapped chest block entities at save time (BE3). Same ChestBlockEntity
    // shape as `chests`, its own self-describing section (TCST). An older world
    // has none and loads with an empty list, exactly as furnaces did before their
    // section existed.
    std::vector<gameplay::ChestBlockEntity> trappedChests;
    // The furnace block entities at save time — their three slots and burn/cook
    // counters — serialised into their own self-describing block by format 15.
    // Before that, furnaces were a single global inventory that no save carried,
    // so an older world simply loads with no furnaces and back-fills one the
    // first time each furnace block is opened.
    std::vector<gameplay::FurnaceBlockEntity> furnaces;
    // The weather timers and flags, the way 1.16.1 keeps them in level.dat;
    // format 11 serialises them into their own self-describing block. A fresh
    // world defaults to a clear spell.
    gameplay::WeatherState weather;
    // The live creatures at save time, serialised into their own self-describing
    // block by format 12 so a world reopens with its herd.
    std::vector<PersistentEntity> entities;
    // Dropped items and blocks mid-fall. Format 16 gives them their own block:
    // before it, everything a player had thrown or mined but not yet picked up
    // simply vanished on reload, and a sand column caught mid-collapse came back
    // as a hole with its blocks nowhere.
    std::vector<PersistentItemDrop> itemDrops;
    std::vector<PersistentFallingBlock> fallingBlocks;
    // The world's own tick count and every named clock, split apart by format 13
    // so the sun can be frozen without freezing gameplay timing. Loading an
    // older save backfills both from gameTimeSeconds above, which used to carry
    // all of it at once.
    std::uint64_t serverTick = 0U;
    std::array<world::ClockState, world::kClockCount> clocks{};
};

// One chunk's persistable payload, used by the batched unload writer. The
// background persistence worker packs a burst of unloaded chunks into a vector
// of these and hands it to saveChunks(), which groups them by region file so a
// region touched by many chunks in the same burst is read-modified-written once
// instead of once per chunk.
struct ChunkPersistRecord final {
    std::int32_t chunkX = 0;
    std::int32_t chunkZ = 0;
    std::vector<world::PersistentBlockEdit> edits;
    std::vector<PersistentEntity> entities;
};

class SaveRepository final {
  public:
    explicit SaveRepository(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }
    [[nodiscard]] std::vector<SaveSummary> list() const;
    [[nodiscard]] SaveGame create(std::string displayName, std::uint64_t seed) const;
    [[nodiscard]] SaveGame load(const std::string& identifier) const;
    // `unloadedChunks` (chunk coordinates) names the chunks the unload path
    // persisted this session and has not yet restored: their creatures are on
    // disk and out of the simulation, so the save-time region merge preserves
    // their records. Every other disk record is a mirror the fresh gather
    // replaces, or a stale copy of a creature that moved or despawned.
    void save(SaveGame game,
              const std::set<std::pair<std::int32_t, std::int32_t>>& unloadedChunks = {}) const;
    // M-3 (C5): one chunk's own edits and creatures, written to (and read back
    // from) the chunk's region file. The unload path calls saveChunk when a
    // chunk leaves the simulation radius so its data persists promptly; the load
    // path calls loadChunkEntities when the chunk streams back in. saveChunk
    // merges with whatever the region already holds; empty data removes the
    // chunk's record (and the file when the region empties).
    // `dimension` selects the per-dimension region subdirectory (DIM-4): the
    // Overworld writes to `<world>/region/` (unchanged, so old flat worlds are
    // byte-compatible), the Nether to `<world>/DIM-1/region/` and the End to
    // `<world>/DIM1/region/` — the vanilla 1.16.1 layout JC3 import targets, so
    // the compat layer adds no new deviation. Defaulted to Overworld so every
    // existing single-dimension caller keeps its behaviour.
    void saveChunk(const std::string& identifier, int chunkX, int chunkZ,
                   std::vector<world::PersistentBlockEdit> edits,
                   std::vector<PersistentEntity> entities,
                   world::DimensionId dimension = world::DimensionId::Overworld) const;
    // Batched form of saveChunk: groups the records by region file and does one
    // read-modify-write per region for the whole burst. Used off the render
    // thread by the background persistence worker so a chunk-unload storm no
    // longer pays one synchronous region rewrite per chunk on the critical path.
    void saveChunks(const std::string& identifier,
                    std::vector<ChunkPersistRecord> records,
                    world::DimensionId dimension = world::DimensionId::Overworld) const;
    [[nodiscard]] std::vector<PersistentEntity> loadChunkEntities(
        const std::string& identifier, int chunkX, int chunkZ,
        world::DimensionId dimension = world::DimensionId::Overworld) const;

    // The region directory for one dimension of a world, mirroring vanilla's
    // 1.16.1 layout (Overworld at the world root, the Nether under DIM-1, the End
    // under DIM1). Exposed so tests and the JC compat layer can cross-check the
    // on-disk path against what a vanilla import expects.
    [[nodiscard]] std::filesystem::path dimensionRegionDirectory(
        const std::string& identifier, world::DimensionId dimension) const;

    // Rename keeps the folder/identifier stable and rewrites only the display
    // name stored in level.properties, so an edited world keeps its identity.
    void rename(const std::string& identifier, std::string displayName) const;
    // Permanently removes the world directory and everything inside it.
    void remove(const std::string& identifier) const;

    [[nodiscard]] static std::string sanitizeDisplayName(std::string name);

  private:
    std::filesystem::path root_;
};

} // namespace mc::persistence

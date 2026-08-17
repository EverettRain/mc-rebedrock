#pragma once

#include "gameplay/GameMode.hpp"
#include "gameplay/ChestSystem.hpp"
#include "gameplay/FurnaceSystem.hpp"
#include "gameplay/GameRules.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/PlayerVitals.hpp"
#include "gameplay/WeatherSystem.hpp"
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
    void saveChunk(const std::string& identifier, int chunkX, int chunkZ,
                   std::vector<world::PersistentBlockEdit> edits,
                   std::vector<PersistentEntity> entities) const;
    // Batched form of saveChunk: groups the records by region file and does one
    // read-modify-write per region for the whole burst. Used off the render
    // thread by the background persistence worker so a chunk-unload storm no
    // longer pays one synchronous region rewrite per chunk on the critical path.
    void saveChunks(const std::string& identifier,
                    std::vector<ChunkPersistRecord> records) const;
    [[nodiscard]] std::vector<PersistentEntity> loadChunkEntities(
        const std::string& identifier, int chunkX, int chunkZ) const;

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

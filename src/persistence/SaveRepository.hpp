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

// An experience orb awaiting pickup (XP-1). Position/velocity are the whole
// physical state, `value`/`count` the whole economic state (a merged orb
// stacks `count` orbs of the same `value` denomination into one record — see
// ExperienceOrb::merge), and `ageTicks`/`pickupDelayTicks` the two timers that
// drive despawn and the same-tick "don't touch what you just placed" guard.
struct PersistentExperienceOrb final {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float vx = 0.0F;
    float vy = 0.0F;
    float vz = 0.0F;
    std::int32_t value = 0;
    std::int32_t count = 1;
    std::uint32_t ageTicks = 0U;
    std::uint32_t pickupDelayTicks = 0U;
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
    // Whether cheats are allowed in this world — 1.16.1's level.dat allowCommands
    // (CMD-8). It drives the command source's op level: on → the host is Owners
    // (op4, every command passes), off → All (only client-side level-0 commands
    // like /help work, gameplay commands are refused by the existing permission
    // layer). Defaults true so a pre-CMD-8 world (whose WRLD block is version 1
    // and carries no flag) keeps its historical op4 behaviour on load; a newly
    // created world takes the value the create screen chose (vanilla default off).
    bool allowCommands = true;
    std::size_t selectedHotbarSlot = 0U;
    float playerHealth = gameplay::PlayerVitals::kMaximumHealth;
    std::int32_t playerFoodLevel = gameplay::PlayerVitals::kMaximumFood;
    float playerSaturation = 5.0F;
    std::int32_t playerAirTicks = gameplay::PlayerVitals::kMaximumAirTicks;
    // XP-0: the experience currency, PLYR block version 2 (JC: XpLevel/XpP
    // stored as the derived pointsIntoLevel/XpTotal/XpSeed). A pre-XP-0 world
    // (version 1) has none of these on disk; the reader leaves them at these
    // zero defaults, matching vanilla's "new player" state.
    std::int32_t playerExperienceLevel = 0;
    std::int32_t playerExperiencePoints = 0;
    std::int32_t playerTotalExperience = 0;
    std::int32_t playerEnchantmentSeed = 0;
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
    // XP-1: the experience orb pool, its own self-describing block (XPOB, added
    // after format 19 — an owner-block addition needs no format bump, the same
    // way DROP itself did not move the number when it was added). A pre-XP-1
    // world simply has no XPOB block and loads with an empty list.
    std::vector<PersistentExperienceOrb> experienceOrbs;
    // The world's own tick count and every named clock, split apart by format 13
    // so the sun can be frozen without freezing gameplay timing. Loading an
    // older save backfills both from gameTimeSeconds above, which used to carry
    // all of it at once.
    std::uint64_t serverTick = 0U;
    std::array<world::ClockState, world::kClockCount> clocks{};
};

// How a stored world's save format relates to this build's (META-2b), decided by
// comparing the world's worldVersion with kVersion.worldVersion. It is pure data:
// the world-selection UI (PX) renders it, but the classification is computed
// headless here so it can be tested and reused without a screen.
enum class WorldCompatibility : std::uint8_t {
    Openable,          // Same format number: opens as-is.
    NeedsUpgrade,      // Older format: this build can read and migrate it.
    FromNewerVersion,  // Newer format than this build understands: not openable.
};

// One world's listing entry (META-2b): the basic summary, the self-description
// read *lazily* from the save header (no chunks loaded), the on-disk size, and
// the compatibility verdict. This is the data a world-selection screen needs to
// list every world with a version/compatibility badge without opening any world.
struct WorldSummary final {
    SaveSummary summary;
    SaveVersionHeader versionHeader;
    WorldCompatibility compatibility = WorldCompatibility::Openable;
    std::uintmax_t sizeBytes = 0U;
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
    // CS-5: this chunk has been visited and its world-generation-time
    // population pass (CS-4) has already run — recorded even when the chunk
    // carries no edits and no surviving creatures (a herd that fully wandered
    // off, or a pass whose probability draw produced nobody), so a later
    // session does not mistake "no record" for "never generated" and re-run
    // the pass on top of whatever remains. See SaveRepository::saveChunk.
    bool populated = false;
};

class SaveRepository final {
  public:
    explicit SaveRepository(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }
    [[nodiscard]] std::vector<SaveSummary> list() const;
    // META-2b: a version-aware listing. Like list(), but for each world it also
    // reads the SaveVersionHeader *lazily* — only world.dat's small header/blocks,
    // never the region chunks — and classifies compatibility against this build.
    // This is what a world-selection screen consumes; the screen itself is PX.
    [[nodiscard]] std::vector<WorldSummary> worldSummaries() const;
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
    // merges with whatever the region already holds; empty data with
    // `populated == false` removes the chunk's record (and the file when the
    // region empties) — but `populated == true` keeps a record on disk even
    // with no edits and no entities, a bare marker (CS-5) proving the chunk's
    // generation-time population pass already ran so a later session does not
    // re-run it on a herd that has since fully wandered off.
    // `dimension` selects the per-dimension region subdirectory (DIM-4): the
    // Overworld writes to `<world>/region/` (unchanged, so old flat worlds are
    // byte-compatible), the Nether to `<world>/DIM-1/region/` and the End to
    // `<world>/DIM1/region/` — the vanilla 1.16.1 layout JC3 import targets, so
    // the compat layer adds no new deviation. Defaulted to Overworld so every
    // existing single-dimension caller keeps its behaviour.
    void saveChunk(const std::string& identifier, int chunkX, int chunkZ,
                   std::vector<world::PersistentBlockEdit> edits,
                   std::vector<PersistentEntity> entities, bool populated = false,
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
    // CS-5: whether this chunk's region record carries the `populated` marker —
    // its world-generation-time population pass (CS-4) has already run, even if
    // no edits or entities currently back it up. False for a chunk with no
    // record at all (never visited) and for a region written by a pre-CS-5
    // build (the version-4-and-earlier CCNK layout has no such field, defaults
    // to unset — see readRegionFile), which is the correct backward-compatible
    // read: an old save's chunks look "never populated" and get exactly one
    // legitimate population pass the first time this build visits them.
    [[nodiscard]] bool isChunkPopulated(
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

    // Diagnostic: how many region files have been opened for chunk loading in
    // this process, ever. worldSummaries() reads only world.dat's header and must
    // not touch region/, so a test proves that by asserting this counter is
    // unchanged across a worldSummaries() call. Monotonic, process-wide.
    [[nodiscard]] static std::uint64_t regionReadCount();

  private:
    std::filesystem::path root_;
};

} // namespace mc::persistence

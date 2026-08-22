#include "persistence/SaveRepository.hpp"

#include "gameplay/ItemRegistry.hpp"
#include "persistence/SaveStream.hpp"
#include "persistence/UnknownBlockTable.hpp"

#include "world/BlockRegistry.hpp"
#include "world/DayNightCycle.hpp"
#include "world/WorldConstants.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace mc::persistence {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{'M', 'C', 'R', 'B', 'S', 'A', 'V', 'E'};
// Format 8 moved `randomTickSpeed` into a fixed header field; format 9 replaces
// that with a sparse, self-describing GameRules block after the chests section;
// format 10 appends the /spawnpoint block; format 11 appends the weather block;
// format 12 appends the entity block; format 13 appends the clock block, which
// splits the single gameTimeSeconds into a server tick and the named clocks;
// format 14 gives each block edit a LIT flag, after the burning furnace and the
// four wall torches stopped being blocks of their own and became states.
// Format 17 stops writing sections at fixed offsets. Everything is a
// self-describing block now, the reader dispatches on the tag, and adding a
// state owner needs neither a format bump nor a positional read.
// Format 19 (DIM-4) adds per-dimension region subdirectories, mirroring vanilla's
// 1.16.1 layout: the Overworld stays at `<world>/region` (byte-identical to
// format 18, so an old flat world is read back unchanged as the Overworld), while
// the Nether writes to `<world>/DIM-1/region` and the End to `<world>/DIM1/region`.
// The bump only advertises the capability — an 18 (or older) world has no
// dimension subfolders and loads exactly as before (only the Overworld present).
constexpr std::uint32_t kFormatVersion = 19U;
constexpr std::uint32_t kFirstOwnerDrivenFormatVersion = 17U;
constexpr std::uint32_t kOldestSupportedFormatVersion = 1U;
constexpr std::uint64_t kMaximumEdits = 16U * 1024U * 1024U;
constexpr std::uint64_t kMaximumChests = 1024U * 1024U;
// Format 5 stopped writing raw enum ordinals and started writing a palette of
// namespaced identifiers, so blocks may be added, removed or reordered freely.
// Format 6 gave items the same treatment.
// Format 18 stopped writing a cell as a block plus three loose fields and
// started writing a palette of *states*: a block identifier plus named
// properties. That is what lets a block gain a fourth property without the file
// layout gaining a column.
[[maybe_unused]] constexpr std::uint32_t kFirstStatePaletteFormatVersion = 18U;
constexpr std::uint32_t kFirstBlockPaletteFormatVersion = 5U;
constexpr std::uint32_t kFirstItemPaletteFormatVersion = 6U;
constexpr std::uint32_t kMaximumPaletteEntries = 65535U;
// What one edit costs on disk in each CHNK layout, plus a slack allowance for
// the header, the palettes and the small blocks. Used to size the write buffer
// up front and to reject a truncated block before reserving from a lying count.
constexpr std::size_t kEditRecordBytes = 5U;         // CHNK version 2
constexpr std::size_t kLegacyEditRecordBytes = 6U;   // CHNK version 1
constexpr std::size_t kReservedPrologueBytes = 8192U;

// The block order formats 1 through 4 wrote ordinals against. It is frozen
// history: never reorder or remove a line, only append if an old save could
// contain a higher ordinal. Anything missing from the current registry loads
// as air.
constexpr std::array<std::string_view, 60> kLegacyBlockOrder{
    "air", "grass_block", "dirt", "stone", "cobblestone", "oak_planks", "oak_log",
    "bricks", "bedrock", "sand", "glass", "coal_ore", "iron_ore", "gold_ore",
    "diamond_ore", "grass", "dandelion", "oak_sapling", "oak_leaves", "water",
    "gravel", "spruce_planks", "birch_planks", "spruce_log", "birch_log",
    "bookshelf", "crafting_table", "furnace", "obsidian", "clay", "snow_block",
    "netherrack", "glowstone", "white_wool", "red_wool", "black_wool",
    "stone_bricks", "mossy_cobblestone", "sandstone", "pumpkin", "melon", "tnt",
    "granite", "diorite", "andesite", "coarse_dirt", "podzol", "red_sand",
    "torch", "wall_torch_north", "wall_torch_east", "wall_torch_south",
    "wall_torch_west", "chest", "lapis_ore", "redstone_ore", "emerald_ore",
    "mossy_stone_bricks", "chiseled_stone_bricks", "quartz_block",
};

// Identifiers that used to name a block and now name one of its states. A save
// written before format 14 refers to them, so resolving a palette entry has to
// hand back both the block it became and the state it carried; the per-edit
// fields cannot express it, because the old format had nowhere to put it.
struct LegacyStateOverride final {
    world::Block block = world::Block::Air;
    std::optional<world::BlockOrientation> orientation{};
    bool lit = false;
};

[[nodiscard]] std::optional<LegacyStateOverride> legacyStateIdentifier(std::string_view text) {
    const auto bare = [&](std::string_view name) {
        return text == name || text == "rebedrock:" + std::string{name} ||
               text == "minecraft:" + std::string{name};
    };
    if (bare("lit_furnace")) {
        return LegacyStateOverride{world::Block::Furnace, std::nullopt, true};
    }
    if (bare("wall_torch_north")) {
        return LegacyStateOverride{world::Block::WallTorch, world::BlockOrientation::North, false};
    }
    if (bare("wall_torch_east")) {
        return LegacyStateOverride{world::Block::WallTorch, world::BlockOrientation::East, false};
    }
    if (bare("wall_torch_south")) {
        return LegacyStateOverride{world::Block::WallTorch, world::BlockOrientation::South, false};
    }
    if (bare("wall_torch_west")) {
        return LegacyStateOverride{world::Block::WallTorch, world::BlockOrientation::West, false};
    }
    return std::nullopt;
}

[[nodiscard]] world::Block legacyBlockFromOrdinal(std::uint8_t ordinal) {
    if (ordinal >= kLegacyBlockOrder.size()) {
        throw std::runtime_error("world.dat references an unknown legacy block");
    }
    const auto block = world::blockFromIdentifier(kLegacyBlockOrder[ordinal]);
    return block.value_or(world::Block::Air);
}

// The item order formats 1 through 5 wrote ordinals against. Frozen history,
// exactly like kLegacyBlockOrder above.
constexpr std::array<std::string_view, 27> kLegacyItemOrder{
    "air", "bucket", "water_bucket", "coal", "iron_ingot", "gold_ingot", "diamond",
    "emerald", "stick", "apple", "bread", "wooden_pickaxe", "stone_pickaxe",
    "iron_pickaxe", "diamond_pickaxe", "golden_pickaxe", "flint", "feather",
    "string", "leather", "sugar", "egg", "bone", "paper", "book", "pig_spawn_egg",
    "zombie_spawn_egg",
};

[[nodiscard]] const gameplay::Item* legacyItemFromOrdinal(std::uint8_t ordinal) {
    if (ordinal >= kLegacyItemOrder.size()) {
        throw std::runtime_error("world.dat references an unknown legacy item");
    }
    // Every legacy name is still a registered item, so the frozen ordinal table
    // resolves straight to the corresponding Item (nullptr = the block sentinel).
    return gameplay::itemFromIdentifier(kLegacyItemOrder[ordinal]);
}

// Formats 1-17 stored a cell as a block plus three loose fields, and the
// orientation byte was overloaded: a crop's age, farmland's moisture and the
// leaves persistence flag all rode in it, masked back out with `& 0x7`. This
// turns that encoding into a state.
//
// Frozen history, exactly like the ordinal tables above: it describes what old
// files contain. It must not be "kept in sync" with the current schema — if a
// crop's age range ever changes, this function still decodes the old range.
[[nodiscard]] world::BlockState legacyBlockState(world::Block block, std::uint8_t orientation,
                                                 std::uint8_t fluidLevel, bool lit) {
    auto state = world::BlockState{block};
    if (world::isCrop(block)) {
        state = state.withAge(orientation & 0x7);
    } else if (world::isFarmland(block)) {
        state = state.withMoisture(orientation & 0x7);
    } else if (world::isLeaves(block)) {
        // kPersistentLeavesState was BlockOrientation::East, ordinal 1.
        state = state.withPersistent(orientation == 1U);
    } else {
        state = state.with(world::StateProperty::Facing, orientation);
    }
    return state.withFluidLevel(fluidLevel).withLit(lit);
}

// Blocks are a dense BlockId, so the palette indexes an array instead of
// hashing: one load per stack rather than a hash and a bucket walk. Sized to the
// block registry at construction (see makeBlockPalette), so it holds however
// many block identities exist rather than the old 256 ceiling.
using BlockPalette = DensePalette<world::BlockId>;

// A palette empty-keyed on air and sized to every registered block identity.
[[nodiscard]] inline BlockPalette makeBlockPalette() {
    return BlockPalette{world::blockId(world::Block::Air), world::blockCount()};
}

// Resolves a saved block identifier through the runtime block registry — the
// single source of block identity since R0-2 — returning the block for a known
// name and nothing for one this build does not carry. This replaces the direct
// enum walk `blockFromIdentifier` did, so external content registered into the
// registry resolves the same way built-ins do, and the `minecraft:` alias a
// 1.16.1 save uses still lands on its block.
[[nodiscard]] std::optional<world::Block> blockByName(std::string_view text) {
    const world::BlockId id = world::blockRegistry().byName(text);
    if (!id.valid()) {
        return std::nullopt;
    }
    return world::blockFromId(id);
}
// States are keyed by their raw interned id, which is compact but *not* stable
// across builds — so the id is only ever the palette's key, never the thing
// written. Each entry goes to disk as a block identifier plus its named
// property values. Air's default state is id 0, which is the palette's empty
// sentinel and therefore index 0, exactly like the block palette's air.
using StatePalette = HashPalette<std::uint16_t, 0U>;
// Items are keyed by their registered instance; nullptr is the block sentinel
// and, as always, palette index 0.
using ItemPalette = HashPalette<const gameplay::Item*, nullptr>;

[[nodiscard]] std::int64_t nowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] bool safeIdentifier(std::string_view identifier) {
    return !identifier.empty() && identifier.size() <= 96U &&
        std::ranges::all_of(identifier, [](unsigned char character) {
            return std::isalnum(character) != 0 || character == '-' || character == '_';
        });
}

[[nodiscard]] std::map<std::string, std::string> readProperties(
    const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error("Unable to open save metadata: " + path.string());
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        values.insert_or_assign(line.substr(0, separator), line.substr(separator + 1U));
    }
    return values;
}

// FNV-1a over everything before the trailing checksum field: a torn or edited
// world.dat is rejected rather than half-loaded.
[[nodiscard]] std::uint64_t checksum(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

void replaceFile(const std::filesystem::path& target, std::span<const std::uint8_t> bytes) {
    std::filesystem::create_directories(target.parent_path());
    const auto temporary = target.string() + ".tmp";
    const auto backup = target.string() + ".bak";
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output) throw std::runtime_error("Unable to create save file: " + temporary);
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!output) throw std::runtime_error("Unable to write save file: " + temporary);
    }
    std::error_code error;
    std::filesystem::remove(backup, error);
    error.clear();
    if (std::filesystem::exists(target)) {
        std::filesystem::rename(target, backup, error);
        if (error) throw std::runtime_error("Unable to rotate save backup: " + error.message());
    }
    std::filesystem::rename(temporary, target, error);
    if (error) {
        if (std::filesystem::exists(backup)) {
            std::error_code ignored;
            std::filesystem::rename(backup, target, ignored);
        }
        throw std::runtime_error("Unable to install save file: " + error.message());
    }
    std::filesystem::remove(backup, error);
}

void writeMetadata(const std::filesystem::path& path, const SaveSummary& summary) {
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output{temporary, std::ios::trunc};
        if (!output) throw std::runtime_error("Unable to write level.properties");
        output << "format=" << kFormatVersion << '\n'
               << "id=" << summary.identifier << '\n'
               << "name=" << summary.displayName << '\n'
               << "seed=" << summary.seed << '\n'
               << "last_played=" << summary.lastPlayedUnixSeconds << '\n';
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error) throw std::runtime_error("Unable to install level.properties: " + error.message());
}

[[nodiscard]] SaveSummary summaryFromProperties(
    const std::filesystem::path& path,
    const std::string& directoryIdentifier) {
    const auto properties = readProperties(path);
    const auto format = properties.contains("format")
        ? std::stoul(properties.at("format")) : 0UL;
    if (format < kOldestSupportedFormatVersion || format > kFormatVersion) {
        throw std::runtime_error("Unsupported save format in " + path.string());
    }
    SaveSummary summary;
    summary.identifier = directoryIdentifier;
    summary.displayName = properties.contains("name")
        ? properties.at("name") : directoryIdentifier;
    summary.seed = properties.contains("seed")
        ? std::stoull(properties.at("seed")) : 0U;
    summary.lastPlayedUnixSeconds = properties.contains("last_played")
        ? std::stoll(properties.at("last_played")) : 0;
    return summary;
}

// The GameRules block is the self-describing region format 9 appends after the
// chests section:
//
//   u32 blockTag          // 'G','R','U','L' — lets a future world.dat refactor
//                         // into a tag sequence locate this block by identity
//   u32 blockSizeBytes    // whole block length incl. this field — an unknown
//                         // future block version is skipped in one jump
//   u16 blockVersion      // 1
//   u16 entryCount        // only rules differing from their default (sparse)
//   entries[]:
//     u16 nameLength + name bytes
//     u8  typeTag         // Boolean=0, Int=1; Float/Compound reserved
//     u32 valueLength     // payload length — unknown name/type skip exactly
//     payload             // Boolean: u8, Int: i32 LE
//
// The four weaknesses of a hand-rolled block versus NBT are answered here:
// every entry self-describes (name/type/length/value), the block carries its
// own version and size, storage is sparse, and the type-tag space is reserved
// so nested payloads can arrive without reworking the framing.
constexpr std::uint32_t kGameRulesBlockTag =
    'G' | ('R' << 8) | ('U' << 16) | ('L' << 24);
constexpr std::uint16_t kGameRulesBlockVersion = 1U;

void appendGameRulesBlock(std::vector<std::uint8_t>& bytes,
                          const gameplay::GameRules& rules) {
    const auto& definitions = gameplay::kGameRuleDefinitions;
    std::uint16_t entryCount = 0U;
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        if (rules.value(static_cast<gameplay::GameRuleId>(index)) !=
            definitions[index].defaultValue) {
            ++entryCount;
        }
    }
    const std::size_t blockStart = bytes.size();
    appendInteger(bytes, kGameRulesBlockTag);
    appendInteger(bytes, 0U);  // blockSizeBytes, patched after the entries
    appendInteger(bytes, kGameRulesBlockVersion);
    appendInteger(bytes, entryCount);
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        const auto& definition = definitions[index];
        const auto id = static_cast<gameplay::GameRuleId>(index);
        const auto& current = rules.value(id);
        if (current == definition.defaultValue) {
            continue;
        }
        appendString(bytes, definition.name);
        appendInteger(bytes, static_cast<std::uint8_t>(definition.type));
        switch (definition.type) {
        case gameplay::GameRuleType::Boolean:
            appendInteger(bytes, static_cast<std::uint32_t>(1U));
            appendInteger(bytes, static_cast<std::uint8_t>(std::get<bool>(current) ? 1U : 0U));
            break;
        case gameplay::GameRuleType::Int:
            appendInteger(bytes, static_cast<std::uint32_t>(4U));
            appendInteger(bytes, std::get<std::int32_t>(current));
            break;
        }
    }
    const auto blockSize = static_cast<std::uint32_t>(bytes.size() - blockStart);
    for (std::size_t offset = 0; offset < sizeof(std::uint32_t); ++offset) {
        bytes[blockStart + 4U + offset] =
            static_cast<std::uint8_t>(blockSize >> (offset * 8U));
    }
}

void readGameRulesBlock(std::span<const std::uint8_t> payload, std::size_t& cursor,
                        gameplay::GameRules& rules) {
    const std::size_t blockStart = cursor;
    if (blockStart + 12U > payload.size()) {
        throw std::runtime_error("world.dat game rules block is truncated");
    }
    const auto tag = readInteger<std::uint32_t>(payload, cursor);
    if (tag != kGameRulesBlockTag) {
        throw std::runtime_error("world.dat has an invalid game rules block");
    }
    const auto blockSize = readInteger<std::uint32_t>(payload, cursor);
    if (blockSize < 12U || static_cast<std::size_t>(blockSize) > payload.size() - blockStart) {
        throw std::runtime_error("world.dat game rules block is malformed");
    }
    const auto blockVersion = readInteger<std::uint16_t>(payload, cursor);
    if (blockVersion > kGameRulesBlockVersion) {
        // A future build's entry layout is unknowable; skip the whole region.
        cursor = blockStart + blockSize;
        return;
    }
    const auto entryCount = readInteger<std::uint16_t>(payload, cursor);
    const std::size_t blockEnd = blockStart + blockSize;
    for (std::uint16_t index = 0; index < entryCount; ++index) {
        const auto name = readString(payload, cursor);
        const auto typeTag = readInteger<std::uint8_t>(payload, cursor);
        const auto valueLength = readInteger<std::uint32_t>(payload, cursor);
        const std::size_t valueStart = cursor;
        // `valueStart > blockEnd` is checked separately so the subtraction
        // below cannot underflow on a truncated entry.
        if (valueStart > blockEnd ||
            static_cast<std::size_t>(valueLength) > blockEnd - valueStart) {
            throw std::runtime_error("world.dat game rules block entry is malformed");
        }
        if (typeTag == static_cast<std::uint8_t>(gameplay::GameRuleType::Boolean)) {
            if (valueLength != 1U) {
                throw std::runtime_error("world.dat game rules block has an invalid boolean");
            }
            const auto raw = readInteger<std::uint8_t>(payload, cursor);
            if (raw > 1U) {
                throw std::runtime_error("world.dat game rules block has an invalid boolean");
            }
            // Unknown rule names or mismatched types are skipped and keep the
            // default, so a newer save never breaks an older build.
            static_cast<void>(rules.applyDecoded(name, gameplay::GameRuleType::Boolean,
                                                 raw != 0U));
        } else if (typeTag == static_cast<std::uint8_t>(gameplay::GameRuleType::Int)) {
            if (valueLength != 4U) {
                throw std::runtime_error("world.dat game rules block has an invalid integer");
            }
            const auto value = readInteger<std::int32_t>(payload, cursor);
            static_cast<void>(
                rules.applyDecoded(name, gameplay::GameRuleType::Int, value));
        } else {
            // Unknown future type tag; skip its payload and keep the default.
            cursor = valueStart + valueLength;
        }
    }
    if (cursor != blockEnd) {
        throw std::runtime_error("world.dat game rules block has trailing data");
    }
}

// The /spawnpoint result rides in its own sparse, self-describing block, the
// same shape as the game rules block: a tag, a size, a version, then the fixed
// fields. A build that does not know the block skips the whole region.
constexpr std::uint32_t kSpawnPointBlockTag =
    'S' | ('P' << 8) | ('W' << 16) | ('N' << 24);
constexpr std::uint16_t kSpawnPointBlockVersion = 1U;

void appendSpawnPointBlock(std::vector<std::uint8_t>& bytes, const SaveGame& game) {
    const std::size_t blockStart = bytes.size();
    appendInteger(bytes, kSpawnPointBlockTag);
    appendInteger(bytes, 0U);  // blockSizeBytes, patched after the fields
    appendInteger(bytes, kSpawnPointBlockVersion);
    appendInteger(bytes, static_cast<std::uint8_t>(game.hasSpawnPoint ? 1U : 0U));
    appendFloat(bytes, game.spawnX);
    appendFloat(bytes, game.spawnY);
    appendFloat(bytes, game.spawnZ);
    appendFloat(bytes, game.spawnYaw);
    const auto blockSize = static_cast<std::uint32_t>(bytes.size() - blockStart);
    for (std::size_t offset = 0; offset < sizeof(std::uint32_t); ++offset) {
        bytes[blockStart + 4U + offset] =
            static_cast<std::uint8_t>(blockSize >> (offset * 8U));
    }
}

void readSpawnPointBlock(std::span<const std::uint8_t> payload, std::size_t& cursor,
                         SaveGame& game) {
    const std::size_t blockStart = cursor;
    if (blockStart + 12U > payload.size()) {
        throw std::runtime_error("world.dat spawn point block is truncated");
    }
    const auto tag = readInteger<std::uint32_t>(payload, cursor);
    if (tag != kSpawnPointBlockTag) {
        throw std::runtime_error("world.dat has an invalid spawn point block");
    }
    const auto blockSize = readInteger<std::uint32_t>(payload, cursor);
    if (blockSize < 15U || static_cast<std::size_t>(blockSize) > payload.size() - blockStart) {
        throw std::runtime_error("world.dat spawn point block is malformed");
    }
    const auto blockVersion = readInteger<std::uint16_t>(payload, cursor);
    if (blockVersion > kSpawnPointBlockVersion) {
        cursor = blockStart + blockSize;
        return;
    }
    game.hasSpawnPoint = readInteger<std::uint8_t>(payload, cursor) != 0U;
    game.spawnX = readFloat(payload, cursor);
    game.spawnY = readFloat(payload, cursor);
    game.spawnZ = readFloat(payload, cursor);
    game.spawnYaw = readFloat(payload, cursor);
    if (cursor != blockStart + blockSize) {
        throw std::runtime_error("world.dat spawn point block has trailing data");
    }
}

// The weather state rides in its own sparse, self-describing block (format 11),
// the same shape as the game rules and spawn point blocks: a tag, a size, a
// version, then the fixed fields. LevelProperties persists the same five
// fields; the smoothed rain/thunder gradients are transient and never saved.
constexpr std::uint32_t kWeatherBlockTag =
    'W' | ('E' << 8) | ('A' << 16) | ('T' << 24);
constexpr std::uint16_t kWeatherBlockVersion = 1U;

void appendWeatherBlock(std::vector<std::uint8_t>& bytes, const SaveGame& game) {
    const std::size_t blockStart = bytes.size();
    appendInteger(bytes, kWeatherBlockTag);
    appendInteger(bytes, 0U);  // blockSizeBytes, patched after the fields
    appendInteger(bytes, kWeatherBlockVersion);
    appendInteger(bytes, static_cast<std::uint8_t>(game.weather.raining ? 1U : 0U));
    appendInteger(bytes, static_cast<std::uint8_t>(game.weather.thundering ? 1U : 0U));
    appendInteger(bytes, game.weather.rainTime);
    appendInteger(bytes, game.weather.thunderTime);
    appendInteger(bytes, game.weather.clearWeatherTime);
    const auto blockSize = static_cast<std::uint32_t>(bytes.size() - blockStart);
    for (std::size_t offset = 0; offset < sizeof(std::uint32_t); ++offset) {
        bytes[blockStart + 4U + offset] =
            static_cast<std::uint8_t>(blockSize >> (offset * 8U));
    }
}

void readWeatherBlock(std::span<const std::uint8_t> payload, std::size_t& cursor,
                      SaveGame& game) {
    const std::size_t blockStart = cursor;
    if (blockStart + 12U > payload.size()) {
        throw std::runtime_error("world.dat weather block is truncated");
    }
    const auto tag = readInteger<std::uint32_t>(payload, cursor);
    if (tag != kWeatherBlockTag) {
        throw std::runtime_error("world.dat has an invalid weather block");
    }
    const auto blockSize = readInteger<std::uint32_t>(payload, cursor);
    if (blockSize < 24U || static_cast<std::size_t>(blockSize) > payload.size() - blockStart) {
        throw std::runtime_error("world.dat weather block is malformed");
    }
    const auto blockVersion = readInteger<std::uint16_t>(payload, cursor);
    if (blockVersion > kWeatherBlockVersion) {
        cursor = blockStart + blockSize;
        return;
    }
    game.weather.raining = readInteger<std::uint8_t>(payload, cursor) != 0U;
    game.weather.thundering = readInteger<std::uint8_t>(payload, cursor) != 0U;
    game.weather.rainTime = readInteger<std::int32_t>(payload, cursor);
    game.weather.thunderTime = readInteger<std::int32_t>(payload, cursor);
    game.weather.clearWeatherTime = readInteger<std::int32_t>(payload, cursor);
    if (cursor != blockStart + blockSize) {
        throw std::runtime_error("world.dat weather block has trailing data");
    }
}

// The ENTITY block is the self-describing region format 12 appends after the
// weather block, mirroring the GameRules framing:
//
//   u32 blockTag          // 'E','N','T','Y'
//   u32 blockSizeBytes    // whole block length incl. this field
//   u16 blockVersion      // 1
//   u16 speciesCount
//   species[]: u16 nameLen + name   // the registered id path, e.g. "pig"
//   u32 entityCount
//   entities[]:
//     u16 speciesIndex
//     f32 x, y, z
//     f32 yaw
//     f32 vx, vy, vz
//     f32 health
//     i32 angerTicks
//     u32 ageTicks
//     u32 rngState
//     i32 fireTicks       // version >= 2; absent (read as 0) in version 1
//     u8  flags           // reserved
//     u8  effectCount     // version >= 3; absent (read as 0) in versions 1-2
//     effects[]:          // version >= 3
//       u16 nameLen + name   // the effect registry path, e.g. "poison"
//       i32 durationTicks
//       u8  amplifier
//     i32 age             // version >= 4; absent (read as 0) in versions 1-3
//     i32 loveTicks       // version >= 4
//
// Species are palette-encoded the same way blocks/items are, so a species that
// is removed in a future build skips cleanly on load (the unknown name becomes
// an unknown palette entry) instead of renumbering every record.
//
// Version 2 inserts `fireTicks` before the reserved flags byte so a creature
// saved mid-burn reopens still ablaze. Version 3 appends the active MobEffects
// after the flags byte (by name, not id). Version 4 appends AgeableMob age/love.
// An older region omits the newer fields, which read back as their defaults, so
// an old world migrates without a fixer.
constexpr std::uint32_t kEntityBlockTag =
    'E' | ('N' << 8) | ('T' << 16) | ('Y' << 24);
constexpr std::uint16_t kEntityBlockVersion = 4U;

// An entity's active MobEffects, shared by the world.dat ENTITY block and the
// per-chunk region record (both grew effects in the same version bump). Effects
// are stored by name, and capped at 255 so the count fits one byte — far above
// the handful an entity ever carries.
void appendEffectList(std::vector<std::uint8_t>& bytes,
                      const std::vector<PersistentEffect>& effects) {
    const auto count = static_cast<std::uint8_t>(
        std::min<std::size_t>(effects.size(), 255U));
    appendInteger(bytes, count);
    for (std::uint8_t index = 0; index < count; ++index) {
        const PersistentEffect& effect = effects[index];
        appendString(bytes, effect.name);
        appendInteger(bytes, effect.durationTicks);
        appendInteger(bytes, effect.amplifier);
    }
}

void readEffectList(std::span<const std::uint8_t> payload, std::size_t& cursor,
                    std::vector<PersistentEffect>& effects) {
    const auto count = readInteger<std::uint8_t>(payload, cursor);
    effects.reserve(count);
    for (std::uint8_t index = 0; index < count; ++index) {
        PersistentEffect effect;
        effect.name = readString(payload, cursor);
        effect.durationTicks = readInteger<std::int32_t>(payload, cursor);
        effect.amplifier = readInteger<std::uint8_t>(payload, cursor);
        effects.push_back(std::move(effect));
    }
}

void appendEntityBlock(std::vector<std::uint8_t>& bytes,
                       const std::vector<PersistentEntity>& entities) {
    const std::size_t blockStart = bytes.size();
    appendInteger(bytes, kEntityBlockTag);
    appendInteger(bytes, 0U);  // blockSizeBytes, patched after the records
    appendInteger(bytes, kEntityBlockVersion);
    // Species palette: gather the names in first-use order.
    std::vector<std::string> speciesPalette;
    std::unordered_map<std::string, std::uint16_t> speciesIndices;
    const auto indexOf = [&](const std::string& name) {
        const auto existing = speciesIndices.find(name);
        if (existing != speciesIndices.end()) {
            return existing->second;
        }
        const auto index = static_cast<std::uint16_t>(speciesPalette.size());
        speciesPalette.push_back(name);
        speciesIndices.emplace(name, index);
        return index;
    };
    for (const auto& entity : entities) {
        static_cast<void>(indexOf(entity.species));
    }
    appendInteger(bytes, static_cast<std::uint16_t>(speciesPalette.size()));
    for (const auto& name : speciesPalette) {
        appendString(bytes, name);
    }
    appendInteger(bytes, static_cast<std::uint32_t>(entities.size()));
    for (const auto& entity : entities) {
        appendInteger(bytes, indexOf(entity.species));
        appendFloat(bytes, entity.x);
        appendFloat(bytes, entity.y);
        appendFloat(bytes, entity.z);
        appendFloat(bytes, entity.yaw);
        appendFloat(bytes, entity.vx);
        appendFloat(bytes, entity.vy);
        appendFloat(bytes, entity.vz);
        appendFloat(bytes, entity.health);
        appendInteger(bytes, entity.angerTicks);
        appendInteger(bytes, entity.ageTicks);
        appendInteger(bytes, entity.rngState);
        appendInteger(bytes, entity.fireTicks);  // version 2
        appendInteger(bytes, static_cast<std::uint8_t>(0U));  // flags, reserved
        appendEffectList(bytes, entity.effects);  // version 3
        appendInteger(bytes, entity.age);        // version 4
        appendInteger(bytes, entity.loveTicks);  // version 4
    }
    const auto blockSize = static_cast<std::uint32_t>(bytes.size() - blockStart);
    for (std::size_t offset = 0; offset < sizeof(std::uint32_t); ++offset) {
        bytes[blockStart + 4U + offset] =
            static_cast<std::uint8_t>(blockSize >> (offset * 8U));
    }
}

void readEntityBlock(std::span<const std::uint8_t> payload, std::size_t& cursor,
                     std::vector<PersistentEntity>& entities) {
    const std::size_t blockStart = cursor;
    if (blockStart + 12U > payload.size()) {
        throw std::runtime_error("world.dat entity block is truncated");
    }
    const auto tag = readInteger<std::uint32_t>(payload, cursor);
    if (tag != kEntityBlockTag) {
        throw std::runtime_error("world.dat has an invalid entity block");
    }
    const auto blockSize = readInteger<std::uint32_t>(payload, cursor);
    if (blockSize < 12U || static_cast<std::size_t>(blockSize) > payload.size() - blockStart) {
        throw std::runtime_error("world.dat entity block is malformed");
    }
    const auto blockVersion = readInteger<std::uint16_t>(payload, cursor);
    if (blockVersion > kEntityBlockVersion) {
        // A future build's record layout is unknowable; skip the whole region.
        cursor = blockStart + blockSize;
        return;
    }
    const std::size_t blockEnd = blockStart + blockSize;
    const auto speciesCount = readInteger<std::uint16_t>(payload, cursor);
    if (blockEnd - cursor < static_cast<std::size_t>(speciesCount) * 2U) {
        throw std::runtime_error("world.dat entity block species list is truncated");
    }
    std::vector<std::string> species;
    species.reserve(speciesCount);
    for (std::uint16_t index = 0; index < speciesCount; ++index) {
        species.push_back(readString(payload, cursor));
    }
    const auto entityCount = readInteger<std::uint32_t>(payload, cursor);
    entities.reserve(static_cast<std::size_t>(entityCount));
    for (std::uint32_t index = 0; index < entityCount; ++index) {
        if (cursor >= blockEnd) {
            throw std::runtime_error("world.dat entity block is truncated");
        }
        const auto speciesIndex = readInteger<std::uint16_t>(payload, cursor);
        if (speciesIndex >= species.size()) {
            throw std::runtime_error("world.dat entity block references an unknown species");
        }
        PersistentEntity entity;
        entity.species = species[speciesIndex];
        entity.x = readFloat(payload, cursor);
        entity.y = readFloat(payload, cursor);
        entity.z = readFloat(payload, cursor);
        entity.yaw = readFloat(payload, cursor);
        entity.vx = readFloat(payload, cursor);
        entity.vy = readFloat(payload, cursor);
        entity.vz = readFloat(payload, cursor);
        entity.health = readFloat(payload, cursor);
        entity.angerTicks = readInteger<std::int32_t>(payload, cursor);
        entity.ageTicks = readInteger<std::uint32_t>(payload, cursor);
        entity.rngState = readInteger<std::uint32_t>(payload, cursor);
        // fireTicks arrived in version 2; a version-1 record leaves it at its
        // default zero (not on fire), which is exactly a migrated old world.
        if (blockVersion >= 2U) {
            entity.fireTicks = readInteger<std::int32_t>(payload, cursor);
        }
        static_cast<void>(readInteger<std::uint8_t>(payload, cursor));  // flags, reserved
        // Active effects arrived in version 3; versions 1-2 leave the list empty.
        if (blockVersion >= 3U) {
            readEffectList(payload, cursor, entity.effects);
        }
        // AgeableMob age/love arrived in version 4; earlier records read as zero.
        if (blockVersion >= 4U) {
            entity.age = readInteger<std::int32_t>(payload, cursor);
            entity.loveTicks = readInteger<std::int32_t>(payload, cursor);
        }
        // A creature saved outside the world is a corrupt record.
        if (!(entity.y >= -64.0F && entity.y <= 384.0F)) {
            throw std::runtime_error("world.dat entity block has an invalid position");
        }
        entities.push_back(std::move(entity));
    }
    if (cursor != blockEnd) {
        throw std::runtime_error("world.dat entity block has trailing data");
    }
}

// The DROP block is the self-describing region format 16 appends, carrying the
// two entity kinds that had no home in the save: dropped items and blocks
// mid-fall.
//
//   u32 blockTag          // 'D','R','O','P'
//   u32 blockSizeBytes    // whole block length incl. this field
//   u16 blockVersion
//   u16 itemPaletteCount, [string]*   // "" is the block-only sentinel
//   u16 blockPaletteCount, [string]*
//   u32 dropCount
//   drops[]:  u16 itemIndex, u16 blockIndex, u8 count,
//             f32 x, y, z, f32 vx, vy, vz, u32 ageTicks
//   u32 fallingCount
//   falling[]: u16 blockIndex, f32 x, y, z, f32 verticalVelocity
//
// Both palettes are local to the block, the way the entity block keeps its own
// species list: an item or block dropped from a future build skips cleanly
// instead of renumbering anything.
constexpr std::uint32_t kDropBlockTag =
    'D' | ('R' << 8) | ('O' << 16) | ('P' << 24);
constexpr std::uint16_t kDropBlockVersion = 1U;

void appendDropBlock(std::vector<std::uint8_t>& bytes,
                     const std::vector<PersistentItemDrop>& drops,
                     const std::vector<PersistentFallingBlock>& falling) {
    const std::size_t blockStart = bytes.size();
    appendInteger(bytes, kDropBlockTag);
    appendInteger(bytes, 0U);  // blockSizeBytes, patched below
    appendInteger(bytes, kDropBlockVersion);

    ItemPalette itemPalette;
    BlockPalette blockPalette = makeBlockPalette();
    for (const auto& drop : drops) {
        static_cast<void>(itemPalette.indexOf(drop.stack.item));
        static_cast<void>(blockPalette.indexOf(world::blockId(drop.stack.block)));
    }
    for (const auto& entity : falling) {
        static_cast<void>(blockPalette.indexOf(world::blockId(entity.block)));
    }
    appendInteger(bytes, static_cast<std::uint16_t>(itemPalette.entries().size()));
    for (const auto* item : itemPalette.entries()) {
        appendString(bytes, item == nullptr ? std::string{} : item->identifier.toString());
    }
    appendInteger(bytes, static_cast<std::uint16_t>(blockPalette.entries().size()));
    for (const auto block : blockPalette.entries()) {
        appendString(bytes, world::blockRegistry().identifier(block).toString());
    }

    appendInteger(bytes, static_cast<std::uint32_t>(drops.size()));
    for (const auto& drop : drops) {
        appendInteger(bytes, itemPalette.indexOf(drop.stack.item));
        appendInteger(bytes, blockPalette.indexOf(world::blockId(drop.stack.block)));
        appendInteger(bytes, drop.stack.count);
        appendFloat(bytes, drop.x);
        appendFloat(bytes, drop.y);
        appendFloat(bytes, drop.z);
        appendFloat(bytes, drop.vx);
        appendFloat(bytes, drop.vy);
        appendFloat(bytes, drop.vz);
        appendInteger(bytes, drop.ageTicks);
    }
    appendInteger(bytes, static_cast<std::uint32_t>(falling.size()));
    for (const auto& entity : falling) {
        appendInteger(bytes, blockPalette.indexOf(world::blockId(entity.block)));
        appendFloat(bytes, entity.x);
        appendFloat(bytes, entity.y);
        appendFloat(bytes, entity.z);
        appendFloat(bytes, entity.verticalVelocity);
    }
    const auto blockSize = static_cast<std::uint32_t>(bytes.size() - blockStart);
    for (std::size_t offset = 0; offset < sizeof(std::uint32_t); ++offset) {
        bytes[blockStart + 4U + offset] =
            static_cast<std::uint8_t>(blockSize >> (offset * 8U));
    }
}

void readDropBlock(std::span<const std::uint8_t> payload, std::size_t& cursor,
                   std::vector<PersistentItemDrop>& drops,
                   std::vector<PersistentFallingBlock>& falling) {
    const std::size_t blockStart = cursor;
    if (blockStart + 12U > payload.size()) {
        throw std::runtime_error("world.dat drop block is truncated");
    }
    const auto tag = readInteger<std::uint32_t>(payload, cursor);
    if (tag != kDropBlockTag) {
        throw std::runtime_error("world.dat has an invalid drop block");
    }
    const auto blockSize = readInteger<std::uint32_t>(payload, cursor);
    if (blockSize < 12U || static_cast<std::size_t>(blockSize) > payload.size() - blockStart) {
        throw std::runtime_error("world.dat drop block is malformed");
    }
    const auto blockVersion = readInteger<std::uint16_t>(payload, cursor);
    if (blockVersion > kDropBlockVersion) {
        cursor = blockStart + blockSize;
        return;
    }
    const std::size_t blockEnd = blockStart + blockSize;

    const auto itemCount = readInteger<std::uint16_t>(payload, cursor);
    std::vector<const gameplay::Item*> items;
    items.reserve(itemCount);
    for (std::uint16_t index = 0; index < itemCount; ++index) {
        const auto name = readString(payload, cursor);
        items.push_back(name.empty() ? nullptr : gameplay::itemFromIdentifier(name));
    }
    const auto blockCount = readInteger<std::uint16_t>(payload, cursor);
    std::vector<world::Block> blocks;
    blocks.reserve(blockCount);
    for (std::uint16_t index = 0; index < blockCount; ++index) {
        const auto name = readString(payload, cursor);
        blocks.push_back(blockByName(name).value_or(world::Block::Air));
    }

    const auto dropCount = readInteger<std::uint32_t>(payload, cursor);
    drops.reserve(static_cast<std::size_t>(dropCount));
    for (std::uint32_t index = 0; index < dropCount; ++index) {
        if (cursor >= blockEnd) {
            throw std::runtime_error("world.dat drop block is truncated");
        }
        const auto itemIndex = readInteger<std::uint16_t>(payload, cursor);
        const auto blockIndex = readInteger<std::uint16_t>(payload, cursor);
        if (itemIndex >= items.size() || blockIndex >= blocks.size()) {
            throw std::runtime_error("world.dat drop block references an unknown palette entry");
        }
        PersistentItemDrop drop;
        drop.stack.item = items[itemIndex];
        drop.stack.block = blocks[blockIndex];
        drop.stack.count = readInteger<std::uint8_t>(payload, cursor);
        drop.x = readFloat(payload, cursor);
        drop.y = readFloat(payload, cursor);
        drop.z = readFloat(payload, cursor);
        drop.vx = readFloat(payload, cursor);
        drop.vy = readFloat(payload, cursor);
        drop.vz = readFloat(payload, cursor);
        drop.ageTicks = readInteger<std::uint32_t>(payload, cursor);
        if (!(drop.y >= -64.0F && drop.y <= 384.0F)) {
            throw std::runtime_error("world.dat drop block has an invalid position");
        }
        // An empty stack would come back as an invisible, unpickable entity.
        if (!drop.stack.empty()) {
            drops.push_back(drop);
        }
    }
    const auto fallingCount = readInteger<std::uint32_t>(payload, cursor);
    falling.reserve(static_cast<std::size_t>(fallingCount));
    for (std::uint32_t index = 0; index < fallingCount; ++index) {
        if (cursor >= blockEnd) {
            throw std::runtime_error("world.dat drop block is truncated");
        }
        const auto blockIndex = readInteger<std::uint16_t>(payload, cursor);
        if (blockIndex >= blocks.size()) {
            throw std::runtime_error("world.dat drop block references an unknown palette entry");
        }
        PersistentFallingBlock entity;
        entity.block = blocks[blockIndex];
        entity.x = readFloat(payload, cursor);
        entity.y = readFloat(payload, cursor);
        entity.z = readFloat(payload, cursor);
        entity.verticalVelocity = readFloat(payload, cursor);
        if (!(entity.y >= -64.0F && entity.y <= 384.0F)) {
            throw std::runtime_error("world.dat drop block has an invalid position");
        }
        if (entity.block != world::Block::Air) {
            falling.push_back(entity);
        }
    }
    if (cursor != blockEnd) {
        throw std::runtime_error("world.dat drop block has trailing data");
    }
}

// The CLOCK block is the self-describing region format 13 appends after the
// entity block, mirroring the GameRules framing:
//
//   u32 blockTag          // 'C','L','O','K'
//   u32 blockSizeBytes    // whole block length incl. this field
//   u16 blockVersion      // 1
//   u64 serverTick        // the world's own tick, reachable by no gamerule
//   u16 clockCount
//   clocks[]:
//     u64 totalTicks
//     f32 partialTick
//     f32 rate
//     u8  paused
//
// Clocks are written positionally rather than by name: ClockId is a fixed enum
// (26.1's WORLD_CLOCK registry has no data-driven entries either), and a build
// that grows the enum reads the clocks it recognises and defaults the rest,
// while an older build skips the whole block on version.
constexpr std::uint32_t kClockBlockTag =
    'C' | ('L' << 8) | ('O' << 16) | ('K' << 24);
constexpr std::uint16_t kClockBlockVersion = 1U;

void appendClockBlock(std::vector<std::uint8_t>& bytes, const SaveGame& game) {
    const std::size_t blockStart = bytes.size();
    appendInteger(bytes, kClockBlockTag);
    appendInteger(bytes, 0U);  // blockSizeBytes, patched after the fields
    appendInteger(bytes, kClockBlockVersion);
    appendInteger(bytes, game.serverTick);
    appendInteger(bytes, static_cast<std::uint16_t>(game.clocks.size()));
    for (const auto& clock : game.clocks) {
        appendInteger(bytes, clock.totalTicks);
        appendFloat(bytes, clock.partialTick);
        appendFloat(bytes, clock.rate);
        appendInteger(bytes, static_cast<std::uint8_t>(clock.paused ? 1U : 0U));
    }
    const auto blockSize = static_cast<std::uint32_t>(bytes.size() - blockStart);
    for (std::size_t offset = 0; offset < sizeof(std::uint32_t); ++offset) {
        bytes[blockStart + 4U + offset] =
            static_cast<std::uint8_t>(blockSize >> (offset * 8U));
    }
}

void readClockBlock(std::span<const std::uint8_t> payload, std::size_t& cursor,
                    SaveGame& game) {
    const std::size_t blockStart = cursor;
    if (blockStart + 12U > payload.size()) {
        throw std::runtime_error("world.dat clock block is truncated");
    }
    const auto tag = readInteger<std::uint32_t>(payload, cursor);
    if (tag != kClockBlockTag) {
        throw std::runtime_error("world.dat has an invalid clock block");
    }
    const auto blockSize = readInteger<std::uint32_t>(payload, cursor);
    if (blockSize < 12U || static_cast<std::size_t>(blockSize) > payload.size() - blockStart) {
        throw std::runtime_error("world.dat clock block is malformed");
    }
    const auto blockVersion = readInteger<std::uint16_t>(payload, cursor);
    if (blockVersion > kClockBlockVersion) {
        cursor = blockStart + blockSize;
        return;
    }
    const std::size_t blockEnd = blockStart + blockSize;
    game.serverTick = readInteger<std::uint64_t>(payload, cursor);
    const auto clockCount = readInteger<std::uint16_t>(payload, cursor);
    for (std::uint16_t index = 0; index < clockCount; ++index) {
        if (blockEnd - cursor < 17U) {
            throw std::runtime_error("world.dat clock block is truncated");
        }
        world::ClockState clock;
        clock.totalTicks = readInteger<std::uint64_t>(payload, cursor);
        clock.partialTick = readFloat(payload, cursor);
        clock.rate = readFloat(payload, cursor);
        clock.paused = readInteger<std::uint8_t>(payload, cursor) != 0U;
        // A save written by a build with more clocks than this one knows about
        // keeps its extra entries on disk but drops them here.
        if (index < game.clocks.size()) {
            game.clocks[index] = clock;
        }
    }
    if (cursor != blockEnd) {
        throw std::runtime_error("world.dat clock block has trailing data");
    }
}

// --- Format 17: the owner-driven blocks -------------------------------------
//
// Everything a format-16 save wrote at a fixed header offset — the player, the
// block edits, the chests, the furnaces — now lives in a block of its own, and
// the reader dispatches on the tag instead of counting bytes from the start of
// the file. What that buys: a new state owner is one table entry rather than a
// field, a positional read and a format bump; blocks may be written in any
// order; and a block this build does not recognise is skipped by its own size
// instead of desynchronising everything after it.

// What a writer needs: the game and the two palettes every record indexes into.
struct SaveWriteContext final {
    const SaveGame& game;
    BlockPalette& blocks;
    ItemPalette& items;
};

// What a reader needs. `stacks` resolves a palette-indexed stack; the legacy
// loader has its own version of that, which is why it is a callback rather than
// a direct palette lookup.
struct SaveReadContext final {
    SaveGame& game;
    std::span<const world::Block> blocks;
    std::span<const gameplay::Item* const> items;
};

[[nodiscard]] world::Block resolveBlock(const SaveReadContext& context, std::uint16_t index) {
    if (index >= context.blocks.size()) {
        throw std::runtime_error("world.dat references a block outside the palette");
    }
    return context.blocks[index];
}

[[nodiscard]] const gameplay::Item* resolveItem(
    const SaveReadContext& context,
    std::uint16_t index) {
    if (index >= context.items.size()) {
        throw std::runtime_error("world.dat references an item outside the palette");
    }
    return context.items[index];
}

void appendStack(std::vector<std::uint8_t>& bytes, const SaveWriteContext& context,
                 const gameplay::ItemStack& stack) {
    appendInteger(bytes, context.blocks.indexOf(world::blockId(stack.block)));
    appendInteger(bytes, stack.count);
    appendInteger(bytes, context.items.indexOf(stack.item));
    appendInteger(bytes, stack.damage);
}

void readStackRecord(std::span<const std::uint8_t> payload, std::size_t& cursor,
                     const SaveReadContext& context, gameplay::ItemStack& stack) {
    stack.block = resolveBlock(context, readInteger<std::uint16_t>(payload, cursor));
    stack.count = readInteger<std::uint8_t>(payload, cursor);
    stack.item = resolveItem(context, readInteger<std::uint16_t>(payload, cursor));
    stack.damage = readInteger<std::uint16_t>(payload, cursor);
    if (stack.damage > gameplay::itemMaximumDamage(stack)) {
        throw std::runtime_error("world.dat contains an over-damaged item");
    }
}

// A slot array written sparsely: only the occupied slots travel, each behind its
// own index. A chest holding three items costs 25 bytes instead of the 189 a
// dense array of 27 stacks would, and the common case in a played world is a
// chest that is mostly air.
template <typename Slots>
void appendSlots(std::vector<std::uint8_t>& bytes, const SaveWriteContext& context,
                 const Slots& slots) {
    std::uint16_t used = 0U;
    for (const auto& stack : slots) {
        if (!stack.empty()) {
            ++used;
        }
    }
    appendInteger(bytes, used);
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (slots[index].empty()) {
            continue;
        }
        appendInteger(bytes, static_cast<std::uint16_t>(index));
        appendStack(bytes, context, slots[index]);
    }
}

template <typename Slots>
void readSlots(std::span<const std::uint8_t> payload, std::size_t& cursor,
               const SaveReadContext& context, Slots& slots) {
    slots = Slots{};
    const auto used = readInteger<std::uint16_t>(payload, cursor);
    if (used > slots.size()) {
        throw std::runtime_error("world.dat container holds more slots than it has");
    }
    for (std::uint16_t entry = 0; entry < used; ++entry) {
        const auto index = readInteger<std::uint16_t>(payload, cursor);
        if (index >= slots.size()) {
            throw std::runtime_error("world.dat container references a slot it has not got");
        }
        readStackRecord(payload, cursor, context, slots[index]);
    }
}

// The world's own settings: what a second player joining would share, as
// opposed to anything the player carries.
constexpr std::uint32_t kWorldBlockTag = blockTag("WRLD");
constexpr std::uint16_t kWorldBlockVersion = 1U;

void appendWorldBlock(std::vector<std::uint8_t>& bytes, const SaveWriteContext& context) {
    const SaveBlockWriter block{bytes, kWorldBlockTag, kWorldBlockVersion};
    appendInteger(bytes, static_cast<std::uint8_t>(context.game.gameMode));
    appendInteger(bytes, static_cast<std::uint8_t>(context.game.difficulty));
}

void readWorldBlock(std::span<const std::uint8_t> payload, std::size_t& cursor,
                    const SaveBlockHeader& header, SaveReadContext& context) {
    const auto mode = readInteger<std::uint8_t>(payload, cursor);
    if (mode > static_cast<std::uint8_t>(gameplay::GameMode::Creative)) {
        throw std::runtime_error("world.dat contains an invalid game mode");
    }
    context.game.gameMode = static_cast<gameplay::GameMode>(mode);
    const auto difficulty = readInteger<std::uint8_t>(payload, cursor);
    if (difficulty >= gameplay::kDifficultyCount) {
        throw std::runtime_error("world.dat contains an invalid difficulty");
    }
    context.game.difficulty = static_cast<gameplay::Difficulty>(difficulty);
    cursor = header.end;
}

// The player: where they stand, what state their body is in, and what they
// carry. The spawn point stays in its own SPWN block, which predates this one.
constexpr std::uint32_t kPlayerBlockTag = blockTag("PLYR");
constexpr std::uint16_t kPlayerBlockVersion = 1U;

void appendPlayerBlock(std::vector<std::uint8_t>& bytes, const SaveWriteContext& context) {
    const auto& game = context.game;
    const SaveBlockWriter block{bytes, kPlayerBlockTag, kPlayerBlockVersion};
    appendInteger(bytes, static_cast<std::uint8_t>(game.hasPlayerPosition ? 1U : 0U));
    appendFloat(bytes, game.playerX);
    appendFloat(bytes, game.playerY);
    appendFloat(bytes, game.playerZ);
    appendInteger(bytes, static_cast<std::uint8_t>(game.selectedHotbarSlot));
    appendFloat(bytes, game.playerHealth);
    appendInteger(bytes, game.playerFoodLevel);
    appendFloat(bytes, game.playerSaturation);
    appendInteger(bytes, game.playerAirTicks);
    appendSlots(bytes, context, game.inventory);
}

void readPlayerBlock(std::span<const std::uint8_t> payload, std::size_t& cursor,
                     const SaveBlockHeader& header, SaveReadContext& context) {
    auto& game = context.game;
    game.hasPlayerPosition = readInteger<std::uint8_t>(payload, cursor) != 0U;
    game.playerX = readFloat(payload, cursor);
    game.playerY = readFloat(payload, cursor);
    game.playerZ = readFloat(payload, cursor);
    game.selectedHotbarSlot = readInteger<std::uint8_t>(payload, cursor);
    if (game.selectedHotbarSlot >= gameplay::Inventory::kHotbarSize) {
        throw std::runtime_error("world.dat contains an invalid hotbar slot");
    }
    game.playerHealth = readFloat(payload, cursor);
    game.playerFoodLevel = readInteger<std::int32_t>(payload, cursor);
    game.playerSaturation = readFloat(payload, cursor);
    game.playerAirTicks = readInteger<std::int32_t>(payload, cursor);
    if (!(game.playerHealth >= 0.0F &&
          game.playerHealth <= gameplay::PlayerVitals::kMaximumHealth) ||
        game.playerFoodLevel < 0 ||
        game.playerFoodLevel > gameplay::PlayerVitals::kMaximumFood ||
        !(game.playerSaturation >= 0.0F &&
          game.playerSaturation <= gameplay::PlayerVitals::kMaximumFood) ||
        game.playerAirTicks < -20 ||
        game.playerAirTicks > gameplay::PlayerVitals::kMaximumAirTicks) {
        throw std::runtime_error("world.dat contains invalid player vitals");
    }
    readSlots(payload, cursor, context, game.inventory);
    cursor = header.end;
}

// The block edits, grouped by the chunk that owns them.
//
// Format 16 wrote three absolute i32 coordinates plus a palette index and
// three loose bytes per edit: 17 bytes each, and the edit list is by far the
// largest thing in a played world's save. Grouping by chunk lets the two
// horizontal coordinates collapse into one packed byte — the 0-15 offsets
// inside the chunk — and the fluid level, orientation and lit flag pack into
// one more. Six bytes, measured at 6.33 including the per-chunk headers: a
// 63% cut on the section that dominates both file size and load time.
//
// Version 2 replaces the block index and that packed byte with one index into
// a state palette local to this block. Two things come out of it:
//
//   - The packed byte was full. Four bits of fluid level, three of orientation
//     and one of lit is the whole byte, so the *next* property a block gained
//     had nowhere to go — and the three bits of orientation were already being
//     shared with a crop's age.
//   - The record shrinks to five bytes, because a played world has tens of
//     distinct states and millions of edits. The property names are written
//     once each in the palette, not once per edit.
//
// This is also what "the chunk owns its edits" means concretely: the grouping
// is the on-disk shape a per-chunk region file would want, so the day chunks
// get their own files this block splits along lines that already exist.
constexpr std::uint32_t kChunkBlockTag = blockTag("CHNK");
constexpr std::uint16_t kChunkBlockVersion = 2U;

// One state palette entry: the block's identifier index plus every property the
// block declares, by name. Names rather than digits is the whole point — a
// reader skips a property it has never heard of and defaults one it was not
// told about, so neither adding nor removing a property breaks a world.
void appendStatePaletteEntry(std::vector<std::uint8_t>& bytes, BlockPalette& blocks,
                             world::BlockState state) {
    appendInteger(bytes, blocks.indexOf(world::blockId(state.block())));
    const auto& schema =
        world::kBlockRegistry[static_cast<std::size_t>(state.block())].states;
    appendInteger(bytes, static_cast<std::uint8_t>(schema.size()));
    for (std::size_t index = 0; index < schema.size(); ++index) {
        const auto property = schema.axis(index).property;
        appendString(bytes, world::statePropertyName(property));
        appendInteger(bytes, state.value(property));
    }
}

[[nodiscard]] world::BlockState readStatePaletteEntry(std::span<const std::uint8_t> payload,
                                                      std::size_t& cursor,
                                                      const SaveReadContext& context) {
    auto state = world::BlockState{resolveBlock(context, readInteger<std::uint16_t>(payload, cursor))};
    const auto propertyCount = readInteger<std::uint8_t>(payload, cursor);
    for (std::uint8_t index = 0; index < propertyCount; ++index) {
        const auto name = readString(payload, cursor);
        const auto value = readInteger<std::uint8_t>(payload, cursor);
        const auto property = world::statePropertyFromName(name);
        if (property == world::StateProperty::Count) {
            continue;  // a property this build has no notion of
        }
        // A value past the property's range clamps to the default rather than
        // refusing the world: the block may have narrowed the property since.
        state = state.with(property, value);
    }
    return state;
}

[[nodiscard]] constexpr std::int32_t chunkOf(std::int32_t coordinate, std::int32_t span) {
    // Floor division: -1 belongs to chunk -1, not chunk 0.
    return coordinate >= 0 ? coordinate / span : -(((-coordinate) + span - 1) / span);
}

[[nodiscard]] constexpr std::uint8_t localOf(std::int32_t coordinate, std::int32_t span) {
    const auto remainder = coordinate - chunkOf(coordinate, span) * span;
    return static_cast<std::uint8_t>(remainder);
}

void appendChunkBlock(std::vector<std::uint8_t>& bytes, const SaveWriteContext& context) {
    const auto& edits = context.game.edits;
    const SaveBlockWriter block{bytes, kChunkBlockTag, kChunkBlockVersion};

    // The grouping key is computed once per edit and sorted alongside its index,
    // rather than recomputed inside the comparator: a comparator that derives the
    // key does two floor divisions per comparison, which is n log n of them
    // instead of n. The index is part of the sort key, so equal chunks keep their
    // original order without the temporary buffer a stable_sort allocates — and
    // that order matters, because two edits on the same cell mean the later one
    // is the cell's final state.
    struct KeyedEdit final {
        std::uint64_t key;
        std::uint32_t index;
    };
    std::vector<KeyedEdit> order;
    order.reserve(edits.size());
    for (std::uint32_t index = 0; index < edits.size(); ++index) {
        const auto& edit = edits[index];
        const auto chunkX = chunkOf(edit.x, world::kChunkWidth);
        const auto chunkZ = chunkOf(edit.z, world::kChunkDepth);
        // Biased into unsigned so negative chunk coordinates sort below positive
        // ones instead of above them.
        const auto biasedX = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(chunkX) + (std::int64_t{1} << 31));
        const auto biasedZ = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(chunkZ) + (std::int64_t{1} << 31));
        order.push_back({(biasedX << 32U) | biasedZ, index});
    }
    std::sort(order.begin(), order.end(), [](const KeyedEdit& left, const KeyedEdit& right) {
        return left.key != right.key ? left.key < right.key : left.index < right.index;
    });

    const std::size_t countOffset = bytes.size();
    appendInteger(bytes, static_cast<std::uint32_t>(0U));  // chunkCount, patched below
    // The total up front so the reader allocates the edit vector once. Reserving
    // per chunk instead is quadratic: a world spread over four thousand chunks
    // reallocates and copies the whole list four thousand times, which measured
    // as a 6x slower load than the write it mirrors.
    appendInteger(bytes, static_cast<std::uint32_t>(edits.size()));

    // The state palette, gathered in the sorted order the records below will
    // reference it in, and written before them.
    StatePalette states;
    for (const auto& entry : order) {
        static_cast<void>(states.indexOf(edits[entry.index].state.rawId()));
    }
    appendInteger(bytes, static_cast<std::uint16_t>(states.entries().size()));
    for (const auto rawId : states.entries()) {
        appendStatePaletteEntry(bytes, context.blocks, world::BlockState::fromRawId(rawId));
    }
    std::uint32_t chunkCount = 0U;
    std::size_t position = 0U;
    while (position < order.size()) {
        const auto key = order[position].key;
        std::size_t run = position;
        while (run < order.size() && order[run].key == key) {
            ++run;
        }
        const auto& first = edits[order[position].index];
        appendInteger(bytes, chunkOf(first.x, world::kChunkWidth));
        appendInteger(bytes, chunkOf(first.z, world::kChunkDepth));
        appendInteger(bytes, static_cast<std::uint32_t>(run - position));
        for (std::size_t index = position; index < run; ++index) {
            const auto& edit = edits[order[index].index];
            const auto packedXZ = static_cast<std::uint8_t>(
                localOf(edit.x, world::kChunkWidth) |
                (localOf(edit.z, world::kChunkDepth) << 4U));
            appendInteger(bytes, packedXZ);
            // i16 rather than the u8 a 256-block world needs, so the block
            // survives milestone 5 raising the world to -64..319 without a
            // second format migration.
            appendInteger(bytes, static_cast<std::int16_t>(edit.y));
            appendInteger(bytes, states.indexOf(edit.state.rawId()));
        }
        ++chunkCount;
        position = run;
    }
    for (std::size_t offset = 0; offset < sizeof(std::uint32_t); ++offset) {
        bytes[countOffset + offset] = static_cast<std::uint8_t>(chunkCount >> (offset * 8U));
    }
}

void readChunkBlock(std::span<const std::uint8_t> payload, std::size_t& cursor,
                    const SaveBlockHeader& header, SaveReadContext& context) {
    auto& edits = context.game.edits;
    const auto chunkCount = readInteger<std::uint32_t>(payload, cursor);
    const auto totalEdits = readInteger<std::uint32_t>(payload, cursor);
    if (totalEdits > kMaximumEdits) {
        throw std::runtime_error("world.dat edit count is unreasonable");
    }
    // One allocation for the whole list; the per-chunk counts below are checked
    // against the block's own length, so a lying total cannot overrun anything.
    edits.reserve(edits.size() + totalEdits);
    // Version 2 names every distinct state once, up front; version 1 spelled a
    // block index and a packed byte into every record.
    std::vector<world::BlockState> statePalette;
    if (header.version >= 2U) {
        const auto paletteCount = readInteger<std::uint16_t>(payload, cursor);
        statePalette.reserve(paletteCount);
        for (std::uint16_t index = 0; index < paletteCount; ++index) {
            statePalette.push_back(readStatePaletteEntry(payload, cursor, context));
        }
        if (statePalette.empty()) {
            throw std::runtime_error("world.dat has an empty state palette");
        }
    }
    const std::size_t recordBytes =
        header.version >= 2U ? kEditRecordBytes : kLegacyEditRecordBytes;
    for (std::uint32_t index = 0; index < chunkCount; ++index) {
        const auto chunkX = readInteger<std::int32_t>(payload, cursor);
        const auto chunkZ = readInteger<std::int32_t>(payload, cursor);
        const auto editCount = readInteger<std::uint32_t>(payload, cursor);
        if (edits.size() + editCount > kMaximumEdits) {
            throw std::runtime_error("world.dat edit count is unreasonable");
        }
        // Checked before reserving so a corrupt count cannot ask for an
        // enormous allocation.
        if (static_cast<std::uint64_t>(editCount) * recordBytes > header.end - cursor) {
            throw std::runtime_error("world.dat chunk block is truncated");
        }
        for (std::uint32_t entry = 0; entry < editCount; ++entry) {
            const auto packedXZ = readInteger<std::uint8_t>(payload, cursor);
            world::PersistentBlockEdit edit;
            edit.x = chunkX * world::kChunkWidth + static_cast<std::int32_t>(packedXZ & 0x0FU);
            edit.z = chunkZ * world::kChunkDepth +
                     static_cast<std::int32_t>((packedXZ >> 4U) & 0x0FU);
            edit.y = readInteger<std::int16_t>(payload, cursor);
            if (header.version >= 2U) {
                const auto stateIndex = readInteger<std::uint16_t>(payload, cursor);
                if (stateIndex >= statePalette.size()) {
                    throw std::runtime_error(
                        "world.dat edit references an unknown state palette entry");
                }
                edit.state = statePalette[stateIndex];
            } else {
                const auto block =
                    resolveBlock(context, readInteger<std::uint16_t>(payload, cursor));
                const auto packedState = readInteger<std::uint8_t>(payload, cursor);
                const auto fluidLevel = static_cast<std::uint8_t>(packedState & 0x0FU);
                if (fluidLevel > 8U) {
                    throw std::runtime_error("world.dat contains an invalid block edit");
                }
                // Crops and farmland reused the orientation slot for their
                // state, so the full 0-7 range was legitimate there.
                edit.state = legacyBlockState(
                    block, static_cast<std::uint8_t>((packedState >> 4U) & 0x07U), fluidLevel,
                    (packedState & 0x80U) != 0U);
            }
            if (!world::isWorldYInRange(edit.y)) {
                throw std::runtime_error("world.dat contains an invalid block edit");
            }
            edits.push_back(edit);
        }
    }
    cursor = header.end;
}

// ---------------------------------------------------------------------------
// M-3 region files (C5: chunks own their edits and their creatures).
//
// The CHNK block already groups every edit by chunk; this is the same grouping
// written to files of its own. A region is a 32x32 grid of chunks, addressed by
// floor division of the chunk coordinates the way Java's r.*.mca are, so a
// world spread over thousands of chunks does not rewrite one giant world.dat on
// every save — and the day a chunk unloads, its edits and its creatures are
// written together in one small file.
//
//   region/r.<rx>.<rz>.cache:
//     u64 magic          "MCRBREG"
//     u32 formatVersion  // 1
//     i32 regionX, regionZ
//     u32 chunkCount
//     u16 statePaletteCount
//     statePalette[]:    { u16 idLen + blockId + u8 propCount + [u16 nameLen + name + u8 value]* }
//     u16 speciesCount
//     species[]:         { u16 nameLen + name }
//     chunks[chunkCount]:
//       u32 tag "CCNK" + u32 size + u16 ver  // self-framed, unknown versions skip
//         i32 cx, i32 cz
//         u32 editCount + edits[]:   { u8 packedXZ + i16 y + u16 stateIndex }
//         u32 entityCount + entities[]: { u16 speciesIndex + f32 x,y,z,yaw + f32 vx,vy,vz
//                                       + f32 health + i32 angerTicks + u32 ageTicks
//                                       + u32 rngState + i32 fireTicks (ver >= 2) + u8 flags
//                                       + effects (ver >= 3): u8 count,
//                                         [u16 nameLen + name + i32 duration + u8 amplifier]*
//                                       + i32 age + i32 loveTicks (ver >= 4) }
//     u64 checksum (FNV-1a over everything above it)
//
// The state and species palettes are region-local and self-contained (block
// identifiers inline rather than world.dat palette indices) so a region can be
// read on its own, without the world.dat palettes, the way the Java import's
// CCNK cache will need.
constexpr std::array<std::uint8_t, 8> kRegionMagic{'M', 'C', 'R', 'B', 'R', 'E', 'G', 0x00};
constexpr std::uint32_t kRegionFileVersion = 1U;
constexpr std::uint32_t kRegionChunkTag = blockTag("CCNK");
// Version 2 appends fireTicks to each entity record; version 3 appends the
// active MobEffects after the flags byte; version 4 appends AgeableMob age/love
// (see the ENTITY block). An older region omits the newer fields, which read
// back as their defaults, migrating cleanly.
constexpr std::uint16_t kRegionChunkVersion = 4U;
constexpr std::uint32_t kRegionWidth = 32U;  // chunks per region side

// Floor division of a chunk coordinate by the region width, exactly like the
// chunk floor division the CHNK block uses for world coordinates.
[[nodiscard]] constexpr std::int32_t regionOf(std::int32_t chunkCoordinate) {
    return chunkOf(chunkCoordinate, static_cast<std::int32_t>(kRegionWidth));
}

// One chunk's share of a region file: its edits and the creatures inside it.
struct RegionChunkData final {
    std::int32_t chunkX = 0;
    std::int32_t chunkZ = 0;
    std::vector<world::PersistentBlockEdit> edits;
    std::vector<PersistentEntity> entities;
};

// One region file's content, as gathered before writing or read back from disk.
struct RegionData final {
    std::int32_t regionX = 0;
    std::int32_t regionZ = 0;
    std::vector<RegionChunkData> chunks;
};

[[nodiscard]] std::string regionFileName(std::int32_t regionX, std::int32_t regionZ) {
    return "r." + std::to_string(regionX) + "." + std::to_string(regionZ) + ".cache";
}

// DIM-4: the per-dimension subdirectory inside a world folder, mirroring vanilla
// 1.16.1's layout so the JC import (JC3) finds each dimension where a vanilla
// world keeps it — the Overworld at the world root (no subfolder, so an existing
// flat world is unchanged), the Nether under DIM-1 and the End under DIM1. An
// empty return means "the world root itself" (Overworld). Returning the vanilla
// folder names here, not a rebedrock-specific spelling, is what keeps the compat
// layer free of a new deviation.
[[nodiscard]] std::filesystem::path dimensionSubdirectory(world::DimensionId dimension) {
    switch (dimension) {
    case world::DimensionId::Overworld:
        return {};  // the world root itself
    case world::DimensionId::Nether:
        return "DIM-1";
    case world::DimensionId::End:
        return "DIM1";
    case world::DimensionId::Count:
        break;
    }
    return {};
}

// The region directory for a dimension: the world folder, plus the dimension
// subdirectory (none for the Overworld), plus "region". Overworld resolves to the
// historical `<world>/region`, so old saves are byte-compatible.
[[nodiscard]] std::filesystem::path regionDirectoryFor(const std::filesystem::path& worldDirectory,
                                                       world::DimensionId dimension) {
    const auto subdirectory = dimensionSubdirectory(dimension);
    return subdirectory.empty() ? worldDirectory / "region"
                                : worldDirectory / subdirectory / "region";
}

// "r.<rx>.<rz>.cache" back into its coordinates; anything that does not match
// the shape is not ours to touch (a foreign file in the region directory).
[[nodiscard]] std::optional<std::pair<std::int32_t, std::int32_t>> parseRegionFileName(
    std::string_view name) {
    if (!name.starts_with("r.") || !name.ends_with(".cache")) {
        return std::nullopt;
    }
    const std::string_view body =
        name.substr(2U, name.size() - 2U - 6U);  // strip "r." and ".cache"
    const auto dot = body.rfind('.');
    if (dot == std::string_view::npos || dot == 0U || dot + 1U == body.size()) {
        return std::nullopt;
    }
    const std::string_view xText = body.substr(0U, dot);
    const std::string_view zText = body.substr(dot + 1U);
    std::int32_t regionX = 0;
    std::int32_t regionZ = 0;
    if (std::from_chars(xText.data(), xText.data() + xText.size(), regionX).ec != std::errc{} ||
        std::from_chars(zText.data(), zText.data() + zText.size(), regionZ).ec != std::errc{}) {
        return std::nullopt;
    }
    return std::pair<std::int32_t, std::int32_t>{regionX, regionZ};
}

// A state palette entry self-contained in the region file: the block identifier
// is written inline rather than through world.dat's block palette, so a region
// needs nothing else to decode. Properties are named exactly like CHNK v2's.
void appendRegionStatePaletteEntry(std::vector<std::uint8_t>& bytes, world::BlockState state) {
    // A block this build's registry does not know (a removed mod/datapack block)
    // is written back exactly as it came in — the original identifier and every
    // property byte — so unloading it is lossless and re-adding the content
    // restores the real block. See persistence/UnknownBlockTable.hpp.
    if (unknownBlockTable().isUnknown(state)) {
        const UnknownBlockState record = unknownBlockTable().record(state);
        appendString(bytes, record.name);
        appendInteger(bytes, static_cast<std::uint8_t>(record.properties.size()));
        for (const auto& property : record.properties) {
            appendString(bytes, property.name);
            appendInteger(bytes, property.value);
        }
        return;
    }
    const auto stateId = world::blockIdOfState(state.rawId());
    appendString(bytes, world::blockRegistry().identifier(stateId).toString());
    const auto& schema =
        world::kBlockRegistry[static_cast<std::size_t>(state.block())].states;
    appendInteger(bytes, static_cast<std::uint8_t>(schema.size()));
    for (std::size_t index = 0; index < schema.size(); ++index) {
        const auto property = schema.axis(index).property;
        appendString(bytes, world::statePropertyName(property));
        appendInteger(bytes, state.value(property));
    }
}

[[nodiscard]] world::BlockState readRegionStatePaletteEntry(
    std::span<const std::uint8_t> payload,
    std::size_t& cursor) {
    const auto name = readString(payload, cursor);
    // Read the whole property blob first: for a known block the values are
    // applied below, and for an unknown one the raw pairs are kept verbatim so the
    // block can be written back unchanged.
    const auto propertyCount = readInteger<std::uint8_t>(payload, cursor);
    std::vector<UnknownStateProperty> blob;
    blob.reserve(propertyCount);
    for (std::uint8_t index = 0; index < propertyCount; ++index) {
        auto propertyName = readString(payload, cursor);
        const auto value = readInteger<std::uint8_t>(payload, cursor);
        blob.push_back({std::move(propertyName), value});
    }
    const std::optional<world::Block> known = blockByName(name);
    if (!known.has_value()) {
        // Content this build lacks: keep it as a placeholder rather than dropping
        // the cell to air, which is what value_or(Air) used to do.
        return unknownBlockTable().intern(std::move(name), std::move(blob));
    }
    auto state = world::BlockState{*known};
    for (const auto& property : blob) {
        const auto resolved = world::statePropertyFromName(property.name);
        if (resolved == world::StateProperty::Count) {
            continue;  // a property this build has no notion of
        }
        state = state.with(resolved, property.value);
    }
    return state;
}

[[nodiscard]] RegionChunkData& chunkInRegion(RegionData& region, std::int32_t chunkX,
                                             std::int32_t chunkZ) {
    for (auto& chunk : region.chunks) {
        if (chunk.chunkX == chunkX && chunk.chunkZ == chunkZ) {
            return chunk;
        }
    }
    region.chunks.push_back({chunkX, chunkZ, {}, {}});
    return region.chunks.back();
}

// Gathers the region files a save needs: every edit and every creature bucketed
// by its chunk, then by that chunk's region. Chunks in the same region share one
// state palette and one species palette, like CHNK and ENTY did per file.
[[nodiscard]] std::map<std::pair<std::int32_t, std::int32_t>, RegionData> gatherRegions(
    const SaveGame& game) {
    std::map<std::pair<std::int32_t, std::int32_t>, RegionData> regions;
    for (const auto& edit : game.edits) {
        if (!world::isWorldYInRange(edit.y)) {
            continue;
        }
        const auto chunkX = chunkOf(edit.x, world::kChunkWidth);
        const auto chunkZ = chunkOf(edit.z, world::kChunkDepth);
        const auto regionX = regionOf(chunkX);
        const auto regionZ = regionOf(chunkZ);
        auto& region = regions[{regionX, regionZ}];
        region.regionX = regionX;
        region.regionZ = regionZ;
        chunkInRegion(region, chunkX, chunkZ).edits.push_back(edit);
    }
    for (const auto& entity : game.entities) {
        // A creature saved outside the world is a corrupt record.
        if (!(entity.y >= -64.0F && entity.y <= 384.0F)) {
            continue;
        }
        const auto chunkX = chunkOf(static_cast<std::int32_t>(std::floor(entity.x)),
                                    world::kChunkWidth);
        const auto chunkZ = chunkOf(static_cast<std::int32_t>(std::floor(entity.z)),
                                    world::kChunkDepth);
        const auto regionX = regionOf(chunkX);
        const auto regionZ = regionOf(chunkZ);
        auto& region = regions[{regionX, regionZ}];
        region.regionX = regionX;
        region.regionZ = regionZ;
        chunkInRegion(region, chunkX, chunkZ).entities.push_back(entity);
    }
    return regions;
}

void appendRegionFile(std::vector<std::uint8_t>& bytes, const RegionData& region) {
    const std::size_t fileStart = bytes.size();
    bytes.insert(bytes.end(), kRegionMagic.begin(), kRegionMagic.end());
    appendInteger(bytes, kRegionFileVersion);
    appendInteger(bytes, region.regionX);
    appendInteger(bytes, region.regionZ);
    appendInteger(bytes, static_cast<std::uint32_t>(region.chunks.size()));

    // Region-wide state palette, gathered in the chunk order the records below
    // reference it in and written before them, exactly like CHNK v2.
    StatePalette states;
    for (const auto& chunk : region.chunks) {
        for (const auto& edit : chunk.edits) {
            static_cast<void>(states.indexOf(edit.state.rawId()));
        }
    }
    appendInteger(bytes, static_cast<std::uint16_t>(states.entries().size()));
    for (const auto rawId : states.entries()) {
        appendRegionStatePaletteEntry(bytes, world::BlockState::fromRawId(rawId));
    }

    // Region-wide species palette, same framing as the ENTITY block's.
    std::vector<std::string> species;
    std::unordered_map<std::string, std::uint16_t> speciesIndices;
    const auto speciesIndexOf = [&](const std::string& name) -> std::uint16_t {
        const auto existing = speciesIndices.find(name);
        if (existing != speciesIndices.end()) {
            return existing->second;
        }
        const auto index = static_cast<std::uint16_t>(species.size());
        species.push_back(name);
        speciesIndices.emplace(name, index);
        return index;
    };
    for (const auto& chunk : region.chunks) {
        for (const auto& entity : chunk.entities) {
            static_cast<void>(speciesIndexOf(entity.species));
        }
    }
    appendInteger(bytes, static_cast<std::uint16_t>(species.size()));
    for (const auto& name : species) {
        appendString(bytes, name);
    }

    for (const auto& chunk : region.chunks) {
        const SaveBlockWriter block{bytes, kRegionChunkTag, kRegionChunkVersion};
        appendInteger(bytes, chunk.chunkX);
        appendInteger(bytes, chunk.chunkZ);
        appendInteger(bytes, static_cast<std::uint32_t>(chunk.edits.size()));
        for (const auto& edit : chunk.edits) {
            const auto packedXZ = static_cast<std::uint8_t>(
                localOf(edit.x, world::kChunkWidth) |
                (localOf(edit.z, world::kChunkDepth) << 4U));
            appendInteger(bytes, packedXZ);
            appendInteger(bytes, static_cast<std::int16_t>(edit.y));
            appendInteger(bytes, states.indexOf(edit.state.rawId()));
        }
        appendInteger(bytes, static_cast<std::uint32_t>(chunk.entities.size()));
        for (const auto& entity : chunk.entities) {
            appendInteger(bytes, speciesIndexOf(entity.species));
            appendFloat(bytes, entity.x);
            appendFloat(bytes, entity.y);
            appendFloat(bytes, entity.z);
            appendFloat(bytes, entity.yaw);
            appendFloat(bytes, entity.vx);
            appendFloat(bytes, entity.vy);
            appendFloat(bytes, entity.vz);
            appendFloat(bytes, entity.health);
            appendInteger(bytes, entity.angerTicks);
            appendInteger(bytes, entity.ageTicks);
            appendInteger(bytes, entity.rngState);
            appendInteger(bytes, entity.fireTicks);  // version 2
            appendInteger(bytes, static_cast<std::uint8_t>(0U));  // flags, reserved
            appendEffectList(bytes, entity.effects);  // version 3
            appendInteger(bytes, entity.age);        // version 4
            appendInteger(bytes, entity.loveTicks);  // version 4
        }
    }
    appendInteger(
        bytes,
        checksum(std::span<const std::uint8_t>{
            bytes.data() + fileStart, bytes.size() - fileStart}));
}

// Reads one region file back into its chunk records. Throws on any structural
// problem; the caller decides a torn region is worth more than the world it sits
// in (it is not — see readRegionDirectory).
void readRegionFile(std::span<const std::uint8_t> bytes, RegionData& region) {
    if (bytes.size() < kRegionMagic.size() + sizeof(std::uint64_t) ||
        !std::equal(kRegionMagic.begin(), kRegionMagic.end(), bytes.begin())) {
        throw std::runtime_error("region file has an invalid header");
    }
    std::size_t checksumCursor = bytes.size() - sizeof(std::uint64_t);
    std::size_t checksumReadCursor = checksumCursor;
    const auto storedChecksum = readInteger<std::uint64_t>(bytes, checksumReadCursor);
    if (checksum(std::span<const std::uint8_t>{bytes}.first(checksumCursor)) != storedChecksum) {
        throw std::runtime_error("region file checksum mismatch");
    }
    const std::span<const std::uint8_t> payload{bytes.data(), checksumCursor};
    std::size_t cursor = kRegionMagic.size();
    const auto version = readInteger<std::uint32_t>(payload, cursor);
    if (version > kRegionFileVersion) {
        throw std::runtime_error("region file has a newer format version");
    }
    region.regionX = readInteger<std::int32_t>(payload, cursor);
    region.regionZ = readInteger<std::int32_t>(payload, cursor);
    const auto chunkCount = readInteger<std::uint32_t>(payload, cursor);
    if (chunkCount > kMaximumEdits) {
        throw std::runtime_error("region file has an unreasonable chunk count");
    }

    std::vector<world::BlockState> statePalette;
    const auto paletteCount = readInteger<std::uint16_t>(payload, cursor);
    statePalette.reserve(paletteCount);
    for (std::uint16_t index = 0; index < paletteCount; ++index) {
        statePalette.push_back(readRegionStatePaletteEntry(payload, cursor));
    }
    if (statePalette.empty()) {
        throw std::runtime_error("region file has an empty state palette");
    }

    std::vector<std::string> species;
    const auto speciesCount = readInteger<std::uint16_t>(payload, cursor);
    species.reserve(speciesCount);
    for (std::uint16_t index = 0; index < speciesCount; ++index) {
        species.push_back(readString(payload, cursor));
    }

    for (std::uint32_t index = 0; index < chunkCount; ++index) {
        std::size_t peek = cursor;
        const auto header = readBlockHeader(payload, peek, "region chunk");
        if (header.tag != kRegionChunkTag || header.version > kRegionChunkVersion) {
            // A layout a future build changed: its size is the point of the frame.
            cursor = header.end;
            continue;
        }
        cursor = peek;
        RegionChunkData chunk;
        chunk.chunkX = readInteger<std::int32_t>(payload, cursor);
        chunk.chunkZ = readInteger<std::int32_t>(payload, cursor);
        const auto editCount = readInteger<std::uint32_t>(payload, cursor);
        if (editCount > kMaximumEdits ||
            static_cast<std::uint64_t>(editCount) * kEditRecordBytes > header.end - cursor) {
            throw std::runtime_error("region file edit section is truncated");
        }
        chunk.edits.reserve(editCount);
        for (std::uint32_t entry = 0; entry < editCount; ++entry) {
            const auto packedXZ = readInteger<std::uint8_t>(payload, cursor);
            world::PersistentBlockEdit edit;
            edit.x = chunk.chunkX * world::kChunkWidth +
                     static_cast<std::int32_t>(packedXZ & 0x0FU);
            edit.z = chunk.chunkZ * world::kChunkDepth +
                     static_cast<std::int32_t>((packedXZ >> 4U) & 0x0FU);
            edit.y = readInteger<std::int16_t>(payload, cursor);
            if (!world::isWorldYInRange(edit.y)) {
                throw std::runtime_error("region file contains an invalid block edit");
            }
            const auto stateIndex = readInteger<std::uint16_t>(payload, cursor);
            if (stateIndex >= statePalette.size()) {
                throw std::runtime_error(
                    "region file references an unknown state palette entry");
            }
            edit.state = statePalette[stateIndex];
            chunk.edits.push_back(std::move(edit));
        }
        const auto entityCount = readInteger<std::uint32_t>(payload, cursor);
        if (entityCount > kMaximumEdits) {
            throw std::runtime_error("region file has an unreasonable entity count");
        }
        chunk.entities.reserve(entityCount);
        for (std::uint32_t entry = 0; entry < entityCount; ++entry) {
            const auto speciesIndex = readInteger<std::uint16_t>(payload, cursor);
            if (speciesIndex >= species.size()) {
                throw std::runtime_error(
                    "region file references an unknown species palette entry");
            }
            PersistentEntity entity;
            entity.species = species[speciesIndex];
            entity.x = readFloat(payload, cursor);
            entity.y = readFloat(payload, cursor);
            entity.z = readFloat(payload, cursor);
            entity.yaw = readFloat(payload, cursor);
            entity.vx = readFloat(payload, cursor);
            entity.vy = readFloat(payload, cursor);
            entity.vz = readFloat(payload, cursor);
            entity.health = readFloat(payload, cursor);
            entity.angerTicks = readInteger<std::int32_t>(payload, cursor);
            entity.ageTicks = readInteger<std::uint32_t>(payload, cursor);
            entity.rngState = readInteger<std::uint32_t>(payload, cursor);
            // fireTicks arrived in version 2; a version-1 region leaves it zero.
            if (header.version >= 2U) {
                entity.fireTicks = readInteger<std::int32_t>(payload, cursor);
            }
            static_cast<void>(readInteger<std::uint8_t>(payload, cursor));  // flags, reserved
            // Active effects arrived in version 3; older regions leave it empty.
            if (header.version >= 3U) {
                readEffectList(payload, cursor, entity.effects);
            }
            // AgeableMob age/love arrived in version 4; earlier regions read zero.
            if (header.version >= 4U) {
                entity.age = readInteger<std::int32_t>(payload, cursor);
                entity.loveTicks = readInteger<std::int32_t>(payload, cursor);
            }
            // A creature saved outside the world is a corrupt record.
            if (!(entity.y >= -64.0F && entity.y <= 384.0F)) {
                throw std::runtime_error("region file has an invalid entity position");
            }
            chunk.entities.push_back(std::move(entity));
        }
        if (cursor != header.end) {
            throw std::runtime_error("region file chunk has trailing data");
        }
        region.chunks.push_back(std::move(chunk));
    }
}

// Writes the region files a save needs and prunes the ones it no longer does.
// Called from save().
//
// The gather starts from the flat lists, then merges the region files already on
// disk for the chunks the unload path is still holding out of the simulation
// (`unloadedChunks`): those creatures live in their region file, not in
// game.entities — the unload path removed them from the simulation — so
// rebuilding the files from game alone would silently drop that herd. Every
// other disk record is a mirror the fresh gather replaces, or a stale copy of a
// creature that moved or despawned while its chunk was loaded, and must not
// survive into the rewritten file.
void writeRegionFiles(const std::filesystem::path& directory, const SaveGame& game,
                      const std::set<std::pair<std::int32_t, std::int32_t>>& unloadedChunks) {
    auto regions = gatherRegions(game);
    const auto regionDirectory = directory / "region";
    std::error_code error;
    if (std::filesystem::is_directory(regionDirectory, error)) {
        for (const auto& entry : std::filesystem::directory_iterator(regionDirectory, error)) {
            if (error) {
                break;
            }
            if (!entry.is_regular_file()) {
                continue;
            }
            const auto coordinates = parseRegionFileName(entry.path().filename().string());
            if (!coordinates.has_value()) {
                continue;
            }
            std::ifstream input{entry.path(), std::ios::binary | std::ios::ate};
            const auto length = input.tellg();
            if (!input || length < static_cast<std::streamoff>(
                                      kRegionMagic.size() + sizeof(std::uint64_t))) {
                continue;  // unreadable: the fresh gather regenerates it
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
            input.seekg(0);
            input.read(reinterpret_cast<char*>(bytes.data()), length);
            if (!input) {
                continue;
            }
            RegionData disk;
            try {
                readRegionFile(bytes, disk);
            } catch (const std::exception&) {
                continue;  // torn: the fresh gather regenerates it
            }
            // Only chunks the unload path is still holding out of the simulation
            // get their disk creatures preserved; the fresh gather (or its
            // absence) is authoritative for everything else.
            bool hasUnloadedChunk = false;
            for (const auto& diskChunk : disk.chunks) {
                if (unloadedChunks.contains(
                        std::pair<std::int32_t, std::int32_t>{diskChunk.chunkX,
                                                              diskChunk.chunkZ})) {
                    hasUnloadedChunk = true;
                    break;
                }
            }
            if (!hasUnloadedChunk) {
                continue;
            }
            auto& region = regions[*coordinates];
            region.regionX = coordinates->first;
            region.regionZ = coordinates->second;
            for (const auto& diskChunk : disk.chunks) {
                if (!unloadedChunks.contains(
                        std::pair<std::int32_t, std::int32_t>{diskChunk.chunkX,
                                                              diskChunk.chunkZ})) {
                    continue;
                }
                auto& target = chunkInRegion(region, diskChunk.chunkX, diskChunk.chunkZ);
                target.entities.insert(target.entities.end(), diskChunk.entities.begin(),
                                       diskChunk.entities.end());
            }
        }
    }
    for (const auto& [coordinates, region] : regions) {
        std::vector<std::uint8_t> bytes;
        appendRegionFile(bytes, region);
        replaceFile(regionDirectory / regionFileName(coordinates.first, coordinates.second), bytes);
    }
    // Prune region files this save no longer produces — a reverted edit or a
    // region emptied by the merge must not linger and resurrect on the next load.
    if (!std::filesystem::is_directory(regionDirectory, error)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(regionDirectory, error)) {
        if (error) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto coordinates = parseRegionFileName(entry.path().filename().string());
        if (!coordinates.has_value()) {
            continue;
        }
        if (regions.contains(*coordinates)) {
            continue;
        }
        std::filesystem::remove(entry.path(), error);
        error.clear();
    }
}

// Loads every region file in the world back into the flat edit and entity lists,
// the shape the rest of the pipeline works with. A corrupt region is skipped —
// a torn file regenerates from seed rather than refusing the whole world, which
// is the difference from world.dat: the region is the regenerable part.
void readRegionDirectory(const std::filesystem::path& directory, SaveGame& game) {
    const auto regionDirectory = directory / "region";
    std::error_code error;
    if (!std::filesystem::is_directory(regionDirectory, error)) {
        return;
    }
    // Directory iteration order is unspecified; sort the region files by name so
    // a save flattens back in the same order every load — region lexicographic,
    // which matches the map order gatherRegions writes them in.
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(regionDirectory, error)) {
        if (error) {
            break;
        }
        if (entry.is_regular_file() &&
            parseRegionFileName(entry.path().filename().string()).has_value()) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files, {}, &std::filesystem::path::filename);
    for (const auto& path : files) {
        std::ifstream input{path, std::ios::binary | std::ios::ate};
        if (!input) {
            std::cerr << "[save] skipping unreadable region " << path.string() << '\n';
            continue;
        }
        const auto length = input.tellg();
        if (length < static_cast<std::streamoff>(kRegionMagic.size() + sizeof(std::uint64_t))) {
            std::cerr << "[save] skipping truncated region " << path.string() << '\n';
            continue;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(bytes.data()), length);
        if (!input) {
            std::cerr << "[save] skipping unreadable region " << path.string() << '\n';
            continue;
        }
        RegionData region;
        try {
            readRegionFile(bytes, region);
        } catch (const std::exception& exception) {
            std::cerr << "[save] skipping corrupt region " << path.string() << ": "
                      << exception.what() << '\n';
            continue;
        }
        for (const auto& chunk : region.chunks) {
            game.edits.insert(game.edits.end(), chunk.edits.begin(), chunk.edits.end());
            game.entities.insert(game.entities.end(), chunk.entities.begin(),
                                 chunk.entities.end());
        }
    }
}

// The block entities. One section per type, each with its own size and version,
// so a build that has never heard of a type skips that section and keeps the
// rest — the same forward compatibility the outer block frame gives, one level
// down. BlockEntityStore already unified chests and furnaces in memory; this is
// the same unification on disk.
constexpr std::uint32_t kBlockEntityBlockTag = blockTag("BENT");
constexpr std::uint16_t kBlockEntityBlockVersion = 1U;
constexpr std::uint32_t kChestSectionTag = blockTag("CHST");
constexpr std::uint32_t kFurnaceSectionTag = blockTag("FURN");
constexpr std::uint32_t kTrappedChestSectionTag = blockTag("TCST");
constexpr std::uint16_t kChestSectionVersion = 1U;
constexpr std::uint16_t kFurnaceSectionVersion = 1U;
constexpr std::uint16_t kTrappedChestSectionVersion = 1U;

void appendBlockEntityBlock(std::vector<std::uint8_t>& bytes, const SaveWriteContext& context) {
    const auto& game = context.game;
    const SaveBlockWriter block{bytes, kBlockEntityBlockTag, kBlockEntityBlockVersion};
    appendInteger(bytes, static_cast<std::uint16_t>(3U));  // section count
    {
        const SaveBlockWriter chests{bytes, kChestSectionTag, kChestSectionVersion};
        appendInteger(bytes, static_cast<std::uint32_t>(game.chests.size()));
        for (const auto& chest : game.chests) {
            appendInteger(bytes, static_cast<std::int32_t>(chest.position.x));
            appendInteger(bytes, static_cast<std::int32_t>(chest.position.y));
            appendInteger(bytes, static_cast<std::int32_t>(chest.position.z));
            appendSlots(bytes, context, chest.items);
        }
    }
    {
        // The trapped chests: the same per-chest record as CHST, in their own
        // section so a chest and a trapped chest are never confused on load.
        const SaveBlockWriter trapped{bytes, kTrappedChestSectionTag, kTrappedChestSectionVersion};
        appendInteger(bytes, static_cast<std::uint32_t>(game.trappedChests.size()));
        for (const auto& chest : game.trappedChests) {
            appendInteger(bytes, static_cast<std::int32_t>(chest.position.x));
            appendInteger(bytes, static_cast<std::int32_t>(chest.position.y));
            appendInteger(bytes, static_cast<std::int32_t>(chest.position.z));
            appendSlots(bytes, context, chest.items);
        }
    }
    {
        const SaveBlockWriter furnaces{bytes, kFurnaceSectionTag, kFurnaceSectionVersion};
        appendInteger(bytes, static_cast<std::uint32_t>(game.furnaces.size()));
        for (const auto& furnace : game.furnaces) {
            appendInteger(bytes, static_cast<std::int32_t>(furnace.position.x));
            appendInteger(bytes, static_cast<std::int32_t>(furnace.position.y));
            appendInteger(bytes, static_cast<std::int32_t>(furnace.position.z));
            appendStack(bytes, context, furnace.input);
            appendStack(bytes, context, furnace.fuel);
            appendStack(bytes, context, furnace.output);
            appendInteger(bytes, static_cast<std::int32_t>(furnace.burnTicks));
            appendInteger(bytes, static_cast<std::int32_t>(furnace.initialBurnTicks));
            appendInteger(bytes, static_cast<std::int32_t>(furnace.cookTicks));
            appendInteger(bytes, static_cast<std::int32_t>(furnace.cookDurationTicks));
        }
    }
}

void readBlockEntityBlock(std::span<const std::uint8_t> payload, std::size_t& cursor,
                          const SaveBlockHeader& header, SaveReadContext& context) {
    auto& game = context.game;
    const auto sectionCount = readInteger<std::uint16_t>(payload, cursor);
    for (std::uint16_t index = 0; index < sectionCount; ++index) {
        const auto section = readBlockHeader(payload, cursor, "block entity section");
        if (section.end > header.end) {
            throw std::runtime_error("world.dat block entity section overruns its block");
        }
        if (section.tag == kChestSectionTag && section.version <= kChestSectionVersion) {
            const auto count = readInteger<std::uint32_t>(payload, cursor);
            if (count > kMaximumChests) {
                throw std::runtime_error("world.dat chest count is unreasonable");
            }
            game.chests.reserve(count);
            for (std::uint32_t entry = 0; entry < count; ++entry) {
                gameplay::ChestBlockEntity chest;
                chest.position.x = readInteger<std::int32_t>(payload, cursor);
                chest.position.y = readInteger<std::int32_t>(payload, cursor);
                chest.position.z = readInteger<std::int32_t>(payload, cursor);
                if (!world::isWorldYInRange(chest.position.y)) {
                    throw std::runtime_error("world.dat contains an invalid chest position");
                }
                readSlots(payload, cursor, context, chest.items);
                game.chests.push_back(std::move(chest));
            }
        } else if (section.tag == kTrappedChestSectionTag &&
                   section.version <= kTrappedChestSectionVersion) {
            const auto count = readInteger<std::uint32_t>(payload, cursor);
            if (count > kMaximumChests) {
                throw std::runtime_error("world.dat trapped chest count is unreasonable");
            }
            game.trappedChests.reserve(count);
            for (std::uint32_t entry = 0; entry < count; ++entry) {
                gameplay::ChestBlockEntity chest;
                chest.position.x = readInteger<std::int32_t>(payload, cursor);
                chest.position.y = readInteger<std::int32_t>(payload, cursor);
                chest.position.z = readInteger<std::int32_t>(payload, cursor);
                if (!world::isWorldYInRange(chest.position.y)) {
                    throw std::runtime_error("world.dat contains an invalid trapped chest position");
                }
                readSlots(payload, cursor, context, chest.items);
                game.trappedChests.push_back(std::move(chest));
            }
        } else if (section.tag == kFurnaceSectionTag &&
                   section.version <= kFurnaceSectionVersion) {
            const auto count = readInteger<std::uint32_t>(payload, cursor);
            if (count > kMaximumChests) {
                throw std::runtime_error("world.dat furnace count is unreasonable");
            }
            game.furnaces.reserve(count);
            for (std::uint32_t entry = 0; entry < count; ++entry) {
                gameplay::FurnaceBlockEntity furnace;
                furnace.position.x = readInteger<std::int32_t>(payload, cursor);
                furnace.position.y = readInteger<std::int32_t>(payload, cursor);
                furnace.position.z = readInteger<std::int32_t>(payload, cursor);
                if (!world::isWorldYInRange(furnace.position.y)) {
                    throw std::runtime_error("world.dat contains an invalid furnace position");
                }
                readStackRecord(payload, cursor, context, furnace.input);
                readStackRecord(payload, cursor, context, furnace.fuel);
                readStackRecord(payload, cursor, context, furnace.output);
                furnace.burnTicks = readInteger<std::int32_t>(payload, cursor);
                furnace.initialBurnTicks = readInteger<std::int32_t>(payload, cursor);
                furnace.cookTicks = readInteger<std::int32_t>(payload, cursor);
                furnace.cookDurationTicks = readInteger<std::int32_t>(payload, cursor);
                game.furnaces.push_back(std::move(furnace));
            }
        }
        // Unknown type, or one written by a newer build: skip the whole section.
        cursor = section.end;
    }
    cursor = header.end;
}

// The registry. Adding a state owner is one line here plus its two functions —
// no format bump, no positional read, nothing for the other owners to notice.
using SaveBlockWriteFn = void (*)(std::vector<std::uint8_t>&, const SaveWriteContext&);
using SaveBlockReadFn = void (*)(std::span<const std::uint8_t>, std::size_t&,
                                 const SaveBlockHeader&, SaveReadContext&);

struct SaveBlockOwner final {
    std::uint32_t tag;
    std::uint16_t version;
    SaveBlockWriteFn write;
    SaveBlockReadFn read;
    // M-3: CHNK and ENTY kept their read handlers so a pre-region world.dat still
    // opens, but nothing new writes them — their content lives in region files
    // now. `writeable` keeps them out of the save() write loop.
    bool writeable = true;
};

// The pre-17 blocks kept their own framing, so they are adapted here rather than
// rewritten: the wrappers hand the reader the block start it still expects.
void writeGameRulesOwner(std::vector<std::uint8_t>& bytes, const SaveWriteContext& context) {
    appendGameRulesBlock(bytes, context.game.gameRules);
}
void readGameRulesOwner(std::span<const std::uint8_t> payload, std::size_t& cursor,
                        const SaveBlockHeader& header, SaveReadContext& context) {
    cursor = header.bodyStart - kBlockHeaderBytes;
    readGameRulesBlock(payload, cursor, context.game.gameRules);
}
void writeSpawnPointOwner(std::vector<std::uint8_t>& bytes, const SaveWriteContext& context) {
    appendSpawnPointBlock(bytes, context.game);
}
void readSpawnPointOwner(std::span<const std::uint8_t> payload, std::size_t& cursor,
                         const SaveBlockHeader& header, SaveReadContext& context) {
    cursor = header.bodyStart - kBlockHeaderBytes;
    readSpawnPointBlock(payload, cursor, context.game);
}
void writeWeatherOwner(std::vector<std::uint8_t>& bytes, const SaveWriteContext& context) {
    appendWeatherBlock(bytes, context.game);
}
void readWeatherOwner(std::span<const std::uint8_t> payload, std::size_t& cursor,
                      const SaveBlockHeader& header, SaveReadContext& context) {
    cursor = header.bodyStart - kBlockHeaderBytes;
    readWeatherBlock(payload, cursor, context.game);
}
void writeEntityOwner(std::vector<std::uint8_t>& bytes, const SaveWriteContext& context) {
    appendEntityBlock(bytes, context.game.entities);
}
void readEntityOwner(std::span<const std::uint8_t> payload, std::size_t& cursor,
                     const SaveBlockHeader& header, SaveReadContext& context) {
    cursor = header.bodyStart - kBlockHeaderBytes;
    readEntityBlock(payload, cursor, context.game.entities);
}
void writeClockOwner(std::vector<std::uint8_t>& bytes, const SaveWriteContext& context) {
    appendClockBlock(bytes, context.game);
}
void readClockOwner(std::span<const std::uint8_t> payload, std::size_t& cursor,
                    const SaveBlockHeader& header, SaveReadContext& context) {
    cursor = header.bodyStart - kBlockHeaderBytes;
    readClockBlock(payload, cursor, context.game);
}
void writeDropOwner(std::vector<std::uint8_t>& bytes, const SaveWriteContext& context) {
    appendDropBlock(bytes, context.game.itemDrops, context.game.fallingBlocks);
}
void readDropOwner(std::span<const std::uint8_t> payload, std::size_t& cursor,
                   const SaveBlockHeader& header, SaveReadContext& context) {
    cursor = header.bodyStart - kBlockHeaderBytes;
    readDropBlock(payload, cursor, context.game.itemDrops, context.game.fallingBlocks);
}

constexpr std::array<SaveBlockOwner, 10> kSaveBlockOwners{{
    {kWorldBlockTag, kWorldBlockVersion, &appendWorldBlock, &readWorldBlock},
    {kPlayerBlockTag, kPlayerBlockVersion, &appendPlayerBlock, &readPlayerBlock},
    {kChunkBlockTag, kChunkBlockVersion, &appendChunkBlock, &readChunkBlock,
     /*writeable=*/false},
    {kBlockEntityBlockTag, kBlockEntityBlockVersion, &appendBlockEntityBlock,
     &readBlockEntityBlock},
    {kGameRulesBlockTag, kGameRulesBlockVersion, &writeGameRulesOwner, &readGameRulesOwner},
    {kSpawnPointBlockTag, kSpawnPointBlockVersion, &writeSpawnPointOwner, &readSpawnPointOwner},
    {kWeatherBlockTag, kWeatherBlockVersion, &writeWeatherOwner, &readWeatherOwner},
    {kEntityBlockTag, kEntityBlockVersion, &writeEntityOwner, &readEntityOwner,
     /*writeable=*/false},
    {kClockBlockTag, kClockBlockVersion, &writeClockOwner, &readClockOwner},
    {kDropBlockTag, kDropBlockVersion, &writeDropOwner, &readDropOwner},
}};

} // namespace

SaveRepository::SaveRepository(std::filesystem::path root) : root_(std::move(root)) {}

std::string SaveRepository::sanitizeDisplayName(std::string name) {
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char character) {
        return character < 32U || character == '=' || character == '\n' || character == '\r';
    }), name.end());
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front())) != 0)
        name.erase(name.begin());
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())) != 0)
        name.pop_back();
    if (name.size() > 32U) name.resize(32U);
    return name.empty() ? "New World" : name;
}

std::vector<SaveSummary> SaveRepository::list() const {
    std::vector<SaveSummary> saves;
    std::error_code error;
    if (!std::filesystem::is_directory(root_, error)) return saves;
    for (const auto& entry : std::filesystem::directory_iterator(root_, error)) {
        if (error) break;
        if (!entry.is_directory()) continue;
        const auto identifier = entry.path().filename().string();
        if (!safeIdentifier(identifier)) continue;
        const auto metadata = entry.path() / "level.properties";
        if (!std::filesystem::is_regular_file(metadata)) continue;
        try {
            saves.push_back(summaryFromProperties(metadata, identifier));
        } catch (const std::exception&) {
            // A damaged world remains isolated and does not hide healthy saves.
        }
    }
    std::ranges::sort(saves, std::greater{}, &SaveSummary::lastPlayedUnixSeconds);
    return saves;
}

SaveGame SaveRepository::create(std::string displayName, std::uint64_t seed) const {
    SaveGame game;
    game.summary.displayName = sanitizeDisplayName(std::move(displayName));
    game.summary.seed = seed;
    game.summary.lastPlayedUnixSeconds = nowUnixSeconds();
    std::string slug;
    for (const char rawCharacter : game.summary.displayName) {
        const auto character = static_cast<unsigned char>(rawCharacter);
        if (std::isalnum(character) != 0) slug.push_back(static_cast<char>(std::tolower(character)));
        else if (!slug.empty() && slug.back() != '-') slug.push_back('-');
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    if (slug.empty()) slug = "world";
    const std::string base = slug + "-" + std::to_string(game.summary.lastPlayedUnixSeconds);
    game.summary.identifier = base;
    for (unsigned int suffix = 2U;
         std::filesystem::exists(root_ / game.summary.identifier);
         ++suffix) {
        game.summary.identifier = base + "-" + std::to_string(suffix);
    }
    return game;
}

void SaveRepository::save(
    SaveGame game,
    const std::set<std::pair<std::int32_t, std::int32_t>>& unloadedChunks) const {
    if (!safeIdentifier(game.summary.identifier)) {
        throw std::invalid_argument("Unsafe save identifier");
    }
    game.summary.displayName = sanitizeDisplayName(std::move(game.summary.displayName));
    game.summary.lastPlayedUnixSeconds = nowUnixSeconds();
    // M-3: the edits and creatures no longer live in world.dat — chunks own them
    // in region/ files, written (and pruned) below. world.dat stays small: just
    // the world and player state, the block entities and the drops.
    const auto directory = root_ / game.summary.identifier;
    writeRegionFiles(directory, game, unloadedChunks);

    std::vector<std::uint8_t> bytes{kMagic.begin(), kMagic.end()};
    // One allocation for the whole file. The edit list used to dominate the
    // buffer; with region files carrying it, world.dat is a few blocks of
    // player and container state, so the prologue slack is plenty.
    bytes.reserve(kReservedPrologueBytes + game.chests.size() * 64U +
                  game.trappedChests.size() * 64U + game.furnaces.size() * 64U +
                  game.itemDrops.size() * 40U);
    appendInteger(bytes, kFormatVersion);
    appendInteger(bytes, game.summary.seed);

    // Both palettes are gathered first and written ahead of every block, because
    // every record past this point names its content by palette index.
    BlockPalette blockPalette = makeBlockPalette();
    ItemPalette itemPalette;
    const auto gatherStack = [&](const gameplay::ItemStack& stack) {
        static_cast<void>(blockPalette.indexOf(world::blockId(stack.block)));
        static_cast<void>(itemPalette.indexOf(stack.item));
    };
    for (const auto& stack : game.inventory) {
        gatherStack(stack);
    }
    for (const auto& chest : game.chests) {
        for (const auto& stack : chest.items) {
            gatherStack(stack);
        }
    }
    for (const auto& chest : game.trappedChests) {
        for (const auto& stack : chest.items) {
            gatherStack(stack);
        }
    }
    for (const auto& furnace : game.furnaces) {
        gatherStack(furnace.input);
        gatherStack(furnace.fuel);
        gatherStack(furnace.output);
    }
    if (blockPalette.entries().size() > kMaximumPaletteEntries ||
        itemPalette.entries().size() > kMaximumPaletteEntries) {
        throw std::runtime_error("Save references more content than a palette can hold");
    }
    appendInteger(bytes, static_cast<std::uint16_t>(blockPalette.entries().size()));
    for (const auto block : blockPalette.entries()) {
        appendString(bytes, world::blockRegistry().identifier(block).toString());
    }
    appendInteger(bytes, static_cast<std::uint16_t>(itemPalette.entries().size()));
    for (const auto* item : itemPalette.entries()) {
        // The block sentinel (nullptr) writes an empty string, which resolves
        // back to nullptr on load.
        appendString(bytes, item == nullptr ? std::string{} : item->identifier.toString());
    }

    // Every owner writes its own block. The order here is the order on disk, but
    // nothing depends on it: the reader dispatches on the tag. CHNK and ENTY are
    // read-only here (writeable=false) — their content is in the region files.
    const SaveWriteContext context{game, blockPalette, itemPalette};
    for (const auto& owner : kSaveBlockOwners) {
        if (!owner.writeable) {
            continue;
        }
        owner.write(bytes, context);
    }
    appendInteger(bytes, checksum(bytes));
    replaceFile(directory / "world.dat", bytes);
    writeMetadata(directory / "level.properties", game.summary);
}

void SaveRepository::saveChunk(const std::string& identifier, int chunkX, int chunkZ,
                               std::vector<world::PersistentBlockEdit> edits,
                               std::vector<PersistentEntity> entities,
                               world::DimensionId dimension) const {
    if (!safeIdentifier(identifier)) throw std::invalid_argument("Unsafe save identifier");
    const auto path = regionDirectoryFor(root_ / identifier, dimension) /
        regionFileName(regionOf(chunkX), regionOf(chunkZ));
    RegionData region;
    // Merge with whatever the region already holds: a chunk unloads while its
    // neighbours stay, so the file is shared.
    if (std::filesystem::is_regular_file(path)) {
        std::ifstream input{path, std::ios::binary | std::ios::ate};
        const auto length = input.tellg();
        std::vector<std::uint8_t> bytes;
        if (input && length >= static_cast<std::streamoff>(
                                  kRegionMagic.size() + sizeof(std::uint64_t))) {
            bytes.resize(static_cast<std::size_t>(length));
            input.seekg(0);
            input.read(reinterpret_cast<char*>(bytes.data()), length);
        }
        if (!bytes.empty()) {
            try {
                readRegionFile(bytes, region);
            } catch (const std::exception&) {
                // A torn neighbour region regenerates; this chunk's write is the
                // new truth for it either way.
                region = RegionData{};
            }
        }
    }
    std::erase_if(region.chunks, [&](const RegionChunkData& chunk) {
        return chunk.chunkX == chunkX && chunk.chunkZ == chunkZ;
    });
    if (!edits.empty() || !entities.empty()) {
        region.chunks.push_back({chunkX, chunkZ, std::move(edits), std::move(entities)});
    }
    if (region.chunks.empty()) {
        std::error_code error;
        std::filesystem::remove(path, error);
        return;
    }
    std::vector<std::uint8_t> bytes;
    appendRegionFile(bytes, region);
    replaceFile(path, bytes);
}

void SaveRepository::saveChunks(const std::string& identifier,
                                std::vector<ChunkPersistRecord> records,
                                world::DimensionId dimension) const {
    if (!safeIdentifier(identifier)) throw std::invalid_argument("Unsafe save identifier");
    if (records.empty()) {
        return;
    }
    const auto regionDirectory = regionDirectoryFor(root_ / identifier, dimension);
    // Group the burst by region file so each region is read-modified-written
    // once, no matter how many chunks in the burst fall inside it.
    std::map<std::pair<std::int32_t, std::int32_t>, std::vector<std::size_t>> byRegion;
    for (std::size_t index = 0; index < records.size(); ++index) {
        byRegion[{regionOf(records[index].chunkX), regionOf(records[index].chunkZ)}]
            .push_back(index);
    }
    for (const auto& [regionKey, indices] : byRegion) {
        const auto path = regionDirectory /
            regionFileName(regionKey.first, regionKey.second);
        RegionData region;
        if (std::filesystem::is_regular_file(path)) {
            std::ifstream input{path, std::ios::binary | std::ios::ate};
            const auto length = input.tellg();
            std::vector<std::uint8_t> existing;
            if (input && length >= static_cast<std::streamoff>(
                                       kRegionMagic.size() + sizeof(std::uint64_t))) {
                existing.resize(static_cast<std::size_t>(length));
                input.seekg(0);
                input.read(reinterpret_cast<char*>(existing.data()), length);
            }
            if (!existing.empty()) {
                try {
                    readRegionFile(existing, region);
                } catch (const std::exception&) {
                    region = RegionData{};
                }
            }
        }
        for (const auto index : indices) {
            auto& record = records[index];
            std::erase_if(region.chunks, [&](const RegionChunkData& chunk) {
                return chunk.chunkX == record.chunkX && chunk.chunkZ == record.chunkZ;
            });
            if (!record.edits.empty() || !record.entities.empty()) {
                region.chunks.push_back({record.chunkX, record.chunkZ,
                                         std::move(record.edits), std::move(record.entities)});
            }
        }
        if (region.chunks.empty()) {
            std::error_code error;
            std::filesystem::remove(path, error);
            continue;
        }
        std::vector<std::uint8_t> bytes;
        appendRegionFile(bytes, region);
        replaceFile(path, bytes);
    }
}

std::vector<PersistentEntity> SaveRepository::loadChunkEntities(
    const std::string& identifier, int chunkX, int chunkZ, world::DimensionId dimension) const {
    std::vector<PersistentEntity> entities;
    if (!safeIdentifier(identifier)) throw std::invalid_argument("Unsafe save identifier");
    const auto path = regionDirectoryFor(root_ / identifier, dimension) /
        regionFileName(regionOf(chunkX), regionOf(chunkZ));
    if (!std::filesystem::is_regular_file(path)) {
        return entities;
    }
    std::ifstream input{path, std::ios::binary | std::ios::ate};
    if (!input) {
        return entities;
    }
    const auto length = input.tellg();
    if (length < static_cast<std::streamoff>(kRegionMagic.size() + sizeof(std::uint64_t))) {
        return entities;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!input) {
        return entities;
    }
    RegionData region;
    try {
        readRegionFile(bytes, region);
    } catch (const std::exception&) {
        // A torn region regenerates from seed; there is no herd to restore.
        return entities;
    }
    for (const auto& chunk : region.chunks) {
        if (chunk.chunkX == chunkX && chunk.chunkZ == chunkZ) {
            return chunk.entities;
        }
    }
    return entities;
}

std::filesystem::path SaveRepository::dimensionRegionDirectory(
    const std::string& identifier, world::DimensionId dimension) const {
    return regionDirectoryFor(root_ / identifier, dimension);
}

void SaveRepository::rename(const std::string& identifier, std::string displayName) const {
    if (!safeIdentifier(identifier)) throw std::invalid_argument("Unsafe save identifier");
    const auto directory = root_ / identifier;
    if (!std::filesystem::is_directory(directory))
        throw std::runtime_error("Save not found: " + identifier);
    auto summary = summaryFromProperties(directory / "level.properties", identifier);
    summary.displayName = sanitizeDisplayName(std::move(displayName));
    writeMetadata(directory / "level.properties", summary);
}

void SaveRepository::remove(const std::string& identifier) const {
    if (!safeIdentifier(identifier)) throw std::invalid_argument("Unsafe save identifier");
    const auto directory = root_ / identifier;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
        throw std::runtime_error("Save not found: " + identifier);
    std::filesystem::remove_all(directory, error);
    if (error) throw std::runtime_error("Unable to delete save: " + error.message());
}

namespace {

// Formats 1 through 16, where every section sat at a fixed offset and each new
// one was gated on a version number. Kept byte-for-byte so old worlds still
// open; nothing new is ever added here, because format 17 onwards is the block
// registry above.
void loadLegacy(std::span<const std::uint8_t> payload, std::size_t& cursor,
                std::uint32_t formatVersion, SaveGame& game) {
    game.hasPlayerPosition = readInteger<std::uint8_t>(payload, cursor) != 0U;
    game.playerX = readFloat(payload, cursor);
    game.playerY = readFloat(payload, cursor);
    game.playerZ = readFloat(payload, cursor);
    game.gameTimeSeconds = readDouble(payload, cursor);
    const auto mode = readInteger<std::uint8_t>(payload, cursor);
    if (mode > static_cast<std::uint8_t>(gameplay::GameMode::Creative))
        throw std::runtime_error("world.dat contains an invalid game mode");
    game.gameMode = static_cast<gameplay::GameMode>(mode);
    game.selectedHotbarSlot = readInteger<std::uint8_t>(payload, cursor);
    if (game.selectedHotbarSlot >= gameplay::Inventory::kHotbarSize)
        throw std::runtime_error("world.dat contains an invalid hotbar slot");
    if (formatVersion >= 7U) {
        const auto difficulty = readInteger<std::uint8_t>(payload, cursor);
        if (difficulty >= gameplay::kDifficultyCount)
            throw std::runtime_error("world.dat contains an invalid difficulty");
        game.difficulty = static_cast<gameplay::Difficulty>(difficulty);
    }
    // randomTickSpeed lived at a fixed header offset through format 8; format 9
    // moved the game rules into a trailing self-describing block. An old save's
    // value is migrated into the rules registry here, then written back as part
    // of the block on the next save.
    if (formatVersion >= 8U && formatVersion < 9U) {
        const auto legacySpeed = readInteger<std::int32_t>(payload, cursor);
        // The gamerule lives in [0, 1000]; anything outside is a corrupt save.
        if (legacySpeed < 0 || legacySpeed > 1000)
            throw std::runtime_error("world.dat contains an invalid randomTickSpeed");
        static_cast<void>(game.gameRules.applyDecoded(
            "randomTickSpeed", gameplay::GameRuleType::Int, legacySpeed));
    }
    if (formatVersion >= 4U) {
        game.playerHealth = readFloat(payload, cursor);
        game.playerFoodLevel = readInteger<std::int32_t>(payload, cursor);
        game.playerSaturation = readFloat(payload, cursor);
        game.playerAirTicks = readInteger<std::int32_t>(payload, cursor);
        if (!(game.playerHealth >= 0.0F &&
              game.playerHealth <= gameplay::PlayerVitals::kMaximumHealth) ||
            game.playerFoodLevel < 0 ||
            game.playerFoodLevel > gameplay::PlayerVitals::kMaximumFood ||
            !(game.playerSaturation >= 0.0F &&
              game.playerSaturation <= gameplay::PlayerVitals::kMaximumFood) ||
            game.playerAirTicks < -20 ||
            game.playerAirTicks > gameplay::PlayerVitals::kMaximumAirTicks)
            throw std::runtime_error("world.dat contains invalid player vitals");
    }
    // Format 5 onwards resolves blocks through a palette of identifiers and
    // format 6 does the same for items; older saves carry frozen ordinals.
    const bool palettedBlocks = formatVersion >= kFirstBlockPaletteFormatVersion;
    const bool palettedItems = formatVersion >= kFirstItemPaletteFormatVersion;
    // Content this build no longer knows becomes air or an empty stack rather
    // than refusing to open the world.
    const auto readPalette = [&](auto resolve, auto fallback) {
        using Value = decltype(fallback);
        std::vector<Value> entries;
        const auto size = readInteger<std::uint16_t>(payload, cursor);
        if (size == 0U) throw std::runtime_error("world.dat has an empty palette");
        entries.reserve(size);
        for (std::uint16_t index = 0; index < size; ++index) {
            entries.push_back(resolve(readString(payload, cursor)).value_or(fallback));
        }
        return entries;
    };
    std::vector<world::Block> blockPalette;
    // Parallel to blockPalette: what state a palette entry carried back when it
    // was a block of its own. Empty for every entry a current save writes.
    std::vector<std::optional<LegacyStateOverride>> blockPaletteStates;
    std::vector<const gameplay::Item*> itemPalette;
    if (palettedBlocks) {
        blockPalette = readPalette(
            [&](std::string_view text) -> std::optional<world::Block> {
                const auto legacy = legacyStateIdentifier(text);
                blockPaletteStates.push_back(legacy);
                if (legacy.has_value()) return legacy->block;
                return blockByName(text);
            },
            world::Block::Air);
    }
    if (palettedItems) {
        itemPalette = readPalette(
            [](std::string_view text) -> std::optional<const gameplay::Item*> {
                const auto* item = gameplay::itemFromIdentifier(text);
                // An unknown id (including the sentinel's empty string) resolves
                // to nullptr, i.e. an empty/block stack.
                return item == nullptr ? std::nullopt
                                       : std::optional<const gameplay::Item*>{item};
            },
            static_cast<const gameplay::Item*>(nullptr));
    }
    // The palette entry an edit last resolved, so the edit loop can pick up a
    // legacy state override without re-reading the index.
    std::optional<LegacyStateOverride> lastBlockState;
    const auto readBlock = [&](std::size_t& readCursor) {
        lastBlockState.reset();
        if (!palettedBlocks) {
            const auto ordinal = readInteger<std::uint8_t>(payload, readCursor);
            if (ordinal < kLegacyBlockOrder.size()) {
                // Formats 1-4 wrote ordinals against a frozen name table, and
                // that table still names lit_furnace and the four wall torches.
                lastBlockState = legacyStateIdentifier(kLegacyBlockOrder[ordinal]);
                if (lastBlockState.has_value()) return lastBlockState->block;
            }
            return legacyBlockFromOrdinal(ordinal);
        }
        const auto index = readInteger<std::uint16_t>(payload, readCursor);
        if (index >= blockPalette.size())
            throw std::runtime_error("world.dat references a block outside the palette");
        if (index < blockPaletteStates.size()) lastBlockState = blockPaletteStates[index];
        return blockPalette[index];
    };
    const auto readItem = [&](std::size_t& readCursor) {
        if (!palettedItems) {
            return legacyItemFromOrdinal(readInteger<std::uint8_t>(payload, readCursor));
        }
        const auto index = readInteger<std::uint16_t>(payload, readCursor);
        if (index >= itemPalette.size())
            throw std::runtime_error("world.dat references an item outside the palette");
        return itemPalette[index];
    };
    // Tool wear arrived with v7; anything older read back as a pristine tool.
    const auto readStack = [&](gameplay::ItemStack& stack) {
        stack.block = readBlock(cursor);
        stack.count = readInteger<std::uint8_t>(payload, cursor);
        stack.item = readItem(cursor);
        stack.damage = formatVersion >= 7U ? readInteger<std::uint16_t>(payload, cursor) : 0U;
        if (stack.damage > gameplay::itemMaximumDamage(stack))
            throw std::runtime_error("world.dat contains an over-damaged item");
    };
    for (auto& stack : game.inventory) {
        readStack(stack);
    }
    const auto editCount = readInteger<std::uint64_t>(payload, cursor);
    if (editCount > kMaximumEdits) throw std::runtime_error("world.dat edit count is unreasonable");
    game.edits.reserve(static_cast<std::size_t>(editCount));
    for (std::uint64_t index = 0; index < editCount; ++index) {
        world::PersistentBlockEdit edit;
        edit.x = readInteger<std::int32_t>(payload, cursor);
        edit.y = readInteger<std::int32_t>(payload, cursor);
        edit.z = readInteger<std::int32_t>(payload, cursor);
        const auto block = readBlock(cursor);
        const auto fluidLevel = readInteger<std::uint8_t>(payload, cursor);
        auto orientation = formatVersion >= 3U
            ? readInteger<std::uint8_t>(payload, cursor)
            : static_cast<std::uint8_t>(world::defaultOrientation(block));
        // Crops and farmland reused the per-cell orientation byte as their state
        // slot, masking the low three bits, so the byte legitimately holds 6-7
        // for a mature crop or well-watered farmland — well past the six
        // enumerated facings. Accept the full 0-7 range; above it is corrupt.
        if (edit.y < 0 || edit.y >= 256 || fluidLevel > 8U || orientation > 7U)
            throw std::runtime_error("world.dat contains an invalid block edit");
        bool lit = false;
        if (formatVersion >= 14U) {
            lit = readInteger<std::uint8_t>(payload, cursor) != 0U;
        }
        // A pre-14 save spelled these states as blocks; the palette resolution
        // kept what they meant, and it wins over the fields the old format had
        // no way to fill.
        if (const auto& legacy = lastBlockState; legacy.has_value()) {
            if (legacy->orientation.has_value()) {
                orientation = static_cast<std::uint8_t>(*legacy->orientation);
            }
            lit = lit || legacy->lit;
        }
        edit.state = legacyBlockState(block, orientation, fluidLevel, lit);
        game.edits.push_back(edit);
    }
    if (formatVersion >= 2U) {
        const auto chestCount = readInteger<std::uint64_t>(payload, cursor);
        if (chestCount > kMaximumChests)
            throw std::runtime_error("world.dat chest count is unreasonable");
        game.chests.reserve(static_cast<std::size_t>(chestCount));
        for (std::uint64_t index = 0; index < chestCount; ++index) {
            gameplay::ChestBlockEntity chest;
            chest.position.x = readInteger<std::int32_t>(payload, cursor);
            chest.position.y = readInteger<std::int32_t>(payload, cursor);
            chest.position.z = readInteger<std::int32_t>(payload, cursor);
            if (chest.position.y < 0 || chest.position.y >= 256)
                throw std::runtime_error("world.dat contains an invalid chest position");
            for (auto& stack : chest.items) {
                readStack(stack);
            }
            game.chests.push_back(std::move(chest));
        }
    }
    if (formatVersion >= 9U) {
        readGameRulesBlock(payload, cursor, game.gameRules);
    }
    if (formatVersion >= 10U) {
        readSpawnPointBlock(payload, cursor, game);
    }
    if (formatVersion >= 11U) {
        readWeatherBlock(payload, cursor, game);
    }
    if (formatVersion >= 12U) {
        readEntityBlock(payload, cursor, game.entities);
    }
    if (formatVersion >= 13U) {
        readClockBlock(payload, cursor, game);
    } else {
        // Before format 13 one gameTimeSeconds carried the world tick, the sun
        // and every gameplay timer at once, and it only advanced while
        // doDaylightCycle was on. Both replacements are seeded from it: the
        // server tick from the elapsed seconds, the sun from the same day-cycle
        // conversion the renderer used to do at every read site.
        game.serverTick = static_cast<std::uint64_t>(
            game.gameTimeSeconds * world::DayNightCycle::kTicksPerSecond);
        game.clocks[static_cast<std::size_t>(world::ClockId::Overworld)].totalTicks =
            static_cast<std::uint64_t>(world::DayNightCycle::worldTick(game.gameTimeSeconds));
    }
    if (formatVersion >= 15U) {
        const auto furnaceCount = readInteger<std::uint64_t>(payload, cursor);
        if (furnaceCount > kMaximumChests)
            throw std::runtime_error("world.dat furnace count is unreasonable");
        game.furnaces.reserve(static_cast<std::size_t>(furnaceCount));
        for (std::uint64_t index = 0; index < furnaceCount; ++index) {
            gameplay::FurnaceBlockEntity furnace;
            furnace.position.x = readInteger<std::int32_t>(payload, cursor);
            furnace.position.y = readInteger<std::int32_t>(payload, cursor);
            furnace.position.z = readInteger<std::int32_t>(payload, cursor);
            if (furnace.position.y < 0 || furnace.position.y >= 256)
                throw std::runtime_error("world.dat contains an invalid furnace position");
            readStack(furnace.input);
            readStack(furnace.fuel);
            readStack(furnace.output);
            furnace.burnTicks = readInteger<std::int32_t>(payload, cursor);
            furnace.initialBurnTicks = readInteger<std::int32_t>(payload, cursor);
            furnace.cookTicks = readInteger<std::int32_t>(payload, cursor);
            furnace.cookDurationTicks = readInteger<std::int32_t>(payload, cursor);
            game.furnaces.push_back(std::move(furnace));
        }
    }
    if (formatVersion >= 16U) {
        readDropBlock(payload, cursor, game.itemDrops, game.fallingBlocks);
    }
    if (cursor != payload.size()) throw std::runtime_error("world.dat has trailing data");
}

// Format 17 onwards: a flat sequence of self-describing blocks in any order.
// An owner this build does not know is skipped by its own size, so a save from
// a newer build still opens with everything this one understands.
void loadOwnerBlocks(std::span<const std::uint8_t> payload, std::size_t& cursor,
                     SaveGame& game) {
    const auto readPalette = [&](auto resolve, auto fallback) {
        using Value = decltype(fallback);
        std::vector<Value> entries;
        const auto size = readInteger<std::uint16_t>(payload, cursor);
        if (size == 0U) throw std::runtime_error("world.dat has an empty palette");
        entries.reserve(size);
        for (std::uint16_t index = 0; index < size; ++index) {
            entries.push_back(resolve(readString(payload, cursor)).value_or(fallback));
        }
        return entries;
    };
    const auto blockPalette = readPalette(
        [](std::string_view text) { return blockByName(text); },
        world::Block::Air);
    const auto itemPalette = readPalette(
        [](std::string_view text) -> std::optional<const gameplay::Item*> {
            const auto* item = gameplay::itemFromIdentifier(text);
            return item == nullptr ? std::nullopt
                                   : std::optional<const gameplay::Item*>{item};
        },
        static_cast<const gameplay::Item*>(nullptr));
    SaveReadContext context{game, blockPalette, itemPalette};
    while (cursor < payload.size()) {
        std::size_t peek = cursor;
        const auto header = readBlockHeader(payload, peek, "save");
        const auto* owner = std::ranges::find_if(
            kSaveBlockOwners,
            [&](const SaveBlockOwner& candidate) { return candidate.tag == header.tag; });
        if (owner == kSaveBlockOwners.end() || header.version > owner->version) {
            // An unknown owner, or one whose layout a newer build changed: its
            // size is the whole point of the frame.
            cursor = header.end;
            continue;
        }
        cursor = peek;
        owner->read(payload, cursor, header, context);
        if (cursor != header.end) {
            throw std::runtime_error("world.dat block reader did not consume its block");
        }
    }
}

} // namespace

SaveGame SaveRepository::load(const std::string& identifier) const {
    if (!safeIdentifier(identifier)) throw std::invalid_argument("Unsafe save identifier");
    const auto directory = root_ / identifier;
    SaveGame game;
    game.summary = summaryFromProperties(directory / "level.properties", identifier);
    std::ifstream input{directory / "world.dat", std::ios::binary | std::ios::ate};
    if (!input) throw std::runtime_error("Unable to open world.dat");
    const auto length = input.tellg();
    if (length < static_cast<std::streamoff>(kMagic.size() + sizeof(std::uint64_t)))
        throw std::runtime_error("world.dat is truncated");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!input) throw std::runtime_error("Unable to read world.dat");
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
        throw std::runtime_error("world.dat has an invalid header");
    std::size_t checksumCursor = bytes.size() - sizeof(std::uint64_t);
    std::size_t checksumReadCursor = checksumCursor;
    const auto storedChecksum = readInteger<std::uint64_t>(bytes, checksumReadCursor);
    if (checksum(std::span<const std::uint8_t>{bytes}.first(checksumCursor)) != storedChecksum)
        throw std::runtime_error("world.dat checksum mismatch");
    const std::span<const std::uint8_t> payload{bytes.data(), checksumCursor};
    std::size_t cursor = kMagic.size();
    const auto formatVersion = readInteger<std::uint32_t>(payload, cursor);
    if (formatVersion < kOldestSupportedFormatVersion ||
        formatVersion > kFormatVersion)
        throw std::runtime_error("Unsupported world.dat version");
    game.summary.seed = readInteger<std::uint64_t>(payload, cursor);
    if (formatVersion >= kFirstOwnerDrivenFormatVersion) {
        loadOwnerBlocks(payload, cursor, game);
        // M-3 region files: a save made since the region layout carries its edits
        // and creatures in region/, not in world.dat (CHNK and ENTY are only read
        // for the pre-region saves). Union the two sources so both open.
        readRegionDirectory(directory, game);
        // gameTimeSeconds stopped being persisted with format 17 — the server
        // tick and the named clocks carry the time now — but the field is still
        // read in a couple of places, so it is derived rather than left at zero.
        game.gameTimeSeconds =
            static_cast<double>(game.serverTick) / world::DayNightCycle::kTicksPerSecond;
    } else {
        loadLegacy(payload, cursor, formatVersion, game);
    }
    return game;
}


} // namespace mc::persistence

#include "persistence/SaveRepository.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace mc::persistence {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{'M', 'C', 'R', 'B', 'S', 'A', 'V', 'E'};
// Format 8 moved `randomTickSpeed` into a fixed header field; format 9 replaces
// that with a sparse, self-describing GameRules block after the chests section.
constexpr std::uint32_t kFormatVersion = 10U;
constexpr std::uint32_t kOldestSupportedFormatVersion = 1U;
constexpr std::uint64_t kMaximumEdits = 16U * 1024U * 1024U;
constexpr std::uint64_t kMaximumChests = 1024U * 1024U;
// Format 5 stopped writing raw enum ordinals and started writing a palette of
// namespaced identifiers, so blocks may be added, removed or reordered freely.
// Format 6 gave items the same treatment.
constexpr std::uint32_t kFirstBlockPaletteFormatVersion = 5U;
constexpr std::uint32_t kFirstItemPaletteFormatVersion = 6U;
constexpr std::uint32_t kMaximumPaletteEntries = 65535U;
constexpr std::size_t kMaximumIdentifierLength = 256U;

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

// Collects the registry entries a save actually mentions and hands out palette
// indices. `Empty` is always index 0, so an index a reader cannot resolve still
// means "nothing here".
template <typename Value, Value Empty>
class RegistryPalette final {
  public:
    [[nodiscard]] std::uint16_t indexOf(Value value) {
        const auto existing = indices_.find(value);
        if (existing != indices_.end()) return existing->second;
        const auto index = static_cast<std::uint16_t>(entries_.size());
        entries_.push_back(value);
        indices_.emplace(value, index);
        return index;
    }

    [[nodiscard]] std::span<const Value> entries() const { return entries_; }

  private:
    std::vector<Value> entries_{Empty};
    std::unordered_map<Value, std::uint16_t> indices_{{Empty, 0U}};
};

using BlockPalette = RegistryPalette<world::Block, world::Block::Air>;
// Items are keyed by their registered instance; nullptr is the block sentinel
// and, as always, palette index 0.
using ItemPalette = RegistryPalette<const gameplay::Item*, nullptr>;

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

template <typename Integer>
void appendInteger(std::vector<std::uint8_t>& bytes, Integer value) {
    using Unsigned = std::make_unsigned_t<Integer>;
    const Unsigned converted = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        bytes.push_back(static_cast<std::uint8_t>(converted >> (index * 8U)));
    }
}

template <typename Integer>
[[nodiscard]] Integer readInteger(
    std::span<const std::uint8_t> bytes, std::size_t& cursor) {
    if (cursor + sizeof(Integer) > bytes.size()) {
        throw std::runtime_error("Save data ended unexpectedly");
    }
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned value = 0;
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        value |= static_cast<Unsigned>(bytes[cursor++]) << (index * 8U);
    }
    return static_cast<Integer>(value);
}

void appendString(std::vector<std::uint8_t>& bytes, std::string_view text) {
    appendInteger(bytes, static_cast<std::uint16_t>(text.size()));
    bytes.insert(bytes.end(), text.begin(), text.end());
}

[[nodiscard]] std::string readString(std::span<const std::uint8_t> bytes, std::size_t& cursor) {
    const auto length = readInteger<std::uint16_t>(bytes, cursor);
    if (length > kMaximumIdentifierLength || cursor + length > bytes.size()) {
        throw std::runtime_error("Save data contains an oversized string");
    }
    std::string text(reinterpret_cast<const char*>(bytes.data() + cursor), length);
    cursor += length;
    return text;
}

void appendFloat(std::vector<std::uint8_t>& bytes, float value) {
    appendInteger(bytes, std::bit_cast<std::uint32_t>(value));
}
void appendDouble(std::vector<std::uint8_t>& bytes, double value) {
    appendInteger(bytes, std::bit_cast<std::uint64_t>(value));
}
[[nodiscard]] float readFloat(std::span<const std::uint8_t> bytes, std::size_t& cursor) {
    return std::bit_cast<float>(readInteger<std::uint32_t>(bytes, cursor));
}
[[nodiscard]] double readDouble(std::span<const std::uint8_t> bytes, std::size_t& cursor) {
    return std::bit_cast<double>(readInteger<std::uint64_t>(bytes, cursor));
}

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

void SaveRepository::save(SaveGame game) const {
    if (!safeIdentifier(game.summary.identifier)) {
        throw std::invalid_argument("Unsafe save identifier");
    }
    game.summary.displayName = sanitizeDisplayName(std::move(game.summary.displayName));
    game.summary.lastPlayedUnixSeconds = nowUnixSeconds();
    std::vector<std::uint8_t> bytes{kMagic.begin(), kMagic.end()};
    appendInteger(bytes, kFormatVersion);
    appendInteger(bytes, game.summary.seed);
    appendInteger(bytes, static_cast<std::uint8_t>(game.hasPlayerPosition ? 1U : 0U));
    appendFloat(bytes, game.playerX);
    appendFloat(bytes, game.playerY);
    appendFloat(bytes, game.playerZ);
    appendDouble(bytes, game.gameTimeSeconds);
    appendInteger(bytes, static_cast<std::uint8_t>(game.gameMode));
    appendInteger(bytes, static_cast<std::uint8_t>(game.selectedHotbarSlot));
    appendInteger(bytes, static_cast<std::uint8_t>(game.difficulty));
    appendFloat(bytes, game.playerHealth);
    appendInteger(bytes, game.playerFoodLevel);
    appendFloat(bytes, game.playerSaturation);
    appendInteger(bytes, game.playerAirTicks);
    // Everything past this point refers to blocks and items by palette index, so
    // both palettes are gathered first and written ahead of their first use.
    BlockPalette blockPalette;
    ItemPalette itemPalette;
    for (const auto& stack : game.inventory) {
        static_cast<void>(blockPalette.indexOf(stack.block));
        static_cast<void>(itemPalette.indexOf(stack.item));
    }
    for (const auto& edit : game.edits) {
        static_cast<void>(blockPalette.indexOf(edit.block));
    }
    for (const auto& chest : game.chests) {
        for (const auto& stack : chest.items) {
            static_cast<void>(blockPalette.indexOf(stack.block));
            static_cast<void>(itemPalette.indexOf(stack.item));
        }
    }
    if (blockPalette.entries().size() > kMaximumPaletteEntries ||
        itemPalette.entries().size() > kMaximumPaletteEntries) {
        throw std::runtime_error("Save references more content than a palette can hold");
    }
    appendInteger(bytes, static_cast<std::uint16_t>(blockPalette.entries().size()));
    for (const auto block : blockPalette.entries()) {
        // The lit furnace is a transient state (its burn is not saved), so it
        // persists as the plain furnace and reloads unlit.
        const auto& definition = block == world::Block::LitFurnace
            ? world::blockDefinition(world::Block::Furnace)
            : world::blockDefinition(block);
        appendString(bytes, definition.identifier.toString());
    }
    appendInteger(bytes, static_cast<std::uint16_t>(itemPalette.entries().size()));
    for (const auto* item : itemPalette.entries()) {
        // The block sentinel (nullptr) writes an empty string, which resolves
        // back to nullptr on load.
        appendString(bytes, item == nullptr ? std::string{} : item->identifier.toString());
    }
    for (const auto& stack : game.inventory) {
        appendInteger(bytes, blockPalette.indexOf(stack.block));
        appendInteger(bytes, stack.count);
        appendInteger(bytes, itemPalette.indexOf(stack.item));
        appendInteger(bytes, stack.damage);
    }
    appendInteger(bytes, static_cast<std::uint64_t>(game.edits.size()));
    for (const auto& edit : game.edits) {
        appendInteger(bytes, static_cast<std::int32_t>(edit.x));
        appendInteger(bytes, static_cast<std::int32_t>(edit.y));
        appendInteger(bytes, static_cast<std::int32_t>(edit.z));
        appendInteger(bytes, blockPalette.indexOf(edit.block));
        appendInteger(bytes, edit.fluidLevel);
        appendInteger(bytes, static_cast<std::uint8_t>(edit.orientation));
    }
    appendInteger(bytes, static_cast<std::uint64_t>(game.chests.size()));
    for (const auto& chest : game.chests) {
        appendInteger(bytes, static_cast<std::int32_t>(chest.position.x));
        appendInteger(bytes, static_cast<std::int32_t>(chest.position.y));
        appendInteger(bytes, static_cast<std::int32_t>(chest.position.z));
        for (const auto& stack : chest.items) {
            appendInteger(bytes, blockPalette.indexOf(stack.block));
            appendInteger(bytes, stack.count);
            appendInteger(bytes, itemPalette.indexOf(stack.item));
            appendInteger(bytes, stack.damage);
        }
    }
    appendGameRulesBlock(bytes, game.gameRules);
    appendSpawnPointBlock(bytes, game);
    appendInteger(bytes, checksum(bytes));
    const auto directory = root_ / game.summary.identifier;
    replaceFile(directory / "world.dat", bytes);
    writeMetadata(directory / "level.properties", game.summary);
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
    std::vector<const gameplay::Item*> itemPalette;
    if (palettedBlocks) {
        blockPalette = readPalette(
            [](std::string_view text) { return world::blockFromIdentifier(text); },
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
    const auto readBlock = [&](std::size_t& readCursor) {
        if (!palettedBlocks) {
            return legacyBlockFromOrdinal(readInteger<std::uint8_t>(payload, readCursor));
        }
        const auto index = readInteger<std::uint16_t>(payload, readCursor);
        if (index >= blockPalette.size())
            throw std::runtime_error("world.dat references a block outside the palette");
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
        edit.block = readBlock(cursor);
        edit.fluidLevel = readInteger<std::uint8_t>(payload, cursor);
        const auto orientation = formatVersion >= 3U
            ? readInteger<std::uint8_t>(payload, cursor)
            : static_cast<std::uint8_t>(world::defaultOrientation(edit.block));
        // Crops and farmland reuse the per-cell orientation byte as their state
        // slot — cropAge/farmlandMoisture mask the low three bits — so the byte
        // legitimately holds 6-7 for a mature crop or well-watered farmland, well
        // past the six enumerated BlockOrientation facings. Accept the full
        // 0-7 state range; anything above it is still a corrupt save.
        if (edit.y < 0 || edit.y >= 256 || edit.fluidLevel > 8U || orientation > 7U)
            throw std::runtime_error("world.dat contains an invalid block edit");
        edit.orientation = static_cast<world::BlockOrientation>(orientation);
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
    if (cursor != payload.size()) throw std::runtime_error("world.dat has trailing data");
    return game;
}

} // namespace mc::persistence

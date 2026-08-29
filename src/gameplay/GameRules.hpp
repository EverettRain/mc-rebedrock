#pragma once

#include "gameplay/CommandResult.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace mc::gameplay {

// The value payload of a game rule on the wire. `Boolean` and `Int` are written
// today; `Float` and `Compound` are reserved so a future rule can add a typed
// payload without breaking the block framing (each entry carries its own type
// tag, so a reader skips what it does not understand).
enum class GameRuleType : std::uint8_t {
    Boolean = 0,
    Int = 1,
};

// The typed value every rule stores, held in-place so a world's rules cost no
// per-rule heap allocation. This is the C++ equivalent of vanilla's generic
// rule value: one value whose exact type is fixed by the rule's registration.
using GameRuleValueData = std::variant<bool, std::int32_t>;

// One rule's static identity, the way vanilla's static rule keys name them.
// `values_` is indexed by this enum, so lookups are an array subscript plus a
// `std::get` — no map, no virtual call. Ordered the way 26.1's registry orders
// them (alphabetical by id); the order is free to change because nothing but
// this build's own array indices depends on it — the save keys entries by name.
enum class GameRuleId : std::uint8_t {
    AdvanceTime,
    AdvanceWeather,
    BlockDrops,
    DrowningDamage,
    FallDamage,
    FireDamage,
    FireSpreadRadiusAroundPlayer,
    KeepInventory,
    MaxBlockModifications,
    MaxCommandForks,
    MaxCommandSequenceLength,
    MobDrops,
    NaturalHealthRegeneration,
    RandomTickSpeed,
    SendCommandFeedback,
    SpawnMobs,
    Count,  // must stay last; the values array is sized by it
};

struct GameRuleDefinition final {
    std::string_view name;
    GameRuleType type;
    GameRuleValueData defaultValue;
    // Int rules reject values below the floor and clamp above the ceiling,
    // mirroring vanilla's integer-rule validation; Boolean rules ignore these.
    std::int32_t minimum = 0;
    std::int32_t maximum = 0;
    // Vanilla's rule category (GameRuleCategory: player/mobs/spawning/drops/
    // updates/chat/misc), kept for the future edit-gamerules screen.
    std::string_view category;
};

// Vanilla's own ceiling for an integer rule registered without an explicit
// maximum (`registerInteger(id, category, default, min)` passes
// Integer.MAX_VALUE). Named so a row that means "no ceiling" reads as such.
inline constexpr std::int32_t kUnboundedRuleMaximum = 2147483647;

// The registry table: one row per rule. Adding a rule = one enumerator in
// `GameRuleId` plus one row here; every other surface (command, save, default)
// derives from this table. Validated at compile time below.
//
// Names, defaults and bounds are 26.1's (GameRules.java). The one deliberate
// deviation is random_tick_speed's ceiling: vanilla leaves it unbounded, this
// build caps it at 1000 because the random-tick pass runs under the world write
// lock, so a typo'd `/gamerule random_tick_speed 100000` would stall the render
// thread rather than merely making the world grow fast. It clamps rather than
// rejects, so the command still succeeds.
inline constexpr std::array<GameRuleDefinition, 16> kGameRuleDefinitions{{
    //     name                              type              default  min  max  category
    {"advance_time",                    GameRuleType::Boolean, true, 0, 0, "updates"},
    {"advance_weather",                 GameRuleType::Boolean, true, 0, 0, "updates"},
    // Whether a broken block rolls its loot table at all — the simulated breaks
    // (an unsupported torch, a decayed leaf) and the mined ones both funnel
    // through GameSession::spawnBlockDrops, which is where this is read.
    {"block_drops",                     GameRuleType::Boolean, true, 0, 0, "drops"},
    {"drowning_damage",                 GameRuleType::Boolean, true, 0, 0, "player"},
    {"fall_damage",                     GameRuleType::Boolean, true, 0, 0, "player"},
    {"fire_damage",                     GameRuleType::Boolean, true, 0, 0, "player"},
    // 26.1's replacement for the retired doFireTick: -1 means "anywhere", 0
    // means "nowhere" (the old doFireTick=false), and a positive value is the
    // block radius around a player inside which fire ticks at all. Gates the
    // whole fire tick — spread, aging and burning out alike — the way
    // ServerLevel#canSpreadFireAround gates FireBlock#tick.
    {"fire_spread_radius_around_player", GameRuleType::Int, std::int32_t{128}, -1,
     kUnboundedRuleMaximum, "updates"},
    {"keep_inventory",                  GameRuleType::Boolean, false, 0, 0, "player"},
    // The /fill and /setblock write budget. The default is vanilla's, and used
    // to live here as a hardcoded kFillLimit constant.
    {"max_block_modifications",         GameRuleType::Int, std::int32_t{32768}, 1,
     kUnboundedRuleMaximum, "misc"},
    // The command-tree fork ceiling (`execute as @e …` fanning out) and the
    // per-invocation command budget a /function may spend. Both defaults were
    // already vanilla's, as CommandDispatcher and FunctionManager's own
    // hardcoded ceilings.
    {"max_command_forks",               GameRuleType::Int, std::int32_t{65536}, 0,
     kUnboundedRuleMaximum, "misc"},
    {"max_command_sequence_length",     GameRuleType::Int, std::int32_t{65536}, 0,
     kUnboundedRuleMaximum, "misc"},
    {"mob_drops",                       GameRuleType::Boolean, true, 0, 0, "drops"},
    {"natural_health_regeneration",     GameRuleType::Boolean, true, 0, 0, "player"},
    {"random_tick_speed",               GameRuleType::Int,     std::int32_t{3}, 0, 1000, "updates"},
    // Whether a successful command reports back to its sender's chat; failures
    // always report. Default true, so it is absent from a save until turned off
    // (the block is sparse), keeping older worlds byte-identical.
    {"send_command_feedback",           GameRuleType::Boolean, true, 0, 0, "chat"},
    {"spawn_mobs",                      GameRuleType::Boolean, true, 0, 0, "spawning"},
}};

constexpr bool gameRuleDefinitionsAreWellFormed() {
    if (kGameRuleDefinitions.size() != static_cast<std::size_t>(GameRuleId::Count)) {
        return false;
    }
    for (std::size_t index = 0; index < kGameRuleDefinitions.size(); ++index) {
        const auto& definition = kGameRuleDefinitions[index];
        if (definition.name.empty()) return false;
        if (definition.type == GameRuleType::Int) {
            if (definition.minimum > definition.maximum) return false;
            const auto defaultValue = std::get<std::int32_t>(definition.defaultValue);
            if (defaultValue < definition.minimum || defaultValue > definition.maximum) {
                return false;
            }
        } else if (!std::holds_alternative<bool>(definition.defaultValue)) {
            return false;
        }
        for (std::size_t other = 0; other < index; ++other) {
            if (kGameRuleDefinitions[other].name == definition.name) return false;
        }
    }
    return true;
}
static_assert(gameRuleDefinitionsAreWellFormed(),
              "kGameRuleDefinitions must be sized, named, bounded, and uniquely identified");

// Matching is case-insensitive: the command should not care how a name was
// typed. It is *not* separator-insensitive — `randomTickSpeed` and
// `random_tick_speed` are different strings, which is exactly why the legacy
// alias table below has to exist rather than falling out of this.
constexpr bool ruleNameEqual(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto toLower = [](char character) {
            return character >= 'A' && character <= 'Z'
                ? static_cast<char>(character - 'A' + 'a')
                : character;
        };
        if (toLower(left[index]) != toLower(right[index])) return false;
    }
    return true;
}

// The camelCase names this build wrote before it moved to 26.1's snake_case
// registry, paired with the rule each one became. This is the C++ equivalent of
// vanilla's own GameRuleRegistryFix, and it exists for exactly one reason: the
// world.dat game-rules block keys its sparse entries *by name*, so a world
// saved by an older build carries `doDaylightCycle` and would otherwise be
// skipped as an unknown rule and silently revert to the default on load.
//
// Deliberately consumed by the save path only (`applyDecoded`). The command
// surface and its completion offer the 26.1 names alone, so a legacy name is
// readable but never suggested and never written back — one load through a
// current build migrates the world.
struct LegacyGameRuleAlias final {
    std::string_view legacyName;
    GameRuleId id;
};

inline constexpr std::array<LegacyGameRuleAlias, 5> kLegacyGameRuleAliases{{
    {"doDaylightCycle", GameRuleId::AdvanceTime},
    {"doWeatherCycle", GameRuleId::AdvanceWeather},
    {"keepInventory", GameRuleId::KeepInventory},
    {"randomTickSpeed", GameRuleId::RandomTickSpeed},
    {"sendCommandFeedback", GameRuleId::SendCommandFeedback},
}};

// Resolves a legacy save-file name to its current rule, or `Count` when the
// name was never one of this build's own.
[[nodiscard]] constexpr GameRuleId legacyGameRuleIdFromName(std::string_view name) {
    for (const auto& alias : kLegacyGameRuleAliases) {
        if (ruleNameEqual(alias.legacyName, name)) {
            return alias.id;
        }
    }
    return GameRuleId::Count;
}

// Resolves a rule name to its identity. Linear scan over the small table, the
// same deliberate choice EntityTypeRegistry makes (no string-keyed dispatch
// map). Returns `Count` for an unknown name.
[[nodiscard]] constexpr GameRuleId gameRuleIdFromName(std::string_view name) {
    for (std::size_t index = 0; index < kGameRuleDefinitions.size(); ++index) {
        if (ruleNameEqual(kGameRuleDefinitions[index].name, name)) {
            return static_cast<GameRuleId>(index);
        }
    }
    return GameRuleId::Count;
}

// Per-world game rules. Holds one typed value per registered rule in a flat
// array, so reading a rule is an array subscript plus a `std::get` — cheap
// enough to call on a frame boundary, and the hot random-tick loop stays on
// WorldSimulation's mirrored int, the same way vanilla passes the value into
// the chunk tick instead of querying the rule per block.
class GameRules final {
  public:
    // Invoked whenever a rule's value changes through `set`/`setFromCommand`.
    // The owner registers one handler per world and mirrors rules into the
    // gameplay systems that consume them.
    using ChangeHandler = std::function<void(GameRuleId, const GameRuleValueData&)>;

    GameRules();

    // Generic typed access. The value type is fixed by the rule's
    // registration; requesting a rule through the wrong `T` is a programming
    // error.
    template <typename T>
    [[nodiscard]] const T& get(GameRuleId id) const {
        return std::get<T>(values_[static_cast<std::size_t>(id)]);
    }

    // Sets a rule's value, honoring the Int floor/ceiling and firing the change
    // handler. Returns false when the value was rejected (below the floor);
    // values above the ceiling are clamped, matching the pre-existing
    // `/gamerule randomTickSpeed` behavior.
    template <typename T>
    [[nodiscard]] bool set(GameRuleId id, T value) {
        const auto& definition = kGameRuleDefinitions[static_cast<std::size_t>(id)];
        if constexpr (std::is_same_v<T, std::int32_t>) {
            if (value < definition.minimum) return false;
            value = std::min(value, definition.maximum);
        }
        values_[static_cast<std::size_t>(id)] = value;
        if (onChange_) onChange_(id, values_[static_cast<std::size_t>(id)]);
        return true;
    }

    [[nodiscard]] const GameRuleValueData& value(GameRuleId id) const {
        return values_[static_cast<std::size_t>(id)];
    }

    void setChangeHandler(ChangeHandler handler) { onChange_ = std::move(handler); }

    // `/gamerule <rule> <value>`: parses `valueText` by the rule's type and
    // applies it. Accepts the bare name, a `minecraft:` prefix, and any case,
    // the way vanilla's /gamerule resolves namespaced names.
    [[nodiscard]] CommandResult setFromCommand(std::string_view name,
                                               std::string_view valueText);

    // `/gamerule <rule>`: reports the current value.
    [[nodiscard]] CommandResult query(std::string_view name) const;

    // Applies an entry decoded from the save. Returns false when the name is
    // unknown or the type does not match, so the reader can skip the entry and
    // keep the default. No change handler fires here: it runs while a save
    // loads, and the renderer re-applies the rule set to its systems
    // afterwards.
    [[nodiscard]] bool applyDecoded(std::string_view name, GameRuleType type,
                                    const GameRuleValueData& decoded);

  private:
    std::array<GameRuleValueData, static_cast<std::size_t>(GameRuleId::Count)> values_;
    ChangeHandler onChange_;
};

} // namespace mc::gameplay

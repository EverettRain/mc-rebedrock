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
// per-rule heap allocation. This is the C++ equivalent of 1.16.1's
// `GameRules.Rule<T>` (Bedrock's `GameRules.Value<T>`): one generic value whose
// exact type is fixed by the rule's registration.
using GameRuleValueData = std::variant<bool, std::int32_t>;

// One rule's static identity, the way 1.16.1's `static final Key<T>` fields
// name the rules. `values_` is indexed by this enum, so lookups are an array
// subscript plus a `std::get` — no map, no virtual call.
enum class GameRuleId : std::uint8_t {
    DoDaylightCycle,
    KeepInventory,
    RandomTickSpeed,
    Count,  // must stay last; the values array is sized by it
};

struct GameRuleDefinition final {
    std::string_view name;
    GameRuleType type;
    GameRuleValueData defaultValue;
    // Int rules reject values below the floor and clamp above the ceiling
    // (mirroring 1.16.1's `IntegerRule#validate`); Boolean rules ignore these.
    std::int32_t minimum = 0;
    std::int32_t maximum = 0;
    // 1.16.1's GameRules.Category, kept for the future edit-gamerules screen.
    std::string_view category;
};

// The registry table: one row per rule. Adding a rule = one enumerator in
// `GameRuleId` plus one row here; every other surface (command, save, default)
// derives from this table. Validated at compile time below.
inline constexpr std::array<GameRuleDefinition, 3> kGameRuleDefinitions{{
    //     name                 type              default  min  max  category
    {"doDaylightCycle",        GameRuleType::Boolean, true, 0, 0, "updates"},
    {"keepInventory",          GameRuleType::Boolean, false, 0, 0, "players"},
    {"randomTickSpeed",        GameRuleType::Int,     std::int32_t{3}, 0, 1000, "updates"},
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

// 1.16.1 rule names are camelCase (`randomTickSpeed`), but the command should
// not care how the player typed the name, so matching is case-insensitive.
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
// WorldSimulation's mirrored int exactly as 1.16.1's `tickChunk(chunk, int)`
// passes the value into the loop instead of querying it per block.
class GameRules final {
  public:
    // Invoked whenever a rule's value changes through `set`/`setFromCommand`.
    // The owner registers one handler per world and mirrors rules into the
    // gameplay systems that consume them.
    using ChangeHandler = std::function<void(GameRuleId, const GameRuleValueData&)>;

    GameRules();

    // Generic typed access — the C++ equivalent of 1.16.1's `GameRules.Rule<T>`.
    // The value type is fixed by the rule's registration; requesting a rule
    // through the wrong `T` is a programming error.
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
    // the way 1.16.1's GameRuleCommand resolves namespaced names.
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

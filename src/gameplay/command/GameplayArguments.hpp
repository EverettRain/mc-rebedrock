#pragma once

#include "gameplay/command/EntitySelector.hpp"
#include "gameplay/command/IdentifierTable.hpp"

#include "gameplay/Difficulty.hpp"
#include "gameplay/GameMode.hpp"
#include "gameplay/EnchantmentRegistry.hpp"
#include "gameplay/GameRules.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "gameplay/StatusEffect.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"
#include "world/WorldClock.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mc::gameplay::command {

[[nodiscard]] inline std::string lowercase(std::string_view text) {
    std::string result{text};
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return result;
}

// `/time set`'s argument, which 26.1 splits across two sibling nodes: a numeric
// `time` and a `timemarker` id. This tree gives a node one argument child, so
// the two arrive as one discriminated value instead — and they have to stay
// discriminated, because they do genuinely different things. A number is an
// absolute tick to land on (TimeCommand#setTotalTicks); a marker moves the clock
// *forward* to that point's next occurrence (#setTimeToTimeMarker), which is why
// `/time set day` never winds the calendar back a day.
struct TimeSpec final {
    std::int64_t ticks = 0;
    // Set when the token named a marker rather than a tick count.
    std::optional<world::ClockTimeMarker> marker;
};

// The markers 26.1 registers for the overworld day (ClockTimeMarkers), by the
// names `/time set` accepts. Kept here rather than in the argument class so the
// completion and the parse read one list.
inline constexpr std::array<std::pair<std::string_view, world::ClockTimeMarker>, 4>
    kClockTimeMarkerNames{{
        {"day", world::ClockTimeMarker::Day},
        {"noon", world::ClockTimeMarker::Noon},
        {"night", world::ClockTimeMarker::Night},
        {"midnight", world::ClockTimeMarker::Midnight},
    }};

// 26.1's TimeArgument units: a bare number is ticks, `d` days, `s` seconds,
// `t` ticks. The value before the unit may be fractional (`0.5d`), so the parse
// runs through a double and rounds the way Mth.round does.
inline constexpr std::array<std::pair<std::string_view, int>, 4> kTimeUnits{{
    {"", 1}, {"t", 1}, {"s", 20}, {"d", 24'000},
}};

// Parses `/time`'s time token: a marker name, or a number with an optional unit
// suffix. `minimum` is the floor the resulting tick count must clear — 0 for
// `set`, negative for `add`, mirroring TimeArgument.time(int).
[[nodiscard]] inline std::optional<TimeSpec> parseTimeOfDay(std::string_view value,
                                                            std::int64_t minimum = 0) {
    const std::string normalized = lowercase(value);
    for (const auto& [name, marker] : kClockTimeMarkerNames) {
        if (normalized == name) {
            return TimeSpec{static_cast<std::int64_t>(world::timeMarkerTicks(marker)), marker};
        }
    }
    double magnitude = 0.0;
    const auto [end, error] = std::from_chars(
        normalized.data(), normalized.data() + normalized.size(), magnitude);
    if (error != std::errc{}) {
        return std::nullopt;
    }
    const std::string_view suffix{end, static_cast<std::size_t>(
                                           normalized.data() + normalized.size() - end)};
    const auto unit = std::ranges::find_if(
        kTimeUnits, [&](const auto& entry) { return entry.first == suffix; });
    if (unit == kTimeUnits.end()) {
        return std::nullopt;
    }
    const auto ticks = static_cast<std::int64_t>(
        std::llround(magnitude * static_cast<double>(unit->second)));
    if (ticks < minimum) {
        return std::nullopt;
    }
    return TimeSpec{ticks, std::nullopt};
}

// ---- Table adapters ---------------------------------------------------------
// Each implements the visitor contract IdentifierTable.hpp documents. Commands
// complete to the project's own `rebedrock:` namespace — the single outward
// identity of the game's content. The `minecraft:` alias registries carry for
// vanilla mirroring stays accepted at parse time (for familiar input),
// but it is never suggested.

class GameModeTable final {
  public:
    template <typename F>
    void forEach(F&& visitor) const {
        visitor(TableEntry{"survival", "Survival"});
        visitor(TableEntry{"creative", "Creative"});
    }

    [[nodiscard]] bool contains(std::string_view identifier) const {
        return gameplay::parseGameMode(identifier).has_value();
    }

    [[nodiscard]] std::string_view kind() const { return "game mode"; }
};

class BlockTable final {
  public:
    template <typename F>
    void forEach(F&& visitor) const {
        for (const auto& definition : world::kBlockRegistry) {
            const Item* item = blockItemFor(definition.block);
            if (item == nullptr) continue;
            const std::string descriptionId = encodeDescriptionId(item->descriptionId());
            visitor(TableEntry{definition.identifier.toString(), descriptionId});
        }
    }

    [[nodiscard]] bool contains(std::string_view identifier) const {
        return world::blockFromIdentifier(identifier).has_value();
    }

    [[nodiscard]] std::string_view kind() const { return "block"; }
};

class ItemTable final {
  public:
    // Every item `/give`-style commands accept: the constexpr item registry, the
    // runtime slot (spawn eggs), and every block wielded as its BlockItem.
    template <typename F>
    void forEach(F&& visitor) const {
        const auto visitItem = [&](const Item* item) {
            if (item == nullptr) return;
            const std::string descriptionId = encodeDescriptionId(item->descriptionId());
            visitor(TableEntry{item->identifier.toString(), descriptionId});
        };
        for (const Item* item : kItemRegistry) {
            visitItem(item);
        }
        for (const Item* item : extraItemRegistry()) {
            visitItem(item);
        }
        for (const auto& definition : world::kBlockRegistry) {
            const Item* item = blockItemFor(definition.block);
            if (item == nullptr) continue;
            const std::string descriptionId = encodeDescriptionId(item->descriptionId());
            visitor(TableEntry{definition.identifier.toString(), descriptionId});
        }
    }

    [[nodiscard]] bool contains(std::string_view identifier) const {
        return gameplay::itemFromIdentifier(identifier) != nullptr;
    }

    [[nodiscard]] std::string_view kind() const { return "item"; }
};

class EntityTable final {
  public:
    template <typename F>
    void forEach(F&& visitor) const {
        for (const entities::EntityType* type : entities::entityTypeRegistry().all()) {
            if (type == nullptr) continue;
            visitor(TableEntry{type->id().toString(), ""});
        }
    }

    [[nodiscard]] bool contains(std::string_view identifier) const {
        return entities::entityTypeRegistry().byId(identifier) != nullptr;
    }

    [[nodiscard]] std::string_view kind() const { return "entity"; }
};

// `/effect give <targets> <effect>`: the registry is the single list, walked by
// dense id so a datapack-registered effect completes with no extra wiring.
class StatusEffectTable final {
  public:
    template <typename F>
    void forEach(F&& visitor) const {
        for (std::size_t index = 0; index < statusEffectCount(); ++index) {
            const core::StatusEffectId id = statusEffectAt(index);
            if (!id.valid()) continue;
            visitor(TableEntry{std::string{core::kNamespace} + ":" +
                                   std::string{statusEffectName(id)},
                               ""});
        }
    }

    [[nodiscard]] bool contains(std::string_view identifier) const {
        return statusEffectByName(identifier).valid();
    }

    [[nodiscard]] std::string_view kind() const { return "effect"; }
};

// `/enchant <targets> <enchantment>`: same shape over the enchantment registry,
// whose dense ids are likewise registry subscripts.
class EnchantmentTable final {
  public:
    template <typename F>
    void forEach(F&& visitor) const {
        const auto& registry = enchantmentRegistry();
        for (std::size_t index = 0; index < registry.size(); ++index) {
            const auto id =
                core::EnchantmentTypeId::of(static_cast<core::EnchantmentTypeId::Value>(index));
            visitor(TableEntry{registry.identifier(id).toString(), ""});
        }
    }

    [[nodiscard]] bool contains(std::string_view identifier) const {
        return enchantmentTypeByName(identifier).valid();
    }

    [[nodiscard]] std::string_view kind() const { return "enchantment"; }
};

class GameRuleTable final {
  public:
    template <typename F>
    void forEach(F&& visitor) const {
        for (const auto& definition : kGameRuleDefinitions) {
            visitor(TableEntry{definition.name, definition.category});
        }
    }

    [[nodiscard]] bool contains(std::string_view identifier) const {
        return gameRuleIdFromName(identifier) != GameRuleId::Count;
    }

    [[nodiscard]] std::string_view kind() const { return "game rule"; }
};

// ---- Command argument types -------------------------------------------------

// `/gamemode <survival|creative>`, binding the typed mode (accepts the `s`/`c`
// aliases the old registry did). Suggests the two canonical names.
class GameModeArgument final : public ArgumentType {
  public:
    ArgumentParseResult parse(StringReader& reader) const override {
        const auto token = reader.readString();
        if (!token.has_value()) {
            return parseFail("Expected a game mode", reader);
        }
        const auto mode = gameplay::parseGameMode(*token);
        if (!mode.has_value()) {
            return parseFail("Unknown game mode: " + *token, reader);
        }
        return parseOk(*mode);
    }

    void collectSuggestions(SuggestionSink& sink, const CommandContext&) const override {
        sink.suggest("survival", "Survival");
        sink.suggest("creative", "Creative");
    }
};

// `/time set <day|noon|night|midnight|<ticks>>`, binding the resolved tick count.
class TimeArgument final : public ArgumentType {
  public:
    // `minimum` is TimeArgument.time(int)'s floor: `set` refuses a negative
    // absolute time, `add` accepts one because winding a clock back is the
    // point of an offset.
    constexpr TimeArgument() = default;
    constexpr explicit TimeArgument(std::int64_t minimum, bool markersAllowed = true)
        : minimum_(minimum), markersAllowed_(markersAllowed) {}

    ArgumentParseResult parse(StringReader& reader) const override {
        const auto token = reader.readString();
        if (!token.has_value()) {
            return parseFail("Invalid time", reader);
        }
        const auto spec = parseTimeOfDay(*token, minimum_);
        if (!spec.has_value() || (spec->marker.has_value() && !markersAllowed_)) {
            return parseFail("Invalid time: " + *token, reader);
        }
        return parseOk(*spec);
    }

    void collectSuggestions(SuggestionSink& sink, const CommandContext&) const override {
        if (markersAllowed_) {
            for (const auto& [name, marker] : kClockTimeMarkerNames) {
                sink.suggest(std::string{name}, std::to_string(world::timeMarkerTicks(marker)));
            }
        }
        // The units, shown against a magnitude so the hint reads as an example.
        sink.suggest("1d", "24000 ticks");
        sink.suggest("1s", "20 ticks");
        sink.suggest("1t", "1 tick");
    }

  private:
    std::int64_t minimum_ = 0;
    bool markersAllowed_ = true;
};

// `/time of <clock> …`: the named clock the rest of the line acts on. 26.1 keeps
// these in Registries.WORLD_CLOCK; this build's ClockId enum is the same set,
// index-aligned with DimensionId, so a clock name is a dimension name.
class ClockArgument final : public ArgumentType {
  public:
    ArgumentParseResult parse(StringReader& reader) const override {
        const auto token = reader.readString();
        if (!token.has_value() || token->empty()) {
            return parseFail("Expected a clock", reader);
        }
        const auto clock = clockFromName(*token);
        if (!clock.has_value()) {
            return parseFail("Unknown clock: " + *token, reader);
        }
        return parseOk(*clock);
    }

    void collectSuggestions(SuggestionSink& sink, const CommandContext&) const override {
        for (const auto& [name, clock] : kClockNames) {
            sink.suggest(std::string{name});
        }
    }

    // Accepts the bare name and the `minecraft:` alias, case-insensitively, the
    // way every other registry-backed argument here resolves a name.
    [[nodiscard]] static std::optional<world::ClockId> clockFromName(std::string_view token) {
        std::string normalized = lowercase(token);
        constexpr std::string_view kVanillaNamespace = "minecraft:";
        if (normalized.starts_with(kVanillaNamespace)) {
            normalized.erase(0, kVanillaNamespace.size());
        }
        for (const auto& [name, clock] : kClockNames) {
            if (normalized == name) {
                return clock;
            }
        }
        return std::nullopt;
    }

  private:
    static constexpr std::array<std::pair<std::string_view, world::ClockId>, 3> kClockNames{{
        {"overworld", world::ClockId::Overworld},
        {"the_nether", world::ClockId::Nether},
        {"the_end", world::ClockId::End},
    }};
};

// `/give <item|index>`: a creative-catalog index (any digits) or an item/block
// identifier. Indexes are range-checked by the handler; identifiers are
// validated here against the item and block tables, so a typo fails at parse
// time instead of surfacing mid-execution.
class GiveItemArgument final : public ArgumentType {
  public:
    ArgumentParseResult parse(StringReader& reader) const override {
        const auto token = reader.readString();
        if (!token.has_value()) {
            return parseFail("Expected an item", reader);
        }
        const bool numeric =
            !token->empty() &&
            std::all_of(token->begin(), token->end(), [](char c) { return c >= '0' && c <= '9'; });
        if (numeric || gameplay::itemFromIdentifier(*token) != nullptr ||
            world::blockFromIdentifier(*token).has_value()) {
            return parseOk(*token);
        }
        return parseFail("Unknown item: " + *token, reader);
    }

    void collectSuggestions(SuggestionSink& sink, const CommandContext&) const override {
        ItemTable table;
        table.forEach([&](const TableEntry& entry) { sink.suggest(entry.identifier, entry.hint); });
    }
};

// `/tp`'s destination: either a three-coordinate position (with `~`-relative
// axes, mirroring vanilla's vec3 argument) or a registered entity id to teleport
// onto. The coordinate form binds a Position3, the entity form a std::string;
// the handler branches on whichever is present.
class TeleportDestinationArgument final : public ArgumentType {
  public:
    ArgumentParseResult parse(StringReader& reader) const override {
        // Coordinates read through readCoordinate (a `~`-relative prefix is
        // outside the unquoted-string character set); a non-coordinate token is
        // a registered entity id instead.
        const std::string first = reader.readCoordinate();
        if (!first.empty()) {
            Position3 position;
            if (!parseCoordinate(first, position.x, position.relativeX)) {
                return parseFail("Invalid X coordinate: " + first, reader);
            }
            // The dispatcher skips whitespace once before parse(), so a
            // multi-token argument must skip between its own tokens.
            reader.skipWhitespace();
            const std::string second = reader.readCoordinate();
            if (second.empty() || !parseCoordinate(second, position.y, position.relativeY)) {
                return parseFail("Incomplete position: expected <x> <y> <z>", reader);
            }
            reader.skipWhitespace();
            const std::string third = reader.readCoordinate();
            if (third.empty() || !parseCoordinate(third, position.z, position.relativeZ)) {
                return parseFail("Incomplete position: expected <x> <y> <z>", reader);
            }
            return parseOk(position);
        }
        const auto entity = reader.readString();
        if (!entity.has_value() || entity->empty()) {
            return parseFail("Expected a position or entity", reader);
        }
        if (!EntityTable{}.contains(*entity)) {
            return parseFail("Unknown entity: " + *entity, reader);
        }
        return parseOk(std::string{*entity});
    }

    void collectSuggestions(SuggestionSink& sink, const CommandContext&) const override {
        EntityTable{}.forEach(
            [&](const TableEntry& entry) { sink.suggest(entry.identifier, entry.hint); });
    }

    // One `<pos>` node name stands for three coordinates; show them in usage.
    [[nodiscard]] std::string usageHint() const override { return "<x> <y> <z>"; }
};

// `/kill`'s target: the special `player` keyword (the player) or a registered
// entity id (every spawned creature of that species).
class EntityTargetArgument final : public ArgumentType {
  public:
    ArgumentParseResult parse(StringReader& reader) const override {
        const auto token = reader.readString();
        if (!token.has_value()) {
            return parseFail("Expected a target", reader);
        }
        if (*token == "player" || EntityTable{}.contains(*token)) {
            return parseOk(std::string{*token});
        }
        return parseFail("Unknown target: " + *token, reader);
    }

    void collectSuggestions(SuggestionSink& sink, const CommandContext&) const override {
        sink.suggest("player", "the player");
        EntityTable{}.forEach(
            [&](const TableEntry& entry) { sink.suggest(entry.identifier, entry.hint); });
    }
};

// `@s/@p/@a/@e/@r[filters]`: a real target selector (vanilla's EntitySelector),
// binding a parsed EntitySelector the handler resolves against its player/entity
// pools. Completion is context-aware: the variable list at `@`, the option keys
// inside `[`, and the species registry after `type=` (never a hardcoded list).
class EntitySelectorArgument final : public ArgumentType {
  public:
    ArgumentParseResult parse(StringReader& reader) const override {
        return parseEntitySelector(reader);
    }

    void collectSuggestions(SuggestionSink& sink, const CommandContext&) const override {
        const std::string_view partial = sink.partial();
        const auto bracket = partial.find('[');
        if (bracket == std::string_view::npos) {
            // Completing the variable: the leading `@` prefix-matches these.
            sink.suggest("@s", "the executor");
            sink.suggest("@p", "nearest player");
            sink.suggest("@a", "all players");
            sink.suggest("@e", "all entities");
            sink.suggest("@r", "random");
            return;
        }
        // Inside the filter block: the current option fragment starts after the
        // last ',' (or the '['), and the completion continues the whole token.
        std::size_t optionStart = partial.rfind(',');
        if (optionStart == std::string_view::npos || optionStart < bracket) {
            optionStart = bracket;
        }
        const std::string prefix{partial.substr(0, optionStart + 1U)};
        const std::string_view fragment = partial.substr(optionStart + 1U);
        const auto equals = fragment.find('=');
        if (equals == std::string_view::npos) {
            for (const char* key : {"type=", "distance=", "limit=", "sort="}) {
                sink.suggest(prefix + key);
            }
            return;
        }
        const std::string_view key = fragment.substr(0, equals);
        const std::string valuePrefix = prefix + std::string{fragment.substr(0, equals + 1U)};
        if (key == "type") {
            // Species from the registry; emit both the full id and its bare path
            // so a bare fragment (`co`) completes as well as a namespaced one.
            for (const entities::EntityType* type : entities::entityTypeRegistry().all()) {
                if (type == nullptr) continue;
                const std::string full = type->id().toString();
                sink.suggest(valuePrefix + full);
                if (const auto colon = full.rfind(':'); colon != std::string::npos) {
                    sink.suggest(valuePrefix + full.substr(colon + 1U));
                }
            }
        } else if (key == "sort") {
            for (const char* option : {"nearest", "furthest", "random", "arbitrary"}) {
                sink.suggest(valuePrefix + option);
            }
        }
        // distance / limit are numeric — no candidate list to offer.
    }
};

// `/difficulty <peaceful|easy|normal|hard>`, binding the typed Difficulty.
class DifficultyArgument final : public ArgumentType {
  public:
    ArgumentParseResult parse(StringReader& reader) const override {
        const auto token = reader.readString();
        if (!token.has_value()) {
            return parseFail("Expected a difficulty", reader);
        }
        const auto difficulty = gameplay::difficultyFromName(*token);
        if (!difficulty.has_value()) {
            return parseFail("Unknown difficulty: " + *token, reader);
        }
        return parseOk(*difficulty);
    }

    void collectSuggestions(SuggestionSink& sink, const CommandContext&) const override {
        for (std::uint8_t index = 0; index < gameplay::kDifficultyCount; ++index) {
            sink.suggest(gameplay::difficultyName(static_cast<gameplay::Difficulty>(index)));
        }
    }
};

// A bare coordinate triple (no entity form), binding a Position3 — `execute
// positioned`/`facing` and `execute if block`. `~`-relative axes resolve against
// the source at execution time. Unlike TeleportDestinationArgument this never
// accepts an entity id, so `execute positioned <entity>` is rejected where only
// a position is meaningful.
class Vec3Argument final : public ArgumentType {
  public:
    ArgumentParseResult parse(StringReader& reader) const override {
        Position3 position;
        const std::string first = reader.readCoordinate();
        if (first.empty() || !parseCoordinate(first, position.x, position.relativeX)) {
            return parseFail("Expected <x> <y> <z>", reader);
        }
        // The dispatcher skips whitespace once before parse(); a multi-token
        // argument skips between its own tokens.
        reader.skipWhitespace();
        const std::string second = reader.readCoordinate();
        if (second.empty() || !parseCoordinate(second, position.y, position.relativeY)) {
            return parseFail("Incomplete position: expected <x> <y> <z>", reader);
        }
        reader.skipWhitespace();
        const std::string third = reader.readCoordinate();
        if (third.empty() || !parseCoordinate(third, position.z, position.relativeZ)) {
            return parseFail("Incomplete position: expected <x> <y> <z>", reader);
        }
        return parseOk(position);
    }

    void collectSuggestions(SuggestionSink& sink, const CommandContext&) const override {
        sink.suggest("~", "here");
    }

    // One `<pos>` node name stands for three coordinates; show them in usage.
    [[nodiscard]] std::string usageHint() const override { return "<x> <y> <z>"; }
};

// `execute in <dimension>`: a dimension id. The overworld is the only dimension
// the runtime ticks today, so completion offers it (and its `minecraft:` alias);
// the handler validates the parsed name.
class DimensionArgument final : public ArgumentType {
  public:
    ArgumentParseResult parse(StringReader& reader) const override {
        const auto token = reader.readString();
        if (!token.has_value() || token->empty()) {
            return parseFail("Expected a dimension", reader);
        }
        return parseOk(std::string{*token});
    }

    void collectSuggestions(SuggestionSink& sink, const CommandContext&) const override {
        sink.suggest("overworld");
        sink.suggest("minecraft:overworld");
    }
};

// `/gamerule <rule> <value>`: the value is parsed permissively (GameRules
// validates it by the rule's type at execution time), but completed by that
// type — a boolean rule offers true/false, an int rule its range — derived from
// the `rule` bound earlier on the line. Vanilla registers a per-rule typed value
// node; rebedrock keeps GameRules as the single rule engine and reads the same
// completion off the parsed rule name, so no per-rule tree is duplicated.
class GameRuleValueArgument final : public ArgumentType {
  public:
    ArgumentParseResult parse(StringReader& reader) const override {
        const auto token = reader.readString();
        if (!token.has_value()) {
            return parseFail("Expected a value", reader);
        }
        return parseOk(std::string{*token});
    }

    void collectSuggestions(SuggestionSink& sink, const CommandContext& context) const override {
        const auto rule = context.find<std::string>("rule");
        if (!rule.has_value()) {
            return; // the rule is not typed yet — nothing to specialise on
        }
        const GameRuleId id = gameRuleIdFromName(*rule);
        if (id == GameRuleId::Count) {
            return;
        }
        const auto& definition = kGameRuleDefinitions[static_cast<std::size_t>(id)];
        if (definition.type == GameRuleType::Boolean) {
            sink.suggest("true");
            sink.suggest("false");
        } else if (definition.type == GameRuleType::Int) {
            sink.suggest(std::to_string(definition.minimum),
                         "[" + std::to_string(definition.minimum) + ", " +
                             std::to_string(definition.maximum) + "]");
        }
    }
};

// Shared, stateless instances of the gameplay argument types. One instance
// serves every command that uses a type (vanilla's ArgumentType.instance()).
inline const GameModeArgument kGameModeArgument;
inline const TimeArgument kTimeArgument;
inline const ClockArgument kClockArgument;
// `/time add <n>`: unlike `set`, this one takes a signed offset (26.1 hands its
// TimeArgument a MIN_VALUE floor for exactly this node), so a clock can be wound
// backwards as well as forwards. A marker is meaningless as an offset, so this
// instance refuses one rather than silently reading `day` as +1000.
inline const TimeArgument kTimeOffsetArgument{
    -static_cast<std::int64_t>(1) << 40, /*markersAllowed=*/false};
// `/time rate <r>`: 26.1's floatArg(1.0E-5F, 1000.0F). The floor is above zero
// on purpose — a rate of 0 is what `pause` is for, and would otherwise be an
// unstoppable clock that looks paused.
inline const DoubleArgument kClockRateArgument{1.0e-5, 1000.0};
// `/effect give <targets> <effect> [<seconds>] [<amplifier>]`: 26.1's
// EffectCommand bounds (0..1000000 seconds, amplifier 0..255).
inline const IntArgument kEffectSecondsArgument{0, 1'000'000};
inline const IntArgument kEffectAmplifierArgument{0, 255};
// `/enchant <targets> <enchantment> [<level>]`: the level is bounded loosely
// here and checked against the enchantment's own maxLevel by the handler, which
// is the only place that knows which enchantment was named.
inline const IntArgument kEnchantmentLevelArgument{1, 255};
inline const GiveItemArgument kGiveItemArgument;
inline const TableArgument<GameRuleTable> kGameRuleArgument;
inline const TeleportDestinationArgument kTeleportDestinationArgument;
inline const EntityTargetArgument kEntityTargetArgument;
inline const EntitySelectorArgument kEntitySelectorArgument;
inline const DifficultyArgument kDifficultyArgument;
inline const Vec3Argument kVec3Argument;
inline const DimensionArgument kDimensionArgument;
inline const GameRuleValueArgument kGameRuleValueArgument;
// Block name (setblock/fill) and entity species name (summon): registry-backed,
// so validation and completion follow the content tables with no hardcoded list.
inline const TableArgument<StatusEffectTable> kStatusEffectArgument;
inline const TableArgument<EnchantmentTable> kEnchantmentArgument;
inline const TableArgument<BlockTable> kBlockArgument;
inline const TableArgument<EntityTable> kSummonEntityArgument;
// `/weather clear|rain [<duration>]`: the duration (seconds) is bounded by the
// same 0..1000000 vanilla's /weather hands its integer argument; the
// handler converts it to ticks at 20 per second. A bound is required so a
// seconds value that would overflow when doubled is rejected at parse time.
inline const IntArgument kWeatherDurationArgument{0, 1'000'000};

} // namespace mc::gameplay::command

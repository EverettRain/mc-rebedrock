#pragma once

#include "gameplay/command/EntitySelector.hpp"
#include "gameplay/command/IdentifierTable.hpp"

#include "gameplay/GameMode.hpp"
#include "gameplay/GameRules.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"

#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>

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

// The literal times 1.16.1's TimeArgument accepts plus a raw tick count taken
// modulo a full day (so 24001 resolves to 1, matching the vanilla formatter).
[[nodiscard]] inline std::optional<double> parseTimeOfDay(std::string_view value) {
    const std::string normalized = lowercase(value);
    if (normalized == "day") return 1'000.0;
    if (normalized == "noon") return 6'000.0;
    if (normalized == "night") return 13'000.0;
    if (normalized == "midnight") return 18'000.0;
    unsigned int ticks = 0U;
    const auto [end, error] = std::from_chars(
        normalized.data(), normalized.data() + normalized.size(), ticks);
    if (error != std::errc{} || end != normalized.data() + normalized.size()) {
        return std::nullopt;
    }
    return static_cast<double>(ticks % 24'000U);
}

// ---- Table adapters ---------------------------------------------------------
// Each implements the visitor contract IdentifierTable.hpp documents. Commands
// complete to the project's own `rebedrock:` namespace — the single outward
// identity of the game's content. The `minecraft:` alias registries carry for
// vanilla mirroring stays accepted at parse time (for 1.16.1-familiar input),
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

    void collectSuggestions(SuggestionSink& sink) const override {
        sink.suggest("survival", "Survival");
        sink.suggest("creative", "Creative");
    }
};

// `/time set <day|noon|night|midnight|<ticks>>`, binding the resolved tick count.
class TimeArgument final : public ArgumentType {
  public:
    ArgumentParseResult parse(StringReader& reader) const override {
        const auto token = reader.readString();
        if (!token.has_value()) {
            return parseFail("Invalid time", reader);
        }
        const auto ticks = parseTimeOfDay(*token);
        if (!ticks.has_value()) {
            return parseFail("Invalid time: " + *token, reader);
        }
        return parseOk(*ticks);
    }

    void collectSuggestions(SuggestionSink& sink) const override {
        sink.suggest("day", "1000");
        sink.suggest("noon", "6000");
        sink.suggest("night", "13000");
        sink.suggest("midnight", "18000");
    }
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

    void collectSuggestions(SuggestionSink& sink) const override {
        ItemTable table;
        table.forEach([&](const TableEntry& entry) { sink.suggest(entry.identifier, entry.hint); });
    }
};

// `/tp`'s destination: either a three-coordinate position (with `~`-relative
// axes, mirroring 1.16.1's Vec3Argument) or a registered entity id to teleport
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

    void collectSuggestions(SuggestionSink& sink) const override {
        EntityTable{}.forEach(
            [&](const TableEntry& entry) { sink.suggest(entry.identifier, entry.hint); });
    }
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

    void collectSuggestions(SuggestionSink& sink) const override {
        sink.suggest("player", "the player");
        EntityTable{}.forEach(
            [&](const TableEntry& entry) { sink.suggest(entry.identifier, entry.hint); });
    }
};

// `@s/@p/@a/@e/@r[filters]`: a real target selector (1.16.1's EntitySelector),
// binding a parsed EntitySelector the handler resolves against its player/entity
// pools. Completion is context-aware: the variable list at `@`, the option keys
// inside `[`, and the species registry after `type=` (never a hardcoded list).
class EntitySelectorArgument final : public ArgumentType {
  public:
    ArgumentParseResult parse(StringReader& reader) const override {
        return parseEntitySelector(reader);
    }

    void collectSuggestions(SuggestionSink& sink) const override {
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

// Shared, stateless instances of the gameplay argument types. One instance
// serves every command that uses a type (1.16.1's ArgumentType.instance()).
inline const GameModeArgument kGameModeArgument;
inline const TimeArgument kTimeArgument;
inline const GiveItemArgument kGiveItemArgument;
inline const TableArgument<GameRuleTable> kGameRuleArgument;
inline const TeleportDestinationArgument kTeleportDestinationArgument;
inline const EntityTargetArgument kEntityTargetArgument;
inline const EntitySelectorArgument kEntitySelectorArgument;
// `/weather clear|rain [<duration>]`: the duration (seconds) is bounded by the
// same 0..1000000 1.16.1's WeatherCommand hands IntegerArgumentType; the
// handler converts it to ticks at 20 per second. A bound is required so a
// seconds value that would overflow when doubled is rejected at parse time.
inline const IntArgument kWeatherDurationArgument{0, 1'000'000};

} // namespace mc::gameplay::command

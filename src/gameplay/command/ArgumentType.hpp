#pragma once

#include "gameplay/CommandResult.hpp"
#include "gameplay/GameMode.hpp"
#include "gameplay/command/StringReader.hpp"

#include <any>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mc::gameplay::command {

// Defined in CommandSource.hpp; the context only holds a pointer to it, so the
// argument layer need not pull the player identity / feedback types in.
struct CommandSource;

// A transparent hasher for string-keyed maps. With `is_transparent` and
// std::equal_to<>, find() runs on a string_view without building a temporary
// std::string; std::hash<string_view> and std::hash<string> hash the same
// characters to the same value on the supported standard libraries.
struct LiteralHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view text) const {
        return std::hash<std::string_view>{}(text);
    }
    [[nodiscard]] std::size_t operator()(const std::string& text) const {
        return std::hash<std::string>{}(text);
    }
};

// A three-coordinate position, from a Vec3-style argument. An axis marked
// relative is resolved against the command source's position at execution time
// (the token carried a `~` prefix).
struct Position3 final {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    bool relativeX = false;
    bool relativeY = false;
    bool relativeZ = false;
};

// A yaw/pitch pair in vanilla semantics: yaw 0 faces +Z, positive pitch looks up.
struct Rotation2 final {
    double yaw = 0.0;
    double pitch = 0.0;
};

// Where in the input a parse failed — the CommandSyntaxException analogue. The
// UI can highlight `input` at `cursor` (both relative to the line that was
// parsed, i.e. without a leading '/') to show the player exactly what broke.
struct ParseError final {
    std::string message;
    std::size_t cursor = 0;
    std::string input;
};

// The result of parsing one argument: a typed value (std::any) on success, or a
// positioned error. parse() is a pure function of the reader — it neither needs
// the argument's declared name nor writes into a context — which is what lets a
// single ArgumentType instance be shared across commands and tested in
// isolation, the way 1.16.1's ArgumentType<S, T> returns T.
struct ArgumentParseResult final {
    std::any value;
    std::optional<ParseError> error;

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

// Mirrors 1.16.1's CommandContext: parsed arguments are stored by name and read
// out strongly typed at execution time. Values are type-erased (std::any) so a
// new argument type never touches this class — the closed variant was the
// extension bottleneck; `find<T>` fails softly on a missing or mistyped key.
class CommandContext final {
  public:
    void bind(std::string name, std::any value) {
        values_.emplace(std::move(name), std::move(value));
    }

    // The transparent hash lets lookups run on the caller's string_view without
    // building a temporary std::string — the hot path a handler pays per find.
    [[nodiscard]] bool has(std::string_view name) const {
        return values_.contains(name);
    }

    // Returns the value bound under `name` when it holds exactly type `T`.
    template <typename T>
    [[nodiscard]] std::optional<T> find(std::string_view name) const {
        const auto found = values_.find(name);
        if (found == values_.end()) {
            return std::nullopt;
        }
        if (const T* value = std::any_cast<T>(&found->second)) {
            return *value;
        }
        return std::nullopt;
    }

    // The source that ran this command — who, where, in which dimension, at what
    // op level (CommandSource.hpp). Set by the dispatcher just before the handler
    // runs; a handler resolves `~` coordinates and (future) `@s` against it. Null
    // when a command was executed without a source (the default execute() path a
    // headless test uses); handlers that touch the source only do so for `~`
    // forms, which the source-aware execute always supplies.
    void setSource(const CommandSource* source) { source_ = source; }
    [[nodiscard]] bool hasSource() const { return source_ != nullptr; }
    [[nodiscard]] const CommandSource& source() const { return *source_; }

  private:
    std::unordered_map<std::string, std::any, LiteralHash, std::equal_to<>> values_;
    const CommandSource* source_ = nullptr;  // not owned; outlives one execute() call
};

using CommandResult = mc::gameplay::CommandResult;

// One completion candidate: `text` replaces the typed token starting at `start`
// (an offset into the full input line, '/' included), and `hint` is the tooltip
// shown next to it.
struct Suggestion final {
    std::size_t start = 0;
    std::string text;
    std::string hint;
};

// Collects completion candidates for one cursor position. Filters by the token
// typed so far and dedupes, so ArgumentTypes only have to feed their full table
// and the sink decides what survives. Matching is case-sensitive, mirroring
// Brigadier; a namespaced candidate matches both by its full form and by its
// bare path, so `/give ac` finds rebedrock:acacia_planks.
class SuggestionSink final {
  public:
    SuggestionSink(std::vector<Suggestion>& output, std::size_t startOffset,
                   std::string_view partial)
        : output_(&output), startOffset_(startOffset), partial_(partial) {}

    // The token typed so far under the cursor, which suggest() filters against.
    // A multi-part argument (an entity selector) reads it to decide what part of
    // the token the cursor sits in and emit whole-token continuations for it.
    [[nodiscard]] std::string_view partial() const { return partial_; }

    void suggest(std::string_view text, std::string_view hint = {}) {
        const bool fullMatch = text.starts_with(partial_);
        bool pathMatch = false;
        if (!fullMatch) {
            if (const std::size_t colon = text.rfind(':');
                colon != std::string_view::npos) {
                pathMatch = text.substr(colon + 1U).starts_with(partial_);
            }
        }
        if (!fullMatch && !pathMatch) {
            return;
        }
        for (const Suggestion& existing : *output_) {
            if (existing.text == text) {
                return; // dedupe identical completions
            }
        }
        output_->push_back(Suggestion{startOffset_, std::string{text}, std::string{hint}});
    }

  private:
    std::vector<Suggestion>* output_;
    std::size_t startOffset_;
    std::string partial_;
};

// A typed argument parser, the 1.16.1 ArgumentType analogue. `parse` consumes
// one value from the reader (positioned after any whitespace), validates it and
// returns the typed value; `collectSuggestions` feeds this type's completion
// candidates to the sink. Both concerns living on one type is what lets a
// table-backed argument validate and complete from the same table.
class ArgumentType {
  public:
    virtual ~ArgumentType() = default;

    [[nodiscard]] virtual ArgumentParseResult parse(StringReader& reader) const = 0;

    // Feeds this type's completion candidates to the sink. `context` carries the
    // arguments already bound earlier on the same line, so a value can complete
    // relative to a prior one (a gamerule's value offers true/false once the rule
    // is known) — Brigadier passes the CommandContext to its SuggestionProvider
    // for the same reason. Most types ignore it.
    virtual void collectSuggestions(SuggestionSink& sink, const CommandContext& context) const = 0;

    // The token this argument shows in a generated usage string (CMD6's
    // smart-usage). Empty means "use the argument node's own name", so the
    // generator renders `<name>` from the tree. A type overrides this only when
    // one node name maps to several placeholders — a Vec3 renders `<x> <y> <z>`,
    // not `<pos>`. Returned verbatim (brackets included) when non-empty.
    [[nodiscard]] virtual std::string usageHint() const { return {}; }
};

// Builds a successful parse result carrying `value`.
[[nodiscard]] inline ArgumentParseResult parseOk(std::any value) {
    return {std::move(value), std::nullopt};
}

// Builds a failed parse result, tagging the reader's current cursor and input
// so the error can be highlighted later.
[[nodiscard]] inline ArgumentParseResult parseFail(std::string message,
                                                   const StringReader& reader) {
    return {std::any{},
            ParseError{std::move(message), reader.cursor(), std::string{reader.input()}}};
}

// Any single token, quoted or not — the free-form string argument.
class StringArgument final : public ArgumentType {
  public:
    ArgumentParseResult parse(StringReader& reader) const override {
        const auto token = reader.readString();
        if (!token.has_value()) {
            return parseFail("Expected a value", reader);
        }
        return parseOk(*token);
    }

    void collectSuggestions(SuggestionSink&, const CommandContext&) const override {}
};

// Reads to the end of the line — StringArgumentType's GreedyString. The value is
// the raw remainder (internal spaces included), so it must be a command's last
// argument (the `/say <message>` shape).
class GreedyStringArgument final : public ArgumentType {
  public:
    ArgumentParseResult parse(StringReader& reader) const override {
        const std::string rest = reader.readToEnd();
        if (rest.empty()) {
            return parseFail("Expected a value", reader);
        }
        return parseOk(rest);
    }

    void collectSuggestions(SuggestionSink&, const CommandContext&) const override {}
};

// A signed whole number, bound as int64, optionally range-checked (the
// IntegerArgumentType min/max floor and ceiling).
class IntArgument final : public ArgumentType {
  public:
    IntArgument() = default;
    IntArgument(std::int64_t minimum, std::int64_t maximum)
        : minimum_(minimum), maximum_(maximum) {}

    ArgumentParseResult parse(StringReader& reader) const override {
        const std::string token = reader.readUnquotedString();
        std::int64_t value = 0;
        const auto [end, error] =
            std::from_chars(token.data(), token.data() + token.size(), value);
        if (error != std::errc{} || end != token.data() + token.size()) {
            return parseFail("Expected a whole number, found \"" + token + "\"", reader);
        }
        if (minimum_.has_value() && value < *minimum_) {
            return parseFail("Expected a whole number of at least " + std::to_string(*minimum_),
                             reader);
        }
        if (maximum_.has_value() && value > *maximum_) {
            return parseFail("Expected a whole number of at most " + std::to_string(*maximum_),
                             reader);
        }
        return parseOk(value);
    }

    void collectSuggestions(SuggestionSink& sink, const CommandContext&) const override {
        if (minimum_.has_value() && maximum_.has_value()) {
            sink.suggest(std::to_string(*minimum_),
                         "[" + std::to_string(*minimum_) + ", " + std::to_string(*maximum_) + "]");
        } else {
            sink.suggest("1", "a whole number");
        }
    }

  private:
    std::optional<std::int64_t> minimum_;
    std::optional<std::int64_t> maximum_;
};

// Parses one coordinate token into a value and its `~`-relative flag: a plain
// number, `~` alone (a zero offset), or `~<number>`. Returns false when the
// token is not a coordinate.
[[nodiscard]] inline bool parseCoordinate(std::string_view token, double& value,
                                          bool& relative) {
    if (token.empty()) {
        return false;
    }
    relative = false;
    if (token.front() == '~') {
        relative = true;
        token.remove_prefix(1);
        if (token.empty()) {
            value = 0.0;
            return true;
        }
    }
    const auto [end, error] =
        std::from_chars(token.data(), token.data() + token.size(), value);
    return error == std::errc{} && end == token.data() + token.size();
}

// A yaw/pitch pair (both absolute), bound as a Rotation2.
class RotationArgument final : public ArgumentType {
  public:
    ArgumentParseResult parse(StringReader& reader) const override {
        const auto yawToken = reader.readString();
        reader.skipWhitespace();
        const auto pitchToken = reader.readString();
        if (!yawToken.has_value() || !pitchToken.has_value()) {
            return parseFail("Expected a yaw and pitch", reader);
        }
        Rotation2 rotation;
        bool yawRelative = false;
        if (!parseCoordinate(*yawToken, rotation.yaw, yawRelative) || yawRelative) {
            return parseFail("Invalid yaw: " + *yawToken, reader);
        }
        bool pitchRelative = false;
        if (!parseCoordinate(*pitchToken, rotation.pitch, pitchRelative) || pitchRelative) {
            return parseFail("Invalid pitch: " + *pitchToken, reader);
        }
        return parseOk(rotation);
    }

    void collectSuggestions(SuggestionSink&, const CommandContext&) const override {}
};

// Shared, stateless instances of the generic argument types. The pure-function
// parse makes one instance safe to reuse across every command, the way 1.16.1
// registers a single ArgumentType and shares it between commands.
inline const StringArgument kStringArgument;
inline const GreedyStringArgument kGreedyStringArgument;
inline const IntArgument kIntArgument;
inline const RotationArgument kRotationArgument;

} // namespace mc::gameplay::command

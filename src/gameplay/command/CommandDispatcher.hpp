#pragma once

#include "gameplay/CommandResult.hpp"
#include "gameplay/command/ArgumentType.hpp"
#include "gameplay/command/StringReader.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mc::gameplay::command {

using CommandResult = mc::gameplay::CommandResult;

class CommandBuilder; // literal() returns it; see the fluent DSL below

// The result of walking the tree over one input line — the single parse path
// both execution and completion share (1.16.1's ParseResults analogue). The
// fields describe where the walk stopped, so execute() and suggestions() never
// reimplement the token-walk themselves.
struct ParseResults final {
    std::size_t node = 0;          // node reached by the last complete token
    CommandContext context;        // argument values bound along the way
    std::size_t cursor = 0;        // where the walk stopped (line coordinates)
    std::size_t partialStart = 0;  // start of the partial token, else cursor
    bool hasPartial = false;       // whether the cursor sits in a partial token
    bool error = false;
    std::string errorMessage;
};

// The command tree, mirroring 1.16.1's Brigadier CommandDispatcher shape: a
// root node, literal nodes (exact tokens) and argument nodes (parsed values),
// dispatched one token at a time through per-node hash maps, so each step is an
// O(1) lookup instead of a linear scan over every command. Every command the
// game exposes is registered here; execution, validation and completion all
// walk the same tree. Literal matching is case-sensitive, exactly like
// Brigadier's HashMap of child nodes.
//
// Registration is idempotent: reusing a literal path reuses its nodes and
// overwrites the handler, so a command can be safely re-registered (e.g. on a
// world reload) without duplicating the tree.
class CommandDispatcher final {
  public:
    // An executable handler receives the parsed context; it is attached to the
    // leaf node whose command it implements.
    using Handler = std::function<CommandResult(const CommandContext&)>;

    // Extra completion candidates for one node, independent of its argument
    // type — Brigadier's node-level customSuggestions. Called with the same
    // sink that collects literal and argument suggestions.
    using SuggestionsProvider = std::function<void(SuggestionSink&)>;

    CommandDispatcher() { nodes_.emplace_back(); } // subscript 0 is always the root

    // Begins registering a root-level literal command (e.g. "gamemode").
    CommandBuilder literal(std::string_view name);

    // Parses and executes one full input line ("/gamemode survival").
    [[nodiscard]] CommandResult execute(std::string_view input) const;

    // Whether a root command with this exact name is registered.
    [[nodiscard]] bool contains(std::string_view name) const;

    // The number of root commands.
    [[nodiscard]] std::size_t commandCount() const {
        return nodes_[root_].literalChildren.size();
    }

    // Walks `line` token by token from the root. With `stopCursor`, stops at the
    // token under the cursor (marking it as partial) so completion can suggest
    // there; without it, consumes the whole line and reports the first error
    // (with its input position) in ParseResults.
    [[nodiscard]] ParseResults parse(std::string_view line,
                                     std::optional<std::size_t> stopCursor) const;

    // Completion for the input line at `cursor`. Returns every candidate that
    // could fill the token under the cursor, each carrying the offset it
    // replaces. The '/' prefix is optional; offsets are relative to `input` as
    // passed in. Exact matches of the typed token sort before prefix matches.
    [[nodiscard]] std::vector<Suggestion> suggestions(std::string_view input,
                                                      std::size_t cursor) const;

  private:
    friend class CommandBuilder;

    struct Node {
        bool argument = false; // true: this node parses a value under `name`
        std::string name;      // literal token (exact) or argument key
        // Transparent (heterogeneous) lookup lets `find(token)` run on a
        // string_view without building a std::string — the hot path allocates
        // nothing per token.
        std::unordered_map<std::string, std::size_t, LiteralHash, std::equal_to<>>
            literalChildren; // exact token → node
        std::optional<std::size_t> argumentChild;  // the value child
        const ArgumentType* argumentType = nullptr; // set when argument; not owned
        Handler handler;                            // executable leaf handler
        SuggestionsProvider customSuggestions;      // extra node-level completions
    };

    [[nodiscard]] std::size_t addLiteralChild(std::size_t parent, std::string_view token);
    [[nodiscard]] std::size_t addArgumentChild(std::size_t parent, std::string_view key,
                                                const ArgumentType& type);

    std::vector<Node> nodes_;
    std::size_t root_ = 0;
};

// Fluent registration: literal("gamemode").argument("mode", ...).executes(handler).
class CommandBuilder final {
  public:
    // Adds a literal child and moves the builder onto it.
    CommandBuilder& then(std::string_view literal);

    // Adds a typed value argument and moves the builder onto it. The argument
    // node may carry both an executes() handler and further arguments, which is
    // how an optional trailing value (`/gamerule <rule> [<value>]`) is built.
    // `type` is not owned and must outlive the dispatcher — pass a shared
    // stateless instance (`command::kGameModeArgument`).
    CommandBuilder& argument(std::string_view name, const ArgumentType& type);

    // Attaches the executable handler to the current node.
    CommandBuilder& executes(CommandDispatcher::Handler handler);

    // Attaches node-level completion candidates to the current node, in
    // addition to its argument type's own suggestions (Brigadier's
    // customSuggestions).
    CommandBuilder& suggests(CommandDispatcher::SuggestionsProvider provider);

  private:
    friend class CommandDispatcher;
    CommandBuilder(CommandDispatcher& dispatcher, std::size_t node)
        : dispatcher_(&dispatcher), node_(node) {}

    CommandDispatcher* dispatcher_;
    std::size_t node_;
};

inline CommandBuilder CommandDispatcher::literal(std::string_view name) {
    const std::size_t node = addLiteralChild(root_, name);
    return CommandBuilder{*this, node};
}

inline CommandBuilder& CommandBuilder::then(std::string_view literal) {
    node_ = dispatcher_->addLiteralChild(node_, literal);
    return *this;
}

inline CommandBuilder& CommandBuilder::argument(std::string_view name,
                                                const ArgumentType& type) {
    node_ = dispatcher_->addArgumentChild(node_, name, type);
    return *this;
}

inline CommandBuilder& CommandBuilder::executes(CommandDispatcher::Handler handler) {
    dispatcher_->nodes_[node_].handler = std::move(handler);
    return *this;
}

inline CommandBuilder& CommandBuilder::suggests(CommandDispatcher::SuggestionsProvider provider) {
    dispatcher_->nodes_[node_].customSuggestions = std::move(provider);
    return *this;
}

inline std::size_t CommandDispatcher::addLiteralChild(std::size_t parent, std::string_view token) {
    const std::string key{token};
    if (const auto found = nodes_[parent].literalChildren.find(key);
        found != nodes_[parent].literalChildren.end()) {
        return found->second;
    }
    const std::size_t id = nodes_.size();
    nodes_.emplace_back(Node{});
    nodes_[id].name = key;
    // emplace_back may reallocate the node vector, invalidating any previously
    // held reference — re-fetch the parent by its (stable) subscript instead.
    nodes_[parent].literalChildren.emplace(key, id);
    return id;
}

inline std::size_t CommandDispatcher::addArgumentChild(std::size_t parent, std::string_view key,
                                                       const ArgumentType& type) {
    if (nodes_[parent].argumentChild.has_value()) {
        const std::size_t existing = *nodes_[parent].argumentChild;
        nodes_[existing].argumentType = &type; // idempotent: reuse the node, refresh the type
        return existing;
    }
    const std::size_t id = nodes_.size();
    nodes_.emplace_back(Node{});
    nodes_[id].argument = true;
    nodes_[id].name = std::string{key};
    nodes_[id].argumentType = &type;
    nodes_[parent].argumentChild = id;
    return id;
}

inline bool CommandDispatcher::contains(std::string_view name) const {
    return nodes_[root_].literalChildren.contains(name);
}

inline CommandResult CommandDispatcher::execute(std::string_view input) const {
    if (input.empty() || input.front() != '/') {
        return {false, "Commands must start with /"};
    }
    const auto parsed = parse(input.substr(1), std::nullopt);
    if (parsed.error) {
        return {false, parsed.errorMessage};
    }
    const Node& leaf = nodes_[parsed.node];
    if (leaf.handler) {
        return leaf.handler(parsed.context);
    }
    return {false, "Incomplete command"};
}

inline std::vector<Suggestion> CommandDispatcher::suggestions(std::string_view input,
                                                              std::size_t cursor) const {
    std::vector<Suggestion> result;
    if (input.empty() || cursor == 0U) {
        return result;
    }
    const std::size_t leadingSlash = input.front() == '/' ? 1U : 0U;
    if (cursor < leadingSlash) {
        return result;
    }
    const std::string_view line = input.substr(leadingSlash);
    const std::size_t pos = std::min(cursor - leadingSlash, line.size());
    const auto parsed = parse(line, pos);
    if (parsed.error) {
        return result; // the line is already invalid past this point
    }

    std::string_view partial = line.substr(parsed.partialStart, pos - parsed.partialStart);
    if (!partial.empty() && StringReader::isQuotedStringStart(partial.front())) {
        partial.remove_prefix(1); // complete inside an open quote
    }
    const std::size_t startOffset = parsed.partialStart + leadingSlash;
    SuggestionSink sink{result, startOffset, partial};

    const Node& current = nodes_[parsed.node];
    for (const auto& [name, child] : current.literalChildren) {
        sink.suggest(name);
    }
    if (current.argumentChild.has_value()) {
        nodes_[*current.argumentChild].argumentType->collectSuggestions(sink);
    }
    if (current.customSuggestions) {
        current.customSuggestions(sink);
    }
    // Exact matches of the typed token lead, then lexicographic order, so a
    // completed command stays put while Tab cycles the remaining candidates.
    std::stable_sort(result.begin(), result.end(),
                     [partial](const Suggestion& left, const Suggestion& right) {
                         const bool leftExact = left.text == partial;
                         const bool rightExact = right.text == partial;
                         if (leftExact != rightExact) {
                             return leftExact;
                         }
                         return left.text < right.text;
                     });
    return result;
}

inline ParseResults CommandDispatcher::parse(std::string_view line,
                                             std::optional<std::size_t> stopCursor) const {
    ParseResults result;
    StringReader reader{line};
    std::size_t node = root_;
    while (true) {
        reader.skipWhitespace();
        if (stopCursor.has_value() && reader.cursor() >= *stopCursor) {
            // Clean boundary: the cursor sits in whitespace or at end-of-input.
            result.partialStart = std::min(reader.cursor(), *stopCursor);
            break;
        }
        if (!reader.canRead()) {
            break;
        }
        const std::size_t tokenStart = reader.cursor();
        const auto token = reader.readString();
        const std::size_t tokenEnd = reader.cursor();
        if (stopCursor.has_value() && tokenEnd >= *stopCursor) {
            // The token under the cursor is the partial one being typed.
            result.partialStart = tokenStart;
            result.hasPartial = true;
            break;
        }
        const Node& current = nodes_[node];
        if (token.has_value()) {
            // Case-sensitive literal match; the transparent hash map finds the
            // token without allocating a temporary string.
            if (const auto found = current.literalChildren.find(*token);
                found != current.literalChildren.end()) {
                node = found->second;
                result.node = node;
                continue;
            }
        }
        // Not a literal child: rewind so the argument parser reads the token
        // itself (it may want a quoted string or its own error message).
        reader.setCursor(tokenStart);
        if (current.argumentChild.has_value()) {
            const std::size_t argumentNode = *current.argumentChild;
            const auto parsed = nodes_[argumentNode].argumentType->parse(reader);
            if (parsed.error.has_value()) {
                if (stopCursor.has_value()) {
                    // Completion mode: an unparseable token yields no useful
                    // suggestions — treat it as the partial and let the sink
                    // filter (an invalid token usually matches nothing).
                    result.partialStart = tokenStart;
                    result.hasPartial = true;
                    break;
                }
                result.error = true;
                result.errorMessage = parsed.error->message;
                break;
            }
            result.context.bind(nodes_[argumentNode].name, parsed.value);
            node = argumentNode;
            result.node = node;
            continue;
        }
        result.error = true;
        result.errorMessage = token.has_value()
            ? "Unknown command or argument: " + *token
            : "Invalid argument near \"" + std::string{reader.remaining()} + "\"";
        break;
    }
    result.node = node;
    result.cursor = reader.cursor();
    return result;
}

} // namespace mc::gameplay::command

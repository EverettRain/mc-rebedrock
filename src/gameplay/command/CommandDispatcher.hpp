#pragma once

#include "gameplay/CommandResult.hpp"
#include "gameplay/command/ArgumentType.hpp"
#include "gameplay/command/CommandSource.hpp"
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

// Transforms the command source(s) an `execute` clause applies to — the DOD
// stand-in for Brigadier's RedirectModifier/fork. Given the arguments bound in
// this segment and one incoming source, it appends the resulting sources to
// `out`: many (`as`/`at` over a multi-entity selector = fork), one (`positioned`
// = transform), or zero (`if`/`unless` that fails = gate). Returns an empty
// string on success or an error message. The dispatcher owns the redirect walk
// and the forking loop; the modifier (a closure over the runtime) owns the
// per-clause semantics, so the tree never grows a clause-specific branch.
using SourceModifier = std::function<std::string(
    const CommandContext& args, const CommandSource& incoming,
    std::vector<CommandSource>& out)>;

// One link of a redirected command (an `execute` clause chain): the arguments
// bound while walking it and the node whose SourceModifier fires after it. The
// terminal command (what `run` points at) is not a segment — it is the leaf the
// dispatcher runs once per surviving source.
struct ParseSegment final {
    std::size_t node = 0;    // the redirect node; its sourceModifier transforms the sources
    CommandContext context;  // arguments bound within this segment
};

// The result of walking the tree over one input line — the single parse path
// both execution and completion share (vanilla's ParseResults analogue). The
// fields describe where the walk stopped, so execute() and suggestions() never
// reimplement the token-walk themselves.
struct ParseResults final {
    std::size_t node = 0;          // node reached by the last complete token
    CommandContext context;        // argument values bound in the terminal segment
    // Redirect links crossed before the terminal node (empty for a plain
    // command). Each carries its own bound arguments and the modifier that forks
    // the source set; execute() replays them in order, then runs the terminal
    // once per surviving source. `run` contributes a modifier-less link.
    std::vector<ParseSegment> priorSegments;
    std::size_t cursor = 0;        // where the walk stopped (line coordinates)
    std::size_t partialStart = 0;  // start of the partial token, else cursor
    bool hasPartial = false;       // whether the cursor sits in a partial token
    bool error = false;
    std::string errorMessage;
    // The root command literal this line began with (the first token matched off
    // the root), so an incomplete command can report that command's usage.
    std::string commandName;
    // The highest op level any node on the walked path declared — a child
    // inherits its ancestors' requirement, so this is folded to the max as the
    // walk descends. execute() refuses a source below it (Brigadier's per-node
    // `requires`, collapsed to one integer compared once at the leaf).
    PermissionLevel requiredLevel = PermissionLevel::All;
};

// The command tree, mirroring vanilla's Brigadier CommandDispatcher shape: a
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
    // type — Brigadier's node-level customSuggestions. Called with the same sink
    // that collects literal and argument suggestions, plus the arguments already
    // bound on the line (so a node can complete relative to an earlier value).
    using SuggestionsProvider = std::function<void(SuggestionSink&, const CommandContext&)>;

    CommandDispatcher() { nodes_.emplace_back(); } // subscript 0 is always the root

    // Begins registering a root-level literal command (e.g. "gamemode").
    CommandBuilder literal(std::string_view name);

    // Parses and executes one full input line ("/gamemode survival") on behalf of
    // `source`: the walk resolves `~` coordinates against it, the command is
    // refused when the source's op level is below what the command declared, and
    // the result is routed to the source's feedback sink (when it has one) before
    // being returned.
    [[nodiscard]] CommandResult execute(std::string_view input, const CommandSource& source) const;
    // Convenience for callers with no source of their own (a headless test, an
    // internal script): runs as the single-player owner (op4, at the origin, no
    // feedback sink), so every command is permitted and the result comes back by
    // return value only.
    [[nodiscard]] CommandResult execute(std::string_view input) const;

    // `/gamerule max_command_forks`: the ceiling on the source set an `execute`
    // chain may fork into. The runtime pushes the world's rule value here; the
    // default is vanilla's, so a dispatcher nobody configures (every headless
    // test) behaves exactly as it did before the rule existed.
    void setMaximumForkedSources(std::size_t maximum) { maxForkedSources_ = maximum; }
    [[nodiscard]] std::size_t maximumForkedSources() const { return maxForkedSources_; }

    // PACK-2: runs an already-parsed line against `source`, with zero re-parsing.
    // This is what makes a compiled `.mcfunction` line cheap to replay every time
    // `/function` (or a `#tick` member) fires: the loader calls parse() once per
    // line and keeps the ParseResults (FunctionManager's compiled command), and
    // every later run calls this instead of execute(input, source) — the token
    // walk, literal/argument matching and permission-level fold from parse() are
    // not repeated. Semantics mirror execute()'s post-parse half exactly (same
    // permission gate, same redirect replay for a parsed `execute … run …` line,
    // same feedback routing); only the "turn text into a walked tree path" step
    // is skipped because `parsed` already did it. Takes `parsed` by non-const
    // reference because running it rebinds its context's source pointer, exactly
    // like execute() does to its own local copy.
    [[nodiscard]] CommandResult executeParsed(ParseResults& parsed, const CommandSource& source) const;

    // The root node id and a builder positioned at an existing node — used to
    // wire `execute`'s redirect chain, where several clauses branch off the same
    // node and `run` redirects back to the root.
    [[nodiscard]] std::size_t rootId() const { return root_; }
    [[nodiscard]] CommandBuilder builderAt(std::size_t node);

    // Whether a root command with this exact name is registered.
    [[nodiscard]] bool contains(std::string_view name) const;

    // The number of root commands.
    [[nodiscard]] std::size_t commandCount() const {
        return nodes_[root_].literalChildren.size();
    }

    // Read-only introspection (CMD6), mirroring Brigadier's getAllUsage /
    // getSmartUsage — it walks the same node tree execution does, never a second
    // structure. forEachRootCommand visits every root command name in sorted
    // order (the children map is a hash map, so sorting is what makes /help
    // output deterministic). The visitor takes a std::string_view name.
    template <typename Visitor>
    void forEachRootCommand(Visitor&& visitor) const {
        std::vector<std::string_view> names;
        names.reserve(nodes_[root_].literalChildren.size());
        for (const auto& [name, child] : nodes_[root_].literalChildren) {
            static_cast<void>(child);
            names.emplace_back(name);
        }
        std::sort(names.begin(), names.end());
        for (const std::string_view name : names) {
            visitor(name);
        }
    }

    // The smart-usage string for `command` (without the leading '/'), generated
    // from its subtree and filtered by the source's op level: a literal group is
    // `(a|b|c)`, an argument is `<name>` (or the type's usageHint), and an
    // optional tail is `[...]`. Empty when the command does not exist or the
    // source may not use it (so /help hides it). Example:
    // `weather (clear|rain|thunder) [<duration>]`.
    [[nodiscard]] std::string usage(std::string_view command, const CommandSource& source) const;

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

    // The ceiling on the forked source set an `execute` chain may build, so a
    // fork bomb fails cleanly instead of exhausting memory. This is vanilla's
    // `max_command_forks` game rule; the constant is its default, which the
    // runtime overwrites with the world's rule value before it dispatches.
    static constexpr std::size_t kDefaultMaxForkedSources = std::size_t{1} << 16U;
    std::size_t maxForkedSources_ = kDefaultMaxForkedSources;

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
        // The op level this node (and, by inheritance, its subtree) requires;
        // All means unrestricted. Set through CommandBuilder::requires.
        PermissionLevel requiredLevel = PermissionLevel::All;
        // After this node, parsing continues from `redirect` instead of this
        // node's own children — Brigadier's node redirect. `execute`'s clauses
        // redirect back to the execute node (another clause follows); `run`
        // redirects to the root (a fresh command follows). `sourceModifier`, when
        // set, forks/gates the source set as the redirect is crossed.
        std::optional<std::size_t> redirect;
        SourceModifier sourceModifier;
    };

    [[nodiscard]] std::size_t addLiteralChild(std::size_t parent, std::string_view token);
    [[nodiscard]] std::size_t addArgumentChild(std::size_t parent, std::string_view key,
                                                const ArgumentType& type);
    // Generates the usage of everything under `node` (its children onward),
    // already bracketed/grouped, op-filtered by `source`. Empty for a leaf.
    [[nodiscard]] std::string usageChildren(std::size_t node, const CommandSource& source) const;

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

    // Declares the op level this command needs (Brigadier's `requires`, narrowed
    // to the op-level predicate; `requires` itself is a C++20 keyword). Set on a
    // literal, it gates the whole subtree rooted there —
    // `literal("gamemode").requiresLevel(GameMasters)` gates every `/gamemode …`
    // form — because execute() folds the path's levels to their max. A source
    // below the level is refused before the handler runs.
    CommandBuilder& requiresLevel(PermissionLevel level);

    // Attaches node-level completion candidates to the current node, in
    // addition to its argument type's own suggestions (Brigadier's
    // customSuggestions).
    CommandBuilder& suggests(CommandDispatcher::SuggestionsProvider provider);

    // Redirects parsing from the current node to `target` (Brigadier's redirect):
    // the next token is matched against `target`'s children. `execute`'s clauses
    // redirect back to the execute node; `run` redirects to rootId().
    CommandBuilder& redirectTo(std::size_t target);

    // Attaches the SourceModifier that forks/gates the source set as this node's
    // redirect is crossed (an `execute` clause's semantics).
    CommandBuilder& modifiesSource(SourceModifier modifier);

    // The id of the node the builder currently sits on — captured so sibling
    // clauses can be branched off it and `run` can redirect to it.
    [[nodiscard]] std::size_t nodeId() const { return node_; }

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

inline CommandBuilder& CommandBuilder::requiresLevel(PermissionLevel level) {
    dispatcher_->nodes_[node_].requiredLevel = level;
    return *this;
}

inline CommandBuilder& CommandBuilder::redirectTo(std::size_t target) {
    dispatcher_->nodes_[node_].redirect = target;
    return *this;
}

inline CommandBuilder& CommandBuilder::modifiesSource(SourceModifier modifier) {
    dispatcher_->nodes_[node_].sourceModifier = std::move(modifier);
    return *this;
}

inline CommandBuilder CommandDispatcher::builderAt(std::size_t node) {
    return CommandBuilder{*this, node};
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

inline CommandResult CommandDispatcher::execute(std::string_view input,
                                                const CommandSource& source) const {
    if (input.empty() || input.front() != '/') {
        CommandResult result{false, "Commands must start with /"};
        if (source.feedback) {
            source.feedback(result);
        }
        return result;
    }
    auto parsed = parse(input.substr(1), std::nullopt);
    return executeParsed(parsed, source);
}

inline CommandResult CommandDispatcher::executeParsed(ParseResults& parsed,
                                                       const CommandSource& source) const {
    // A single place decides the outcome, then routes it to the source's feedback
    // sink before returning — successes and failures alike (the sink applies the
    // sendCommandFeedback gamerule, so the gate is not duplicated here). Shared by
    // execute() (parses then falls straight through) and a compiled `.mcfunction`
    // line replay (PACK-2: parse() ran once at load time, this runs the cached
    // result every time the function fires — no re-parsing on the hot path).
    const auto route = [&source](CommandResult result) {
        if (source.feedback) {
            source.feedback(result);
        }
        return result;
    };
    if (parsed.error) {
        return route({false, parsed.errorMessage});
    }
    if (!hasPermission(source.permissionLevel, parsed.requiredLevel)) {
        return route({false, "You do not have permission to use this command"});
    }
    const Node& leaf = nodes_[parsed.node];
    if (!leaf.handler) {
        // Incomplete: point the player at what the command wants (R2) instead of
        // a bare "Incomplete command".
        if (const std::string smart = usage(parsed.commandName, source); !smart.empty()) {
            return route({false, "Usage: /" + smart});
        }
        return route({false, "Incomplete command"});
    }
    // Plain command (no redirect crossed): run once as the source. The handler
    // reads the source (relative coordinates, op level) through the context.
    if (parsed.priorSegments.empty()) {
        parsed.context.setSource(&source);
        return route(leaf.handler(parsed.context));
    }
    // Redirected command (`execute … run <command>`): replay the clause chain to
    // build the source set, then run the terminal command once per surviving
    // source. Inner results are aggregated (a forked run does not broadcast each
    // sub-result), so the working sources carry no feedback sink — only this one
    // aggregate is routed to the original source's sink.
    std::vector<CommandSource> sources;
    {
        CommandSource base = source;
        base.feedback = nullptr;
        sources.push_back(std::move(base));
    }
    for (const ParseSegment& segment : parsed.priorSegments) {
        const Node& redirectNode = nodes_[segment.node];
        if (!redirectNode.sourceModifier) {
            continue; // a modifier-less link (`run`) leaves the source set intact
        }
        std::vector<CommandSource> next;
        for (const CommandSource& incoming : sources) {
            if (std::string error = redirectNode.sourceModifier(segment.context, incoming, next);
                !error.empty()) {
                return route({false, std::move(error)});
            }
        }
        // Guard against a fork bomb (`execute as @e run execute as @e …`): the
        // source set can multiply per clause, so cap it the way vanilla bounds the
        // command chain rather than letting it grow without limit.
        if (next.size() > maxForkedSources_) {
            return route({false, "Too many entities selected by execute"});
        }
        sources = std::move(next);
    }
    std::size_t ran = 0;
    for (const CommandSource& forked : sources) {
        parsed.context.setSource(&forked);
        if (leaf.handler(parsed.context).success) {
            ++ran;
        }
    }
    if (ran == 0) {
        return route({false, "Executed no commands"});
    }
    return route({true, "Executed " + std::to_string(ran) + (ran == 1U ? " command" : " commands")});
}

inline CommandResult CommandDispatcher::execute(std::string_view input) const {
    // The single-player owner: op4 at the origin, no feedback sink. Static so its
    // address is stable for the context's source pointer across the call.
    static const CommandSource kOwnerSource{};
    return execute(input, kOwnerSource);
}

inline std::string CommandDispatcher::usageChildren(std::size_t node,
                                                    const CommandSource& source) const {
    // Collect the children the source may use: literals (sorted for determinism)
    // then the single argument child.
    const Node& parent = nodes_[node];
    std::vector<std::size_t> children;
    {
        std::vector<std::string_view> literalNames;
        literalNames.reserve(parent.literalChildren.size());
        for (const auto& [name, child] : parent.literalChildren) {
            static_cast<void>(child);
            literalNames.emplace_back(name);
        }
        std::sort(literalNames.begin(), literalNames.end());
        for (const std::string_view name : literalNames) {
            const std::size_t child = parent.literalChildren.find(name)->second;
            if (hasPermission(source.permissionLevel, nodes_[child].requiredLevel)) {
                children.push_back(child);
            }
        }
    }
    if (parent.argumentChild.has_value() &&
        hasPermission(source.permissionLevel, nodes_[*parent.argumentChild].requiredLevel)) {
        children.push_back(*parent.argumentChild);
    }
    if (children.empty()) {
        return {};
    }

    // The token and tail of each child.
    struct Part final {
        bool literal = false;
        std::string token;
        std::string tail;
    };
    std::vector<Part> parts;
    parts.reserve(children.size());
    for (const std::size_t child : children) {
        Part part;
        part.literal = !nodes_[child].argument;
        if (part.literal) {
            part.token = nodes_[child].name;
        } else if (std::string hint = nodes_[child].argumentType->usageHint(); !hint.empty()) {
            part.token = std::move(hint);
        } else {
            part.token = "<" + nodes_[child].name + ">";
        }
        part.tail = usageChildren(child, source);
        parts.push_back(std::move(part));
    }

    // A node with a handler makes its children optional (`[...]`), the way
    // `/gamerule <rule> [<value>]` works.
    const bool optional = static_cast<bool>(parent.handler);
    const auto joinTokens = [&parts](char separator) {
        std::string joined;
        for (std::size_t index = 0; index < parts.size(); ++index) {
            if (index != 0) {
                joined.push_back(separator);
            }
            joined += parts[index].token;
        }
        return joined;
    };

    // All-literal siblings that share the same tail factor into one group with
    // the tail pulled out once — `(clear|rain|thunder) [<duration>]`.
    const bool allLiteral =
        std::all_of(parts.begin(), parts.end(), [](const Part& part) { return part.literal; });
    const bool sameTail = std::all_of(parts.begin(), parts.end(), [&parts](const Part& part) {
        return part.tail == parts.front().tail;
    });
    if (parts.size() > 1 && allLiteral && sameTail) {
        const std::string group = joinTokens('|');
        std::string body = optional ? "[" + group + "]" : "(" + group + ")";
        if (!parts.front().tail.empty()) {
            body += " " + parts.front().tail;
        }
        return body;
    }
    if (parts.size() == 1) {
        std::string body = parts.front().token;
        if (!parts.front().tail.empty()) {
            body += " " + parts.front().tail;
        }
        return optional ? "[" + body + "]" : body;
    }
    // Mixed or differing tails: each alternative carries its own tail.
    std::string group;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index != 0) {
            group.push_back('|');
        }
        group += parts[index].token;
        if (!parts[index].tail.empty()) {
            group += " " + parts[index].tail;
        }
    }
    return optional ? "[" + group + "]" : "(" + group + ")";
}

inline std::string CommandDispatcher::usage(std::string_view command,
                                            const CommandSource& source) const {
    const auto found = nodes_[root_].literalChildren.find(command);
    if (found == nodes_[root_].literalChildren.end()) {
        return {};
    }
    const std::size_t node = found->second;
    if (!hasPermission(source.permissionLevel, nodes_[node].requiredLevel)) {
        return {}; // hidden from a source that may not use it
    }
    const std::string tail = usageChildren(node, source);
    return tail.empty() ? std::string{command} : std::string{command} + " " + tail;
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
        // The value type completes against the arguments bound so far (parsed.context),
        // so e.g. a gamerule value offers true/false once its rule is known.
        nodes_[*current.argumentChild].argumentType->collectSuggestions(sink, parsed.context);
    }
    if (current.customSuggestions) {
        current.customSuggestions(sink, parsed.context);
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
    CommandContext currentContext; // arguments of the segment being walked now
    // Crosses every redirect the current node declares, recording each as a
    // prior segment (its bound arguments + the modifier that forks the source
    // set) and moving to the redirect target. Called only when another token
    // slot follows, so an incomplete `execute as @e` (no `run`) stops on the
    // redirect node itself rather than chasing the link into nothing.
    const auto followRedirects = [&] {
        while (nodes_[node].redirect.has_value()) {
            result.priorSegments.push_back(ParseSegment{node, std::move(currentContext)});
            currentContext = CommandContext{};
            node = *nodes_[node].redirect;
            result.requiredLevel = maxLevel(result.requiredLevel, nodes_[node].requiredLevel);
        }
    };
    while (true) {
        reader.skipWhitespace();
        if (stopCursor.has_value() && reader.cursor() >= *stopCursor) {
            // Clean boundary: the cursor sits in whitespace or at end-of-input,
            // which is a fresh token slot — cross any redirect first so
            // completion offers the redirect target's children (execute's
            // clauses, or every command after `run`).
            followRedirects();
            result.partialStart = std::min(reader.cursor(), *stopCursor);
            break;
        }
        if (!reader.canRead()) {
            break; // end of input with no further token: do not cross a redirect
        }
        // Another token follows: its continuation lives under the redirect
        // target when the current node redirects, so cross the link now.
        followRedirects();
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
                if (node == root_) {
                    result.commandName = *token; // the command this line invokes
                }
                node = found->second;
                result.node = node;
                result.requiredLevel = maxLevel(result.requiredLevel, nodes_[node].requiredLevel);
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
            currentContext.bind(nodes_[argumentNode].name, parsed.value);
            node = argumentNode;
            result.node = node;
            result.requiredLevel = maxLevel(result.requiredLevel, nodes_[node].requiredLevel);
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
    result.context = std::move(currentContext);
    return result;
}

} // namespace mc::gameplay::command

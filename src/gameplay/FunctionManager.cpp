#include "gameplay/FunctionManager.hpp"

#include "assets/ResourceProvider.hpp"
#include "core/Json.hpp"
#include "data/TagFile.hpp"

#include <algorithm>
#include <unordered_set>

namespace mc::gameplay {
namespace {

// `data/minecraft/functions/**/*.mcfunction` -> "minecraft:foo/bar" (the id a
// `/function` argument and a tag's `values` entries both name). Mirrors
// RecipeTable's keyFor / LootTable's blockNameFromPath: strip the category
// prefix, then the file extension.
[[nodiscard]] std::string functionIdFromLocation(const assets::ResourceLocation& location) {
    std::string_view path = location.path;
    constexpr std::string_view kPrefix = "functions/";
    if (path.size() >= kPrefix.size() && path.substr(0, kPrefix.size()) == kPrefix) {
        path.remove_prefix(kPrefix.size());
    }
    constexpr std::string_view kSuffix = ".mcfunction";
    if (path.size() >= kSuffix.size() && path.substr(path.size() - kSuffix.size()) == kSuffix) {
        path.remove_suffix(kSuffix.size());
    }
    return location.space + ":" + std::string{path};
}

// One `.mcfunction` line, comment/blank-stripped and leading/trailing
// whitespace trimmed. `#`-prefixed lines are comments (JE's mcfunction
// format), not to be confused with a tag reference's `#` prefix inside a JSON
// values array — two different contexts, both vanilla's own convention.
[[nodiscard]] std::string_view trimLine(std::string_view line) {
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = line.find_last_not_of(" \t\r");
    return line.substr(first, last - first + 1U);
}

// Splits `text` on '\n', handing each raw line to `visit`. Kept allocation-free
// (string_view slices of the already-read file) since a function's whole body
// is walked exactly once, at load time.
template <typename Visit>
void forEachLine(std::string_view text, Visit&& visit) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto newline = text.find('\n', start);
        const std::string_view line =
            newline == std::string_view::npos ? text.substr(start) : text.substr(start, newline - start);
        visit(line);
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1U;
    }
}

// Turns a tag reference (`#minecraft:tick`, or a bare `tick` defaulting to
// `minecraft`) into the tag file's content path — functions/ tags live under
// `tags/functions/`, block tags under `tags/block/`, the only difference from
// BlockTags.cpp's tagLocation.
[[nodiscard]] assets::ResourceLocation functionTagLocation(std::string_view reference) {
    const auto separator = reference.find(':');
    const std::string_view space =
        separator == std::string_view::npos ? std::string_view{"minecraft"} : reference.substr(0, separator);
    const std::string_view name =
        separator == std::string_view::npos ? reference : reference.substr(separator + 1U);
    return assets::data("tags/functions/" + std::string{name} + ".json", space);
}

// How deep a chain of `#tag` references inside a function tag may go —
// BlockTags.cpp's kMaximumTagDepth, restated here because function tags are
// their own small file set with their own (generous, cycle-proof) ceiling.
constexpr int kMaximumFunctionTagDepth = 16;

// Accumulates the function ids a tag names, following `#tag` references
// exactly like BlockTags::collectTag (low-to-high pack merge, `replace`
// truncates, a self-reference terminates via `visited` rather than hanging).
// `known` is the set of ids load() already compiled — an entry naming a
// function this stack does not have is skipped (vanilla tolerates a tag
// listing an absent function unless the entry is marked required, and this
// project treats every entry as optional per TagEntry's existing convention).
void collectFunctionTag(const assets::ResourceProvider& resources, const assets::ResourceLocation& location,
                        int depth, std::unordered_set<std::string>& visited,
                        const std::unordered_map<std::string, CompiledFunction>& known,
                        std::vector<std::string>& out) {
    if (depth > kMaximumFunctionTagDepth || !visited.insert(location.toString()).second) {
        return;
    }
    for (const auto& bytes : resources.readAllBytes(location)) {
        core::Json root;
        try {
            root = core::Json::parse(
                std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        } catch (const std::exception&) {
            continue; // a malformed tag file must not take the rest down
        }
        data::TagFile tag;
        if (!data::Codec<data::TagFile>::read(root, tag)) {
            continue;
        }
        if (tag.replace) {
            out.clear();
        }
        for (const auto& entry : tag.values) {
            if (entry.id.empty()) {
                continue;
            }
            if (entry.id.front() == '#') {
                collectFunctionTag(resources, functionTagLocation(std::string_view{entry.id}.substr(1U)),
                                   depth + 1, visited, known, out);
                continue;
            }
            if (known.contains(entry.id)) {
                out.push_back(entry.id);
            }
        }
    }
}

} // namespace

void FunctionManager::reset() {
    functions_.clear();
    tickFunctions_.clear();
    loadFunctions_.clear();
    activeBudget_ = nullptr;
}

void FunctionManager::load(const command::CommandDispatcher& dispatcher,
                           const assets::ResourceProvider& resources) {
    // /reload's contract (PACK-DESIGN §4, the card's #1): a rebuild replaces the
    // whole compiled set, never merges onto what a previous load left — the
    // same "reset then reapply the current stack" discipline
    // PerSaveDataStack::rebuild follows for its five tables. Only the
    // `minecraft` namespace's functions/ tree is scanned for *discovery* (see
    // the header's namespace-scope note); a compiled function can still be
    // called under any id, including one outside that namespace, once it
    // exists in functions_.
    reset();

    for (const auto& location :
        resources.list("minecraft", "functions", assets::PackType::ServerData)) {
        if (location.path.size() < 11U ||
            location.path.substr(location.path.size() - 11U) != ".mcfunction") {
            continue; // functions/ may hold nothing else, but a stray file must not crash discovery
        }
        const auto bytes = resources.readBytes(location);
        if (bytes.empty()) {
            continue;
        }
        CompiledFunction compiled;
        compiled.id = functionIdFromLocation(location);
        const std::string_view text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        forEachLine(text, [&](std::string_view rawLine) {
            const std::string_view line = trimLine(rawLine);
            // Blank lines and `#`-prefixed comments are skipped at compile
            // time, not carried as no-op ParseResults — so a commented-out line
            // costs nothing at runtime and never counts against the command
            // budget.
            if (line.empty() || line.front() == '#') {
                return;
            }
            // A vanilla .mcfunction line never carries a leading '/' (that is
            // chat-only syntax); tolerate one anyway in case a line was
            // copy-pasted from chat, since CommandDispatcher::parse() expects
            // the line with any leading slash already stripped (see execute()).
            const std::string_view command = line.front() == '/' ? line.substr(1U) : line;
            // parse() is the one and only time this line's text is walked.
            // executeParsed() (runCompiledShared() below) replays this exact
            // ParseResults every time the function fires — zero re-parsing on
            // the hot path, the card's central discipline.
            compiled.lines.push_back(dispatcher.parse(command, std::nullopt));
        });
        functions_.emplace(compiled.id, std::move(compiled));
    }

    // Function tags: #minecraft:tick and #minecraft:load are just two
    // conventionally-named tags among however many a pack defines — expand
    // every tags/functions/*.json this stack carries, but only the two this
    // card actually schedules (tick every authoritative tick, load once) are
    // kept as named lists; an arbitrary third-party tag a datapack defines
    // purely so /function can target a group via # is discoverable through the
    // same collectFunctionTag a future consumer would reuse, but this class
    // has no reader for it yet (noted as a deferred gap, not a silent one).
    const auto expand = [&](const char* tagName, std::vector<std::string>& out) {
        std::unordered_set<std::string> visited;
        collectFunctionTag(resources, assets::data(std::string{"tags/functions/"} + tagName + ".json"),
                           0, visited, functions_, out);
        // Deterministic order (REGULAR §3 / the card's #2): sorted, not
        // discovery/hash order, so two runs (or two members inserted in a
        // different pack-merge order) always iterate identically.
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    };
    expand("tick", tickFunctions_);
    expand("load", loadFunctions_);
}

CommandResult FunctionManager::run(const command::CommandDispatcher& dispatcher, const std::string& id,
                                   const command::CommandSource& source) {
    // Top-level entry (activeBudget_ is null): own a fresh budget for this
    // whole call tree. A nested entry (a /function line inside a function,
    // dispatched back here through the "function" command's handler) finds
    // activeBudget_ already set by the enclosing runCompiled and shares it —
    // see the header's RunBudget note for why that is what makes the caps
    // bound the whole tree rather than resetting per nesting level.
    if (activeBudget_ != nullptr) {
        return runCompiledShared(dispatcher, id, source, *activeBudget_);
    }
    RunBudget budget;
    return runCompiledShared(dispatcher, id, source, budget);
}

CommandResult FunctionManager::runCompiledShared(const command::CommandDispatcher& dispatcher,
                                                  const std::string& id,
                                                  const command::CommandSource& source,
                                                  RunBudget& budget) {
    if (budget.depth >= kMaxRecursionDepth) {
        budget.haltedByGuardrail = true;
        return {false, "/function recursion too deep (limit " +
                           std::to_string(kMaxRecursionDepth) + ")"};
    }
    const auto found = functions_.find(id);
    if (found == functions_.end()) {
        return {false, "Unknown function: " + id};
    }

    RunBudget* const previous = activeBudget_;
    activeBudget_ = &budget;
    ++budget.depth;

    std::size_t ran = 0;
    bool haltedOnBudget = false;
    for (auto& line : found->second.lines) {
        if (budget.commandsRun >= maxCommandsPerInvocation_) {
            haltedOnBudget = true;
            budget.haltedByGuardrail = true;
            break;
        }
        ++budget.commandsRun;
        // executeParsed(), never execute(std::string_view, …): this line's
        // ParseResults was produced exactly once, at load(); running it here
        // costs no re-parse no matter how many times this function fires (a
        // #tick member, for instance, runs it 20 times a second). One line's
        // own failure (a bad block id, a refused permission) does not abort
        // the rest of the function — vanilla keeps running a function's
        // remaining lines after one fails — so the per-line result is
        // observed only for the budget it already paid, not rethrown. A
        // guardrail trip deep in a nested /function call is different: it
        // sets budget.haltedByGuardrail, which is checked below so it
        // propagates to every enclosing frame instead of being swallowed the
        // way an ordinary line failure is.
        static_cast<void>(dispatcher.executeParsed(line, source));
        ++ran;
        if (budget.haltedByGuardrail) {
            break; // a nested call already hit a cap; unwind without running more lines
        }
    }

    --budget.depth;
    activeBudget_ = previous;

    if (haltedOnBudget) {
        return {false, "/function " + id + " halted: exceeded the " +
                           std::to_string(maxCommandsPerInvocation_) + "-command chain limit"};
    }
    if (budget.haltedByGuardrail) {
        // A nested call (possibly several frames down) already hit a cap and
        // reported it; this frame's own contribution ran fine, but the whole
        // top-level invocation this frame is part of must still surface as
        // halted, so the caller that started it all (chat's /function, a
        // #tick member) observes the guardrail rather than a false success.
        return {false, "/function " + id + " halted: a nested call exceeded a guardrail"};
    }
    return {true, "Ran " + std::to_string(ran) + " command(s) from " + id};
}

void FunctionManager::runTick(const command::CommandDispatcher& dispatcher,
                              const command::CommandSource& source) {
    // Each #tick member is its own top-level invocation (its own RunBudget),
    // so one heavy member cannot starve the next's command budget — mirrors
    // vanilla running each tag member as an independent /function call.
    for (const auto& id : tickFunctions_) {
        static_cast<void>(run(dispatcher, id, source));
    }
}

void FunctionManager::runLoad(const command::CommandDispatcher& dispatcher,
                              const command::CommandSource& source) {
    for (const auto& id : loadFunctions_) {
        static_cast<void>(run(dispatcher, id, source));
    }
}

} // namespace mc::gameplay

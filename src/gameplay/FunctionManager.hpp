#pragma once

// PACK-2: the mcfunction system — `.mcfunction` files, `/function`, the
// `#minecraft:tick`/`#minecraft:load` function tags, and `/reload`.
//
// A function is a command list, not a new language (PACK-DESIGN §4, contrast
// [[molang-runtime-plan]]): every line is a command the existing CMD dispatcher
// already knows how to parse and run. So this class adds no interpreter of its
// own — it calls gameplay::command::CommandDispatcher::parse() once per line at
// load time (a `.mcfunction`'s compiled form is just a cached
// gameplay::command::ParseResults per line) and CommandDispatcher::executeParsed()
// every time the function actually runs. That split is what makes "/function
// test:hello" (and every `#tick` member, every tick) zero-reparse: the token
// walk, literal/argument matching and permission fold happened exactly once,
// at load or /reload, never on the hot path.
//
// Tags reuse data::TagFile — the exact codec/expansion shape BlockTagTable
// already uses for `#minecraft:mineable/pickaxe` and friends (`#`-prefixed
// values recurse into another tag, a cycle terminates, "replace" truncates the
// lower-priority members) — because JE's function tags and block tags are the
// same file format, just a different content root
// (`data/<ns>/tags/functions/*.json` instead of `tags/block/*.json`). Function
// tag membership is a ResourceLocation, not a bitset (a table sized "how many
// blocks exist" makes no sense for "how many functions could a pack define"),
// but the merge/expand algorithm is identical, so the two headers describe one
// idea from two angles rather than two designs.
//
// Only the `minecraft` namespace is scanned for function *discovery* and *tag*
// membership — the same simplification RecipeTable::applyOverlay and
// LootTable::applyOverlay already made for `resources.list("minecraft", …)`
// (multi-namespace pack discovery is a PACK-DESIGN gap noted there, not
// reopened here). A pack function can still live in and be *called* from any
// namespace via its `ns:path` id — this only bounds which namespace's
// `functions/` and `tags/functions/` directories are walked to *find* one.

#include "gameplay/CommandResult.hpp"
#include "gameplay/command/CommandDispatcher.hpp"
#include "gameplay/command/CommandSource.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace mc::assets {
class ResourceProvider;
}

namespace mc::gameplay {

// One compiled `.mcfunction`: its lines, already walked through the dispatcher
// once. Runtime replay touches only this — never the source text, never
// parse() — which is the "compile once" contract PACK-2 asks for.
struct CompiledFunction final {
    std::string id;  // "ns:path", the key callers/#tick/#load reference
    std::vector<command::ParseResults> lines;
};

class FunctionManager final {
  public:
    // Vanilla parity guardrails (Commands.java's maxCommandChainLength / the
    // /function recursion vanilla silently bounds the same way): a single
    // /function invocation (including everything it calls, transitively) may
    // not run more than the command budget below allows, and a nested
    // /function-inside-a-function chain may not exceed kMaxRecursionDepth
    // levels. Either cap halts the *offending* function call with a failure
    // result — it does not hang, recurse the C++ stack, or take the process
    // down. The command budget is now vanilla's `max_command_sequence_length`
    // game rule (the renamed maxCommandChainLength); the constant below is its
    // default, which the runtime overwrites with the world's rule value. 256 is
    // a generous but finite recursion ceiling no legitimate datapack script
    // approaches (vanilla has no formal recursion limit but every real datapack
    // stays in the single digits — this exists purely to turn a runaway
    // `function loop() -> function loop()` into a clean failure), and stays a
    // constant because vanilla has no rule for it.
    static constexpr std::size_t kDefaultMaxCommandsPerInvocation = 65536;
    static constexpr std::size_t kMaxRecursionDepth = 256;

    // `/gamerule max_command_sequence_length`. Defaults to vanilla's default, so
    // a manager nobody configures (every headless test) is unchanged.
    void setMaximumCommandsPerInvocation(std::size_t maximum) {
        maxCommandsPerInvocation_ = maximum;
    }
    [[nodiscard]] std::size_t maximumCommandsPerInvocation() const {
        return maxCommandsPerInvocation_;
    }

    // Discovers and compiles every `.mcfunction` under this stack's
    // `data/minecraft/functions/**` (recursively — vanilla nests functions in
    // subfolders freely), then expands the `#minecraft:tick` and
    // `#minecraft:load` tags (plus any other `tags/functions/*.json` a pack
    // defines) against the now-known function ids. `dispatcher` is the tree
    // every line parses against — the same one `/function`'s runtime replay and
    // ordinary chat commands share, so a function can call any command the
    // player can. Replaces whatever this manager held before (a `/reload`
    // rebuild, exactly like PerSaveDataStack::rebuild resets its five tables):
    // idempotent, no residue from the previous stack.
    void load(const command::CommandDispatcher& dispatcher, const assets::ResourceProvider& resources);

    // Drops every compiled function and tag — the built-in floor (nothing) a
    // world with no datapack functions, or a freshly unloaded world, sits at.
    void reset();

    [[nodiscard]] bool contains(const std::string& id) const { return functions_.contains(id); }
    [[nodiscard]] std::size_t functionCount() const { return functions_.size(); }

    // `#minecraft:tick`'s members, in the same deterministic (sorted) order
    // every call — what GameRuntime::tick() runs each authoritative tick.
    [[nodiscard]] const std::vector<std::string>& tickFunctions() const { return tickFunctions_; }
    // `#minecraft:load`'s members, sorted the same way — run once after
    // load() finishes (world load, and every `/reload`), never per tick.
    [[nodiscard]] const std::vector<std::string>& loadFunctions() const { return loadFunctions_; }

    // Runs a compiled function's lines in file order against `source`. A line
    // that is itself `/function <id>` recurses through the dispatcher exactly
    // like any other command — GameRuntime wires the "function" command's
    // handler to call this same method again, and run() detects that
    // re-entrancy (activeBudget_ is already set) and shares the in-flight
    // budget instead of starting a fresh one, so `function a` calling
    // `function b` calling `function a` still trips the caps instead of
    // resetting them per nesting level, and a top-level call from chat or a
    // #tick member always starts at depth 0 with a full command budget.
    // Returns a failure (never hangs, never recurses the C++ stack past
    // kMaxRecursionDepth) when `id` is unknown or a guardrail is hit.
    [[nodiscard]] CommandResult run(const command::CommandDispatcher& dispatcher, const std::string& id,
                                    const command::CommandSource& source);

    // Runs every `#minecraft:tick` member, in order, as `source` — called once
    // per authoritative tick (GameRuntime::tick()). Each member is its own
    // top-level invocation (its own fresh guardrail budget), so one heavy
    // tick function cannot starve the command budget of the next.
    void runTick(const command::CommandDispatcher& dispatcher, const command::CommandSource& source);

    // Runs every `#minecraft:load` member, in order, as `source` — called once
    // right after load() (world load or /reload finishing its rebuild), never
    // from the tick loop.
    void runLoad(const command::CommandDispatcher& dispatcher, const command::CommandSource& source);

  private:
    // Recursion/command-count accounting for one top-level run() call (and
    // everything it transitively calls via a nested /function line, however
    // deep). A fresh instance is created only when run() is entered from
    // outside (activeBudget_ is null); a nested call — a /function line inside
    // a function, dispatched through the ordinary command tree back into
    // run() — finds activeBudget_ already set and shares it, so the caps bound
    // the whole call tree, not each nesting level individually.
    struct RunBudget final {
        std::size_t commandsRun = 0;
        std::size_t depth = 0;
        // Sticky once a guardrail fires anywhere in this call tree: a nested
        // `/function` line that trips the depth or command cap deep inside a
        // recursive chain must make the *top-level* run() report failure too
        // (not just the innermost frame, whose caller — an ordinary
        // executeParsed() — discards a single line's own result the same way
        // vanilla keeps running a function's later lines after one fails). Once
        // set, every enclosing frame's return is forced to failure as it
        // unwinds, so the caller that started the whole call — chat's
        // /function, a #tick member — observably sees "halted", matching the
        // card's guardrail acceptance test.
        bool haltedByGuardrail = false;
    };

    // The shared body run() delegates to: executes `id`'s cached lines against
    // `source`, charging `budget` per command and refusing to push `budget`'s
    // depth past kMaxRecursionDepth. Every entry point (top-level run(),
    // runTick(), runLoad()) funnels through here with either a brand-new budget
    // or the caller's shared one, so there is exactly one place that enforces
    // both guardrails.
    [[nodiscard]] CommandResult runCompiledShared(const command::CommandDispatcher& dispatcher,
                                                  const std::string& id,
                                                  const command::CommandSource& source,
                                                  RunBudget& budget);

    std::size_t maxCommandsPerInvocation_ = kDefaultMaxCommandsPerInvocation;
    std::unordered_map<std::string, CompiledFunction> functions_;
    std::vector<std::string> tickFunctions_;
    std::vector<std::string> loadFunctions_;
    // The budget of the run() call currently unwinding through the dispatcher —
    // non-null only while a function (top-level or nested) is executing, so a
    // `/function` line encountered mid-replay (GameRuntime's "function" command
    // handler calls run() again) finds and shares it instead of starting a
    // fresh, ungoverned one. Null between calls — every other command runs
    // through the ordinary dispatcher path untouched.
    RunBudget* activeBudget_ = nullptr;
};

} // namespace mc::gameplay

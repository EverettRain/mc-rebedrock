#include "gameplay/GameMode.hpp"
#include "gameplay/command/CommandDispatcher.hpp"
#include "gameplay/command/CommandSource.hpp"
#include "gameplay/command/GameplayArguments.hpp"
#include "gameplay/entities/EntityRegistry.hpp"

#include <cassert>
#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using mc::gameplay::command::CommandContext;
using mc::gameplay::command::CommandDispatcher;
using mc::gameplay::command::CommandSource;
using mc::gameplay::command::GreedyStringArgument;
using mc::gameplay::command::kEntityTargetArgument;
using mc::gameplay::command::kGameModeArgument;
using mc::gameplay::command::kGameRuleArgument;
using mc::gameplay::command::kGiveItemArgument;
using mc::gameplay::command::kIntArgument;
using mc::gameplay::command::kRotationArgument;
using mc::gameplay::command::kStringArgument;
using mc::gameplay::command::kTeleportDestinationArgument;
using mc::gameplay::command::kTimeArgument;
using mc::gameplay::CommandResult;

namespace {
bool hasText(const std::vector<mc::gameplay::command::Suggestion>& suggestions,
             std::string_view text) {
    for (const auto& suggestion : suggestions) {
        if (suggestion.text == text) return true;
    }
    return false;
}
} // namespace

int main() {
    // Every built-in species must be registered before entity-target commands
    // validate their arguments.
    mc::gameplay::entities::registerBuiltinEntities();
    CommandDispatcher dispatcher;

    // /gamemode <mode> — typed GameMode argument.
    mc::gameplay::GameMode mode = mc::gameplay::GameMode::Creative;
    dispatcher.literal("gamemode")
        .argument("mode", kGameModeArgument)
        .executes([&](const CommandContext& context) {
            const auto parsed = context.find<mc::gameplay::GameMode>("mode");
            if (!parsed.has_value()) {
                return CommandResult{false, "Usage: /gamemode <survival|creative>"};
            }
            mode = *parsed;
            return CommandResult{true, std::string{mc::gameplay::gameModeName(mode)}};
        });
    assert(dispatcher.contains("gamemode"));
    assert(!dispatcher.contains("GAMEMODE")); // literals are case-sensitive, like Brigadier
    assert(!dispatcher.contains("missing"));
    assert(dispatcher.commandCount() == 1U);

    auto result = dispatcher.execute("/gamemode survival");
    assert(result.success);
    assert(mode == mc::gameplay::GameMode::Survival);
    assert(!dispatcher.execute("/GAMEMODE creative").success); // case-sensitive
    assert(!dispatcher.execute("/gamemode spectator").success);     // GameModeArgument rejects
    assert(!dispatcher.execute("/missing").success);
    assert(!dispatcher.execute("gamemode survival").success);       // must start with /
    assert(!dispatcher.execute("/gamemode").success);               // incomplete
    assert(!dispatcher.execute("/gamemode survival extra").success); // extra token

    // /time set <time> — TimeArgument binds the resolved ticks as a double.
    double lastTicks = -1.0;
    dispatcher.literal("time")
        .then("set")
        .argument("time", kTimeArgument)
        .executes([&](const CommandContext& context) {
            const auto ticks = context.find<double>("time");
            if (!ticks.has_value()) {
                return CommandResult{false, "Usage: /time set <day|noon|night|midnight|ticks>"};
            }
            lastTicks = *ticks;
            return CommandResult{true, "ok"};
        });
    assert(dispatcher.execute("/time set noon").success);
    assert(lastTicks == 6000.0);
    assert(dispatcher.execute("/time set 24001").success);
    assert(lastTicks == 1.0);
    assert(!dispatcher.execute("/time day").success);          // missing the literal `set`
    assert(!dispatcher.execute("/time nope").success);         // unknown subcommand
    assert(!dispatcher.execute("/time set").success);          // incomplete
    assert(!dispatcher.execute("/time set tomorrow").success); // TimeArgument rejects

    // The old parseTimeOfDay contract survives on the command module.
    assert(mc::gameplay::command::parseTimeOfDay("day") == 1'000.0);
    assert(mc::gameplay::command::parseTimeOfDay("NOON") == 6'000.0);
    assert(mc::gameplay::command::parseTimeOfDay("24001") == 1.0);
    assert(!mc::gameplay::command::parseTimeOfDay("tomorrow").has_value());

    // /give <item> <count> — GiveItemArgument validates the identifier up front.
    std::string givenItem;
    std::int64_t givenCount = 0;
    dispatcher.literal("give")
        .argument("item", kGiveItemArgument)
        .argument("count", kIntArgument)
        .executes([&](const CommandContext& context) {
            const auto item = context.find<std::string>("item");
            const auto count = context.find<std::int64_t>("count");
            if (!item.has_value() || !count.has_value()) {
                return CommandResult{false, "Usage: /give <item|index> [count]"};
            }
            givenItem = *item;
            givenCount = *count;
            return CommandResult{true, "gave"};
        });
    assert(dispatcher.execute("/give minecraft:acacia_planks 3").success);
    assert(givenItem == "minecraft:acacia_planks");
    assert(givenCount == 3);
    assert(dispatcher.execute("/give 0 1").success); // numeric creative-catalog index
    assert(givenCount == 1);
    assert(dispatcher.execute("/give \"minecraft:oak_planks\" 5").success); // quoted
    assert(givenItem == "minecraft:oak_planks");
    assert(givenCount == 5);
    // Both namespaces parse (rebedrock is the outward identity, minecraft is a
    // tolerated vanilla alias), so either form gives the same item.
    assert(dispatcher.execute("/give rebedrock:glass 2").success);
    assert(givenItem == "rebedrock:glass");
    assert(givenCount == 2);
    assert(!dispatcher.execute("/give unknown_thing 3").success); // GiveItemArgument rejects
    assert(!dispatcher.execute("/give minecraft:acacia_planks").success); // missing count
    assert(!dispatcher.execute("/give minecraft:acacia_planks abc").success); // IntArgument rejects

    // /gamerule <rule> [<value>] — the optional trailing argument is an
    // executable argument node that is also the parent of the next argument.
    bool queried = false;
    bool set = false;
    dispatcher.literal("gamerule")
        .argument("rule", kGameRuleArgument)
        .executes([&](const CommandContext& context) {
            const auto rule = context.find<std::string>("rule");
            if (!rule.has_value()) {
                return CommandResult{false, "Usage"};
            }
            queried = *rule == "keepInventory";
            return CommandResult{true, "query"};
        })
        .argument("value", kStringArgument)
        .executes([&](const CommandContext& context) {
            const auto rule = context.find<std::string>("rule");
            const auto value = context.find<std::string>("value");
            if (!rule.has_value() || !value.has_value()) {
                return CommandResult{false, "Usage"};
            }
            set = *rule == "keepInventory" && *value == "true";
            return CommandResult{true, "set"};
        });
    assert(dispatcher.execute("/gamerule keepInventory").success);
    assert(queried);
    assert(!set);
    assert(dispatcher.execute("/gamerule keepInventory true").success);
    assert(set);
    assert(!dispatcher.execute("/gamerule notARule").success); // GameRuleTable rejects
    assert(!dispatcher.execute("/gamerule").success);          // incomplete

    // /tp <x> <y> <z> [<yaw> <pitch>] and /tp <entity> — the destination is a
    // position (relative `~` axes allowed) or a registered entity id.
    std::optional<mc::gameplay::command::Position3> tpPosition;
    std::optional<mc::gameplay::command::Rotation2> tpRotation;
    std::string tpEntity;
    const auto recordTp = [&](const CommandContext& context) {
        tpPosition = context.find<mc::gameplay::command::Position3>("destination");
        tpRotation = context.find<mc::gameplay::command::Rotation2>("rotation");
        tpEntity = context.find<std::string>("destination").value_or("");
        return CommandResult{true, "ok"};
    };
    dispatcher.literal("tp")
        .argument("destination", kTeleportDestinationArgument)
        .executes(recordTp)
        .argument("rotation", kRotationArgument)
        .executes(recordTp);

    assert(dispatcher.execute("/tp 100 64 100").success);
    assert(tpPosition.has_value());
    assert(tpPosition->x == 100 && tpPosition->y == 64 && tpPosition->z == 100);
    assert(!tpPosition->relativeX && !tpPosition->relativeY && !tpPosition->relativeZ);
    assert(tpEntity.empty());
    assert(!tpRotation.has_value());
    // `~`-relative axes keep their flag for the handler to resolve.
    assert(dispatcher.execute("/tp ~ 5 ~").success);
    assert(tpPosition->relativeX && !tpPosition->relativeY && tpPosition->relativeZ);
    assert(tpPosition->y == 5);
    // Rotation only follows a position.
    assert(dispatcher.execute("/tp 1 2 3 90 0").success);
    assert(tpPosition->x == 1);
    assert(tpRotation.has_value());
    assert(tpRotation->yaw == 90 && tpRotation->pitch == 0);
    // An entity destination binds a string instead.
    assert(dispatcher.execute("/tp pig").success);
    assert(tpEntity == "pig");
    assert(!tpPosition.has_value());
    // Errors: missing coordinates, unknown entity, bad rotation.
    assert(!dispatcher.execute("/tp").success);
    assert(!dispatcher.execute("/tp 1 2").success);
    assert(!dispatcher.execute("/tp notanentity").success);
    assert(!dispatcher.execute("/tp 1 2 3 bad 0").success);

    // /spawnpoint — executable with no arguments (use the player's own block),
    // and the optional position shares /tp's coordinate parser.
    std::optional<mc::gameplay::command::Position3> spawnPosition;
    bool spawnpointSelf = false;
    const auto recordSpawn = [&](const CommandContext& context) {
        spawnPosition = context.find<mc::gameplay::command::Position3>("pos");
        // An entity-id destination is not a spawn position (the renderer's
        // handler rejects it the same way).
        if (context.find<std::string>("pos").has_value()) {
            return CommandResult{false, "Usage: /spawnpoint [<x> <y> <z>]"};
        }
        spawnpointSelf = !spawnPosition.has_value();
        return CommandResult{true, "ok"};
    };
    dispatcher.literal("spawnpoint")
        .executes(recordSpawn)
        .argument("pos", kTeleportDestinationArgument)
        .executes(recordSpawn);
    assert(dispatcher.execute("/spawnpoint").success);
    assert(spawnpointSelf);
    assert(dispatcher.execute("/spawnpoint 100 65 100").success);
    assert(!spawnpointSelf);
    assert(spawnPosition.has_value());
    assert(spawnPosition->x == 100 && spawnPosition->y == 65 && spawnPosition->z == 100);
    assert(dispatcher.execute("/spawnpoint ~ 70 ~").success);
    assert(spawnPosition->relativeX && !spawnPosition->relativeY && spawnPosition->relativeZ);
    assert(spawnPosition->y == 70);
    // Missing coordinates are rejected; an entity id is not a spawn position.
    assert(!dispatcher.execute("/spawnpoint 1 2").success);
    assert(!dispatcher.execute("/spawnpoint pig").success);

    // /kill kills the player by default; /kill <target> accepts the player
    // keyword or a registered entity id.
    bool killedSelf = false;
    std::string killTarget;
    dispatcher.literal("kill")
        .executes([&](const CommandContext&) {
            killedSelf = true;
            return CommandResult{true, "ok"};
        })
        .argument("target", kEntityTargetArgument)
        .executes([&](const CommandContext& context) {
            killTarget = context.find<std::string>("target").value_or("");
            return CommandResult{true, "ok"};
        });

    assert(dispatcher.execute("/kill").success);
    assert(killedSelf);
    assert(dispatcher.execute("/kill player").success);
    assert(killTarget == "player");
    assert(dispatcher.execute("/kill pig").success);
    assert(killTarget == "pig");
    assert(!dispatcher.execute("/kill notathing").success); // unknown target

    // A custom argument type binding a value the old variant never listed
    // (float) — proof that CommandContext's type-erasure lets new argument
    // types ship without touching the command core, and that the pure-function
    // parse returns values for the dispatcher to bind.
    class SpeedArgument final : public mc::gameplay::command::ArgumentType {
      public:
        mc::gameplay::command::ArgumentParseResult
        parse(mc::gameplay::command::StringReader& reader) const override {
            const auto token = reader.readString();
            if (!token.has_value()) {
                return mc::gameplay::command::parseFail("Expected a speed", reader);
            }
            float speed = 0.0F;
            const auto [end, error] =
                std::from_chars(token->data(), token->data() + token->size(), speed);
            if (error != std::errc{} || end != token->data() + token->size()) {
                return mc::gameplay::command::parseFail("Invalid speed: " + *token, reader);
            }
            return mc::gameplay::command::parseOk(speed);
        }
        void collectSuggestions(mc::gameplay::command::SuggestionSink&,
                                const mc::gameplay::command::CommandContext&) const override {}
    };
    static SpeedArgument speedArgument; // local instance; outlives the dispatcher
    float lastSpeed = 0.0F;
    dispatcher.literal("speed")
        .argument("value", speedArgument)
        .executes([&](const CommandContext& context) {
            const auto speed = context.find<float>("value");
            if (!speed.has_value()) {
                return CommandResult{false, "Usage"};
            }
            lastSpeed = *speed;
            return CommandResult{true, "ok"};
        });
    assert(dispatcher.execute("/speed 3.5").success);
    assert(lastSpeed == 3.5F);
    assert(!dispatcher.execute("/speed fast").success);

    // GreedyStringArgument reads to the end of the line, spaces included — the
    // `/say <message>` shape.
    std::string echoed;
    dispatcher.literal("echo")
        .argument("message", mc::gameplay::command::kGreedyStringArgument)
        .executes([&](const CommandContext& context) {
            echoed = context.find<std::string>("message").value_or("");
            return CommandResult{true, "ok"};
        });
    assert(dispatcher.execute("/echo hello world 123").success);
    assert(echoed == "hello world 123");
    assert(!dispatcher.execute("/echo").success); // greedy still needs a value

    // Range-checked IntArgument rejects values outside its floor/ceiling.
    static mc::gameplay::command::IntArgument rollArgument{1, 100};
    std::int64_t rolled = 0;
    dispatcher.literal("roll")
        .argument("value", rollArgument)
        .executes([&](const CommandContext& context) {
            rolled = context.find<std::int64_t>("value").value_or(0);
            return CommandResult{true, "ok"};
        });
    assert(dispatcher.execute("/roll 50").success);
    assert(rolled == 50);
    assert(!dispatcher.execute("/roll 0").success);   // below floor
    assert(!dispatcher.execute("/roll 101").success); // above ceiling

    // Idempotent re-registration: the same path reuses its nodes and overwrites
    // the handler, leaving the command count unchanged.
    bool replacementCalled = false;
    dispatcher.literal("gamemode")
        .argument("mode", kGameModeArgument)
        .executes([&](const CommandContext&) {
            replacementCalled = true;
            return CommandResult{true, ""};
        });
    assert(dispatcher.commandCount() == 10U); // gamemode, time, give, gamerule, tp, kill, spawnpoint, speed, echo, roll
    assert(dispatcher.execute("/gamemode creative").success);
    assert(replacementCalled);

    // Missing argument keys resolve to nullopt; wrong types resolve to nullopt
    // rather than a variant access crash.
    CommandContext isolated;
    assert(!isolated.has("nope"));
    assert(!isolated.find<std::int64_t>("nope").has_value());
    isolated.bind("x", std::int64_t{7});
    assert(isolated.find<std::int64_t>("x") == 7);
    assert(!isolated.find<std::string>("x").has_value());

    // CMD-7: the redirect / source-fork machinery `execute` is built on. A tiny
    // `exec` subtree exercises forking, gating, redirect-to-root, and completion
    // across a redirect — the same primitives GameRuntime wires the real clauses
    // onto — without depending on the world.
    {
        CommandDispatcher redirect;
        std::vector<std::uint64_t> marks; // one entry per source the terminal ran as
        // Terminal command: records which source ran it (by executor entity id).
        redirect.literal("mark").executes([&](const CommandContext& context) {
            marks.push_back(context.hasSource() ? context.source().entityId : 0U);
            return CommandResult{true, ""};
        });
        auto exec = redirect.literal("exec");
        const std::size_t execNode = exec.nodeId();
        const std::size_t rootNode = redirect.rootId();
        // `spread <n>`: fork the incoming source into n copies, each tagged with a
        // distinct executor id, then continue the clause chain.
        redirect.builderAt(execNode)
            .then("spread")
            .argument("n", kIntArgument)
            .redirectTo(execNode)
            .modifiesSource([](const CommandContext& args, const CommandSource& incoming,
                               std::vector<CommandSource>& out) -> std::string {
                const auto n = args.find<std::int64_t>("n");
                if (!n.has_value() || *n < 0) return "spread: expected a count";
                for (std::int64_t index = 0; index < *n; ++index) {
                    out.push_back(incoming.withExecutorEntity(static_cast<std::uint64_t>(index)));
                }
                return "";
            });
        // `keep <flag>`: gate — the source survives only when flag != 0.
        redirect.builderAt(execNode)
            .then("keep")
            .argument("flag", kIntArgument)
            .redirectTo(execNode)
            .modifiesSource([](const CommandContext& args, const CommandSource& incoming,
                               std::vector<CommandSource>& out) -> std::string {
                const auto flag = args.find<std::int64_t>("flag");
                if (!flag.has_value()) return "keep: expected a flag";
                if (*flag != 0) out.push_back(incoming);
                return "";
            });
        // `run` redirects to the root: the tail parses as a fresh command.
        redirect.builderAt(execNode).then("run").redirectTo(rootNode);

        // Fork: three sources, so `mark` runs three times, once per tagged source.
        marks.clear();
        auto forked = redirect.execute("/exec spread 3 run mark");
        assert(forked.success);
        assert((marks == std::vector<std::uint64_t>{0U, 1U, 2U}));

        // Nesting composes left to right: 2 × 2 = 4 sources reach the terminal.
        marks.clear();
        assert(redirect.execute("/exec spread 2 run exec spread 2 run mark").success);
        assert(marks.size() == 4U);

        // Gate that passes runs once; gate that fails runs nothing (and reports
        // failure, so a forked `run` that matched no source is not a silent win).
        marks.clear();
        assert(redirect.execute("/exec keep 1 run mark").success);
        assert(marks.size() == 1U);
        marks.clear();
        assert(!redirect.execute("/exec keep 0 run mark").success);
        assert(marks.empty());

        // Incomplete: a clause chain with no `run` executes nothing.
        marks.clear();
        assert(!redirect.execute("/exec spread 3").success);
        assert(marks.empty());

        // The fork guard trips before a fork bomb exhausts memory.
        marks.clear();
        assert(!redirect.execute("/exec spread 70000 run mark").success);
        assert(marks.empty());

        // Completion follows the redirect: after a clause the target's children
        // are offered, and after `run` every root command is.
        auto clauseSuggest = redirect.suggestions("/exec ", 6);
        assert(hasText(clauseSuggest, "spread"));
        assert(hasText(clauseSuggest, "keep"));
        assert(hasText(clauseSuggest, "run"));
        auto afterClause = redirect.suggestions("/exec spread 3 ", 15);
        assert(hasText(afterClause, "run"));
        auto afterRun = redirect.suggestions("/exec spread 3 run ", 19);
        assert(hasText(afterRun, "mark"));
        assert(hasText(afterRun, "exec"));
        auto partialRun = redirect.suggestions("/exec spread 3 ru", 17);
        assert(hasText(partialRun, "run") && partialRun.size() == 1U);
    }
    return 0;
}

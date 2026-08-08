#include "gameplay/GameMode.hpp"
#include "gameplay/command/CommandDispatcher.hpp"
#include "gameplay/command/GameplayArguments.hpp"
#include "gameplay/entities/EntityRegistry.hpp"

#include <cassert>
#include <charconv>
#include <memory>
#include <optional>
#include <string>

using mc::gameplay::command::CommandContext;
using mc::gameplay::command::CommandDispatcher;
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
        void collectSuggestions(mc::gameplay::command::SuggestionSink&) const override {}
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
    assert(dispatcher.commandCount() == 9U); // gamemode, time, give, gamerule, tp, kill, speed, echo, roll
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
    return 0;
}

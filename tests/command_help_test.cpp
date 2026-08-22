#include "gameplay/command/ArgumentType.hpp"
#include "gameplay/command/CommandDispatcher.hpp"
#include "gameplay/command/CommandSource.hpp"
#include "gameplay/command/GameplayArguments.hpp"

#include <cassert>
#include <string>
#include <vector>

using mc::gameplay::CommandResult;
using mc::gameplay::command::CommandContext;
using mc::gameplay::command::CommandDispatcher;
using mc::gameplay::command::CommandSource;
using mc::gameplay::command::kGameModeArgument;
using mc::gameplay::command::kStringArgument;
using mc::gameplay::command::kTeleportDestinationArgument;
using mc::gameplay::command::kWeatherDurationArgument;
using mc::gameplay::command::PermissionLevel;

namespace {
const auto kOk = [](const CommandContext&) { return CommandResult{true, ""}; };

// Registers the three weather subcommands (each executable with an optional
// duration), mirroring the real command's shape.
void registerWeather(CommandDispatcher& dispatcher) {
    for (const char* kind : {"clear", "rain", "thunder"}) {
        dispatcher.literal("weather")
            .requiresLevel(PermissionLevel::GameMasters)
            .then(kind)
            .executes(kOk)
            .argument("duration", kWeatherDurationArgument)
            .executes(kOk);
    }
}
} // namespace

int main() {
    CommandDispatcher dispatcher;
    dispatcher.literal("gamemode")
        .requiresLevel(PermissionLevel::GameMasters)
        .argument("mode", kGameModeArgument)
        .executes(kOk);
    dispatcher.literal("gamerule")
        .requiresLevel(PermissionLevel::GameMasters)
        .argument("rule", kStringArgument)
        .executes(kOk)
        .argument("value", kStringArgument)
        .executes(kOk);
    dispatcher.literal("tp").argument("pos", kTeleportDestinationArgument).executes(kOk);
    dispatcher.literal("seed").requiresLevel(PermissionLevel::GameMasters).executes(kOk);
    registerWeather(dispatcher);

    CommandSource owner; // op4
    CommandSource op1;
    op1.permissionLevel = PermissionLevel::Moderators;

    // ---- smart-usage shapes --------------------------------------------------
    assert(dispatcher.usage("gamemode", owner) == "gamemode <mode>");
    assert(dispatcher.usage("gamerule", owner) == "gamerule <rule> [<value>]");
    // Literal siblings with a shared optional tail factor into one group.
    assert(dispatcher.usage("weather", owner) == "weather (clear|rain|thunder) [<duration>]");
    // A Vec3 argument renders its three coordinates (the type's usageHint), not
    // the bare node name.
    assert(dispatcher.usage("tp", owner) == "tp <x> <y> <z>");
    // A no-argument command is just its name.
    assert(dispatcher.usage("seed", owner) == "seed");
    // An unknown command has no usage.
    assert(dispatcher.usage("nope", owner).empty());

    // ---- op filtering --------------------------------------------------------
    // Every command here needs op2; op1 sees none of them.
    assert(dispatcher.usage("gamemode", op1).empty());
    assert(dispatcher.usage("weather", op1).empty());
    assert(dispatcher.usage("gamemode", owner) == "gamemode <mode>"); // owner still does

    // ---- forEachRootCommand: sorted, complete --------------------------------
    {
        std::vector<std::string> names;
        dispatcher.forEachRootCommand([&](std::string_view name) { names.emplace_back(name); });
        const std::vector<std::string> expected{"gamemode", "gamerule", "seed", "tp", "weather"};
        assert(names == expected); // sorted, not hash-iteration order
    }

    // ---- determinism: the /help listing is order-independent -----------------
    // A second dispatcher with commands registered in a different order produces
    // byte-identical help output, because the walk sorts.
    const auto listing = [](const CommandDispatcher& d, const CommandSource& source) {
        std::string out;
        d.forEachRootCommand([&](std::string_view name) {
            const std::string smart = d.usage(name, source);
            if (!smart.empty()) {
                out += "/" + smart + "\n";
            }
        });
        return out;
    };
    CommandDispatcher other;
    registerWeather(other);
    other.literal("seed").requiresLevel(PermissionLevel::GameMasters).executes(kOk);
    other.literal("tp").argument("pos", kTeleportDestinationArgument).executes(kOk);
    other.literal("gamerule")
        .requiresLevel(PermissionLevel::GameMasters)
        .argument("rule", kStringArgument)
        .executes(kOk)
        .argument("value", kStringArgument)
        .executes(kOk);
    other.literal("gamemode")
        .requiresLevel(PermissionLevel::GameMasters)
        .argument("mode", kGameModeArgument)
        .executes(kOk);
    assert(listing(dispatcher, owner) == listing(other, owner));
    assert(!listing(dispatcher, owner).empty());
    // op1 sees only the level-All command (tp), never the op2 ones.
    const std::string op1Listing = listing(dispatcher, op1);
    assert(op1Listing.find("/tp ") != std::string::npos);
    assert(op1Listing.find("gamemode") == std::string::npos);
    assert(op1Listing.find("weather") == std::string::npos);

    // ---- R2: an incomplete command reports its usage -------------------------
    const CommandResult incomplete = dispatcher.execute("/gamerule", owner);
    assert(!incomplete.success);
    assert(incomplete.message == "Usage: /gamerule <rule> [<value>]");

    return 0;
}

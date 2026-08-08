#include "gameplay/command/CommandDispatcher.hpp"
#include "gameplay/command/GameplayArguments.hpp"
#include "gameplay/entities/EntityRegistry.hpp"

#include <algorithm>
#include <cassert>
#include <memory>
#include <string>
#include <vector>

using mc::gameplay::command::CommandDispatcher;
using mc::gameplay::command::kEntityTargetArgument;
using mc::gameplay::command::kGameModeArgument;
using mc::gameplay::command::kGameRuleArgument;
using mc::gameplay::command::kGiveItemArgument;
using mc::gameplay::command::kIntArgument;
using mc::gameplay::command::kRotationArgument;
using mc::gameplay::command::kStringArgument;
using mc::gameplay::command::kTeleportDestinationArgument;
using mc::gameplay::command::kTimeArgument;
using mc::gameplay::command::Suggestion;
using mc::gameplay::CommandResult;

namespace {

bool hasSuggestion(const std::vector<Suggestion>& suggestions, std::string_view text) {
    return std::ranges::any_of(suggestions,
                               [&](const Suggestion& suggestion) { return suggestion.text == text; });
}

} // namespace

int main() {
    mc::gameplay::entities::registerBuiltinEntities();
    CommandDispatcher dispatcher;
    dispatcher.literal("gamemode")
        .argument("mode", kGameModeArgument)
        .executes([](const auto&) { return CommandResult{true, ""}; });
    dispatcher.literal("time")
        .then("set")
        .argument("time", kTimeArgument)
        .executes([](const auto&) { return CommandResult{true, ""}; });
    dispatcher.literal("give")
        .argument("item", kGiveItemArgument)
        .argument("count", kIntArgument)
        .executes([](const auto&) { return CommandResult{true, ""}; });
    dispatcher.literal("gamerule")
        .argument("rule", kGameRuleArgument)
        .executes([](const auto&) { return CommandResult{true, ""}; })
        .argument("value", kStringArgument)
        .executes([](const auto&) { return CommandResult{true, ""}; });
    dispatcher.literal("tp")
        .argument("destination", kTeleportDestinationArgument)
        .executes([](const auto&) { return CommandResult{true, ""}; })
        .argument("rotation", kRotationArgument)
        .executes([](const auto&) { return CommandResult{true, ""}; });
    dispatcher.literal("kill")
        .executes([](const auto&) { return CommandResult{true, ""}; })
        .argument("target", kEntityTargetArgument)
        .executes([](const auto&) { return CommandResult{true, ""}; });

    // "/" alone completes to every root command.
    auto suggestions = dispatcher.suggestions("/", 1);
    assert(hasSuggestion(suggestions, "gamemode"));
    assert(hasSuggestion(suggestions, "time"));
    assert(hasSuggestion(suggestions, "give"));
    assert(hasSuggestion(suggestions, "gamerule"));
    assert(hasSuggestion(suggestions, "tp"));
    assert(hasSuggestion(suggestions, "kill"));

    // Prefix filtering and the replace offset. `/ga` matches both commands that
    // share the prefix; `/gamem` narrows to gamemode alone.
    suggestions = dispatcher.suggestions("/ga", 3);
    assert(hasSuggestion(suggestions, "gamemode"));
    assert(hasSuggestion(suggestions, "gamerule"));
    suggestions = dispatcher.suggestions("/gamem", 6);
    assert(suggestions.size() == 1 && suggestions[0].text == "gamemode");
    assert(suggestions[0].start == 1); // replaces input[1..6) = "gamem"

    // Argument completion for a typed enum argument.
    suggestions = dispatcher.suggestions("/gamemode ", 10);
    assert(hasSuggestion(suggestions, "survival"));
    assert(hasSuggestion(suggestions, "creative"));
    suggestions = dispatcher.suggestions("/gamemode su", 12);
    assert(suggestions.size() == 1 && suggestions[0].text == "survival");

    // Time literals.
    suggestions = dispatcher.suggestions("/time set ", 10);
    assert(hasSuggestion(suggestions, "day"));
    assert(hasSuggestion(suggestions, "noon"));

    // The item table completes to the project's `rebedrock:` namespace, and
    // matches a bare path prefix so `/give ac` finds rebedrock:acacia_planks.
    suggestions = dispatcher.suggestions("/give ", 6);
    assert(!suggestions.empty());
    assert(hasSuggestion(suggestions, "rebedrock:acacia_planks"));
    suggestions = dispatcher.suggestions("/give rebedrock:ac", 18);
    assert(hasSuggestion(suggestions, "rebedrock:acacia_planks"));
    suggestions = dispatcher.suggestions("/give ac", 8);
    assert(hasSuggestion(suggestions, "rebedrock:acacia_planks"));
    // The numeric count argument offers a basic placeholder hint.
    suggestions = dispatcher.suggestions("/give rebedrock:acacia_planks ", 30);
    assert(hasSuggestion(suggestions, "1"));

    // Game rules complete from the GameRules table.
    suggestions = dispatcher.suggestions("/gamerule ", 10);
    assert(hasSuggestion(suggestions, "doDaylightCycle"));
    assert(hasSuggestion(suggestions, "randomTickSpeed"));
    suggestions = dispatcher.suggestions("/gamerule keep", 14);
    assert(suggestions.size() == 1 && suggestions[0].text == "keepInventory");

    // Entity-target commands complete from the registered species table.
    suggestions = dispatcher.suggestions("/tp ", 4);
    assert(hasSuggestion(suggestions, "rebedrock:pig"));
    assert(hasSuggestion(suggestions, "rebedrock:zombie"));
    suggestions = dispatcher.suggestions("/tp pi", 5);
    assert(suggestions.size() == 1 && suggestions[0].text == "rebedrock:pig");
    suggestions = dispatcher.suggestions("/kill ", 6);
    assert(hasSuggestion(suggestions, "player"));
    assert(hasSuggestion(suggestions, "rebedrock:pig"));
    suggestions = dispatcher.suggestions("/kill pla", 9);
    assert(suggestions.size() == 1 && suggestions[0].text == "player");

    // Node-level customSuggestions add candidates beyond any argument type
    // (Brigadier's customSuggestions), and exact matches sort before prefixes.
    dispatcher.literal("custom")
        .suggests([](mc::gameplay::command::SuggestionSink& sink) {
            sink.suggest("foo", "a custom suggestion");
            sink.suggest("foobar", "a longer one");
        })
        .executes([](const auto&) { return CommandResult{true, ""}; });
    suggestions = dispatcher.suggestions("/custom ", 8);
    assert(hasSuggestion(suggestions, "foo"));
    assert(hasSuggestion(suggestions, "foobar"));
    suggestions = dispatcher.suggestions("/custom foo", 11);
    assert(suggestions.size() == 2);
    assert(suggestions[0].text == "foo");    // exact match leads
    assert(suggestions[1].text == "foobar"); // then the longer prefix

    // Empty input and an already-invalid line produce no suggestions.
    assert(dispatcher.suggestions("", 0).empty());
    assert(dispatcher.suggestions("/nope ", 6).empty());
    return 0;
}

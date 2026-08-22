#include "gameplay/command/CommandSource.hpp"
#include "gameplay/command/EntitySelector.hpp"
#include "gameplay/command/GameplayArguments.hpp"
#include "gameplay/command/StringReader.hpp"
#include "gameplay/entities/EntityRegistry.hpp"

#include <algorithm>
#include <any>
#include <cassert>
#include <cstdint>
#include <glm/vec3.hpp>
#include <string>
#include <string_view>
#include <vector>

using namespace mc::gameplay::command;
using mc::gameplay::entities::EntityType;
using mc::gameplay::entities::entityTypeRegistry;

namespace {

[[nodiscard]] EntitySelector parseSel(std::string_view text) {
    StringReader reader{text};
    const ArgumentParseResult result = kEntitySelectorArgument.parse(reader);
    assert(result.ok());
    return std::any_cast<EntitySelector>(result.value);
}

[[nodiscard]] bool parseFails(std::string_view text) {
    StringReader reader{text};
    return !kEntitySelectorArgument.parse(reader).ok();
}

[[nodiscard]] bool suggested(const std::vector<Suggestion>& out, std::string_view text) {
    return std::any_of(out.begin(), out.end(),
                       [text](const Suggestion& s) { return s.text == text; });
}

} // namespace

int main() {
    mc::gameplay::entities::registerBuiltinEntities();
    const EntityType* pig = entityTypeRegistry().byId("pig");
    const EntityType* zombie = entityTypeRegistry().byId("zombie");
    assert(pig != nullptr && zombie != nullptr);

    // ---- parse: variables ----------------------------------------------------
    assert(parseSel("@s").variable == SelectorVariable::Self);
    assert(parseSel("@p").variable == SelectorVariable::NearestPlayer);
    assert(parseSel("@a").variable == SelectorVariable::AllPlayers);
    assert(parseSel("@e").variable == SelectorVariable::AllEntities);
    assert(parseSel("@r").variable == SelectorVariable::Random);

    // ---- parse: filters ------------------------------------------------------
    {
        const EntitySelector sel = parseSel("@e[type=pig,distance=..5,limit=2,sort=nearest]");
        assert(sel.typeId == "pig" && !sel.typeNegated);
        assert(!sel.distanceMinimum.has_value() && sel.distanceMaximum == 5.0);
        assert(sel.limit == std::size_t{2});
        assert(sel.sort == SelectorSort::Nearest);
    }
    assert(parseSel("@e[type=!pig]").typeNegated);
    assert(parseSel("@e[type=minecraft:pig]").typeId == "minecraft:pig"); // vanilla alias tolerated
    {
        const EntitySelector range = parseSel("@e[distance=2..5]");
        assert(range.distanceMinimum == 2.0 && range.distanceMaximum == 5.0);
        assert(parseSel("@e[distance=3..]").distanceMaximum.has_value() == false);
        assert(parseSel("@e[distance=3..]").distanceMinimum == 3.0);
        const EntitySelector exact = parseSel("@e[distance=3]");
        assert(exact.distanceMinimum == 3.0 && exact.distanceMaximum == 3.0);
    }
    assert(parseSel("@e[]").variable == SelectorVariable::AllEntities); // empty block

    // ---- parse: errors -------------------------------------------------------
    assert(parseFails("@x"));                 // unknown variable
    assert(parseFails("cow"));                // missing @
    assert(parseFails("@e[type=notreal]"));   // unregistered species
    assert(parseFails("@e[type=pig"));        // unterminated
    assert(parseFails("@e[name=bob]"));       // unsupported option (no backing system)
    assert(parseFails("@e[limit=0]"));        // non-positive limit
    assert(parseFails("@e[limit=-1]"));
    assert(parseFails("@e[sort=weird]"));     // unknown sort
    assert(parseFails("@e[distance=..]"));    // empty range

    // ---- resolve -------------------------------------------------------------
    CommandSource source;
    source.playerId = 1;
    source.position = {0.0F, 0.0F, 0.0F};
    // A player at 1, two pigs at 2 and 10, a zombie at 3 (all along +X).
    const std::vector<SelectorCandidate> candidates{
        {true, 1, 0, {1.0F, 0.0F, 0.0F}, nullptr},
        {false, 0, 100, {2.0F, 0.0F, 0.0F}, pig},
        {false, 0, 101, {10.0F, 0.0F, 0.0F}, pig},
        {false, 0, 102, {3.0F, 0.0F, 0.0F}, zombie},
    };

    // @s is the executor itself, regardless of the candidate list.
    {
        const auto targets = parseSel("@s").resolve(source, candidates, 0U);
        assert(targets.size() == 1U && targets[0].player && targets[0].playerId == 1U);
    }
    // @a / @p are players only; @p caps at the nearest one.
    assert(parseSel("@a").resolve(source, candidates, 0U).size() == 1U);
    {
        const auto nearest = parseSel("@p").resolve(source, candidates, 0U);
        assert(nearest.size() == 1U && nearest[0].player);
    }
    // @e is everything.
    assert(parseSel("@e").resolve(source, candidates, 0U).size() == 4U);
    // type= keeps only the species; negation keeps the rest (players included).
    {
        const auto pigs = parseSel("@e[type=pig]").resolve(source, candidates, 0U);
        assert(pigs.size() == 2U);
        assert(std::all_of(pigs.begin(), pigs.end(),
                           [](const SelectorTarget& t) { return t.entityId == 100 || t.entityId == 101; }));
        const auto notPigs = parseSel("@e[type=!pig]").resolve(source, candidates, 0U);
        assert(notPigs.size() == 2U); // the player and the zombie
    }
    // distance= is measured from the source.
    {
        const auto near = parseSel("@e[distance=..3]").resolve(source, candidates, 0U);
        assert(near.size() == 3U); // player(1), pig(2), zombie(3); the pig at 10 is out
    }
    // ...and it follows the source, not the world origin: a candidate far from the
    // origin but close to a moved source is in range (and vice-versa).
    {
        CommandSource moved;
        moved.position = {100.0F, 0.0F, 0.0F};
        const std::vector<SelectorCandidate> nearMoved{{false, 0, 200, {101.0F, 0.0F, 0.0F}, pig}};
        assert(parseSel("@e[distance=..3]").resolve(moved, nearMoved, 0U).size() == 1U);
        assert(parseSel("@e[distance=..3]").resolve(source, nearMoved, 0U).empty());
    }
    // limit + sort=nearest yields the single closest match.
    {
        const auto one = parseSel("@e[type=pig,limit=1,sort=nearest]").resolve(source, candidates, 0U);
        assert(one.size() == 1U && one[0].entityId == 100); // the pig at distance 2
    }

    // ---- @r determinism ------------------------------------------------------
    // Same seed and candidates → same pick (never the wall clock).
    {
        const auto a = parseSel("@r[type=pig]").resolve(source, candidates, 42U);
        const auto b = parseSel("@r[type=pig]").resolve(source, candidates, 42U);
        assert(a.size() == 1U && b.size() == 1U);
        assert(a[0].entityId == b[0].entityId);
        assert(a[0].entityId == 100 || a[0].entityId == 101); // it is one of the pigs
    }

    // ---- suggestions ---------------------------------------------------------
    {
        std::vector<Suggestion> out;
        SuggestionSink sink{out, 0U, "@"};
        kEntitySelectorArgument.collectSuggestions(sink);
        for (const char* v : {"@s", "@p", "@a", "@e", "@r"}) {
            assert(suggested(out, v));
        }
    }
    {
        std::vector<Suggestion> out;
        SuggestionSink sink{out, 0U, "@e["};
        kEntitySelectorArgument.collectSuggestions(sink);
        for (const char* k : {"@e[type=", "@e[distance=", "@e[limit=", "@e[sort="}) {
            assert(suggested(out, k));
        }
    }
    {
        // type= completes from the registry (bare path form), not a hardcoded list.
        std::vector<Suggestion> out;
        SuggestionSink sink{out, 0U, "@e[type="};
        kEntitySelectorArgument.collectSuggestions(sink);
        assert(suggested(out, "@e[type=pig"));
        assert(suggested(out, "@e[type=zombie"));
    }
    {
        std::vector<Suggestion> out;
        SuggestionSink sink{out, 0U, "@e[sort="};
        kEntitySelectorArgument.collectSuggestions(sink);
        for (const char* s : {"@e[sort=nearest", "@e[sort=furthest", "@e[sort=random"}) {
            assert(suggested(out, s));
        }
    }

    // ---- CMD5 support: @e order is deterministic (no randomness) -------------
    // execute-as walks @e in a fixed order; two different seeds must resolve the
    // same sequence, so a chain over @e is reproducible.
    {
        const auto orderA = parseSel("@e").resolve(source, candidates, 1U);
        const auto orderB = parseSel("@e").resolve(source, candidates, 999U);
        assert(orderA.size() == orderB.size());
        for (std::size_t index = 0; index < orderA.size(); ++index) {
            assert(orderA[index].player == orderB[index].player &&
                   orderA[index].entityId == orderB[index].entityId &&
                   orderA[index].playerId == orderB[index].playerId);
        }
    }

    // ---- CMD5 support: @s resolves the entity executor (execute as <entity>) --
    {
        CommandSource entitySource;
        entitySource.executorIsEntity = true;
        entitySource.entityId = 101; // the pig at (10,0,0) in the candidate set
        const auto self = parseSel("@s").resolve(entitySource, candidates, 0U);
        assert(self.size() == 1U && !self[0].player && self[0].entityId == 101);
        entitySource.entityId = 9999; // a since-removed entity yields nothing
        assert(parseSel("@s").resolve(entitySource, candidates, 0U).empty());
    }

    // ---- CMD5 support: the withXxx source copies change one field -------------
    {
        CommandSource base;
        base.playerId = 1;
        base.position = {1.0F, 2.0F, 3.0F};
        assert(base.withExecutorEntity(7).executorIsEntity &&
               base.withExecutorEntity(7).entityId == 7U);
        assert(!base.withExecutorEntity(7).withExecutorPlayer(2).executorIsEntity);
        assert(base.withExecutorPlayer(2).playerId == 2U);
        const CommandSource moved = base.withPosition({4.0F, 5.0F, 6.0F});
        assert(moved.position == glm::vec3(4.0F, 5.0F, 6.0F));
        assert(moved.playerId == 1U); // other fields preserved by the copy
    }

    return 0;
}

#include "gameplay/GameRules.hpp"

#include <cassert>
#include <string>

int main() {
    using namespace mc::gameplay;

    // Defaults come from the registry table.
    GameRules rules;
    assert(rules.get<bool>(GameRuleId::DoDaylightCycle));
    assert(!rules.get<bool>(GameRuleId::KeepInventory));
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 3);

    // Command parsing dispatches by the rule's registered type.
    auto result = rules.setFromCommand("randomTickSpeed", "7");
    assert(result.success);
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 7);
    // Values past the safety ceiling clamp instead of rejecting, so a typo like
    // /gamerule randomTickSpeed 100000 cannot stall the render thread.
    result = rules.setFromCommand("randomTickSpeed", "100000");
    assert(result.success);
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 1000);
    // Below the floor is rejected outright.
    result = rules.setFromCommand("randomTickSpeed", "-1");
    assert(!result.success);
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 1000);
    result = rules.setFromCommand("randomTickSpeed", "fast");
    assert(!result.success);
    result = rules.setFromCommand("randomTickSpeed", "");
    assert(!result.success);

    // Boolean rules accept true/false case-insensitively and reject anything else.
    result = rules.setFromCommand("doDaylightCycle", "FALSE");
    assert(result.success);
    assert(!rules.get<bool>(GameRuleId::DoDaylightCycle));
    result = rules.setFromCommand("doDaylightCycle", "true");
    assert(result.success);
    assert(rules.get<bool>(GameRuleId::DoDaylightCycle));
    result = rules.setFromCommand("doDaylightCycle", "yes");
    assert(!result.success);

    // The vanilla alias and any case resolve to the same rule.
    result = rules.setFromCommand("minecraft:randomTickSpeed", "5");
    assert(result.success);
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 5);
    result = rules.setFromCommand("RANDOMTICKSPEED", "6");
    assert(result.success);
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 6);

    // Unknown rules report an error instead of touching anything.
    result = rules.setFromCommand("notARule", "1");
    assert(!result.success);
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 6);

    // `/gamerule <rule>` without a value queries the current value.
    result = rules.query("randomTickSpeed");
    assert(result.success);
    assert(result.message == "randomTickSpeed: 6");
    result = rules.query("keepInventory");
    assert(result.success);
    assert(result.message == "keepInventory: false");
    result = rules.query("notARule");
    assert(!result.success);

    // The change handler fires whenever a rule changes.
    int notifiedId = -1;
    GameRuleValueData notifiedValue{false};
    rules.setChangeHandler([&](GameRuleId id, const GameRuleValueData& value) {
        notifiedId = static_cast<int>(id);
        notifiedValue = value;
    });
    assert(rules.set<std::int32_t>(GameRuleId::RandomTickSpeed, 9));
    assert(notifiedId == static_cast<int>(GameRuleId::RandomTickSpeed));
    assert(std::get<std::int32_t>(notifiedValue) == 9);
    // Replacing the handler swaps which rules are mirrored.
    bool fired = false;
    rules.setChangeHandler([&](GameRuleId, const GameRuleValueData&) { fired = true; });
    static_cast<void>(rules.set<bool>(GameRuleId::KeepInventory, true));
    assert(fired);

    // applyDecoded drives load: unknown names and type mismatches are skipped
    // silently, keeping the current value, so a newer save never breaks an
    // older build. It never fires the change handler — that is left to the
    // renderer, which applies the whole rule set after load.
    fired = false;
    assert(!rules.applyDecoded("notARule", GameRuleType::Boolean, true));
    assert(!rules.applyDecoded("keepInventory", GameRuleType::Int, std::int32_t{1}));
    assert(rules.applyDecoded("keepInventory", GameRuleType::Boolean, true));
    assert(rules.get<bool>(GameRuleId::KeepInventory));
    // Out-of-range integers are skipped too.
    assert(!rules.applyDecoded("randomTickSpeed", GameRuleType::Int, std::int32_t{-5}));
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 9);
    assert(rules.applyDecoded("randomTickSpeed", GameRuleType::Int, std::int32_t{2}));
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 2);
    // applyDecoded never fires the change handler.
    assert(!fired);

    return 0;
}

#include "gameplay/GameRules.hpp"

#include <cassert>
#include <string>

int main() {
    using namespace mc::gameplay;

    // Defaults come from the registry table.
    GameRules rules;
    assert(rules.get<bool>(GameRuleId::AdvanceTime));
    assert(!rules.get<bool>(GameRuleId::KeepInventory));
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 3);

    // Command parsing dispatches by the rule's registered type.
    auto result = rules.setFromCommand("random_tick_speed", "7");
    assert(result.success);
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 7);
    // Values past the safety ceiling clamp instead of rejecting, so a typo like
    // /gamerule random_tick_speed 100000 cannot stall the render thread.
    result = rules.setFromCommand("random_tick_speed", "100000");
    assert(result.success);
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 1000);
    // Below the floor is rejected outright.
    result = rules.setFromCommand("random_tick_speed", "-1");
    assert(!result.success);
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 1000);
    result = rules.setFromCommand("random_tick_speed", "fast");
    assert(!result.success);
    result = rules.setFromCommand("random_tick_speed", "");
    assert(!result.success);

    // Boolean rules accept true/false case-insensitively and reject anything else.
    result = rules.setFromCommand("advance_time", "FALSE");
    assert(result.success);
    assert(!rules.get<bool>(GameRuleId::AdvanceTime));
    result = rules.setFromCommand("advance_time", "true");
    assert(result.success);
    assert(rules.get<bool>(GameRuleId::AdvanceTime));
    result = rules.setFromCommand("advance_time", "yes");
    assert(!result.success);

    // The vanilla alias and any case resolve to the same rule.
    result = rules.setFromCommand("minecraft:random_tick_speed", "5");
    assert(result.success);
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 5);
    result = rules.setFromCommand("RANDOM_TICK_SPEED", "6");
    assert(result.success);
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 6);

    // Unknown rules report an error instead of touching anything.
    result = rules.setFromCommand("notARule", "1");
    assert(!result.success);
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 6);

    // `/gamerule <rule>` without a value queries the current value.
    result = rules.query("random_tick_speed");
    assert(result.success);
    assert(result.message == "random_tick_speed: 6");
    result = rules.query("keep_inventory");
    assert(result.success);
    assert(result.message == "keep_inventory: false");
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
    assert(!rules.applyDecoded("keep_inventory", GameRuleType::Int, std::int32_t{1}));
    assert(rules.applyDecoded("keep_inventory", GameRuleType::Boolean, true));
    assert(rules.get<bool>(GameRuleId::KeepInventory));
    // Out-of-range integers are skipped too.
    assert(!rules.applyDecoded("random_tick_speed", GameRuleType::Int, std::int32_t{-5}));
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 9);
    assert(rules.applyDecoded("random_tick_speed", GameRuleType::Int, std::int32_t{2}));
    assert(rules.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 2);
    // applyDecoded never fires the change handler.
    assert(!fired);

    // --- 26.1 naming ---------------------------------------------------------
    // Every registered name is the snake_case 26.1 id, and the old camelCase
    // spellings are gone from the command surface entirely: the rename is not
    // "both names work", it is "the new name is the name".
    for (const auto& definition : kGameRuleDefinitions) {
        for (const char character : definition.name) {
            assert(!(character >= 'A' && character <= 'Z'));
        }
    }
    assert(gameRuleIdFromName("doDaylightCycle") == GameRuleId::Count);
    assert(gameRuleIdFromName("randomTickSpeed") == GameRuleId::Count);
    assert(!rules.setFromCommand("doDaylightCycle", "false").success);
    assert(!rules.query("keepInventory").success);

    // --- Legacy save names ---------------------------------------------------
    // The save path, and only the save path, still resolves the pre-rename
    // names, so a world written by an older build keeps its non-default rules
    // instead of silently reverting. This is the whole reason the alias table
    // exists (vanilla does the same job in GameRuleRegistryFix).
    {
        GameRules loaded;
        assert(loaded.get<bool>(GameRuleId::AdvanceTime));
        assert(loaded.applyDecoded("doDaylightCycle", GameRuleType::Boolean, false));
        assert(!loaded.get<bool>(GameRuleId::AdvanceTime));
        assert(loaded.applyDecoded("doWeatherCycle", GameRuleType::Boolean, false));
        assert(!loaded.get<bool>(GameRuleId::AdvanceWeather));
        assert(loaded.applyDecoded("keepInventory", GameRuleType::Boolean, true));
        assert(loaded.get<bool>(GameRuleId::KeepInventory));
        assert(loaded.applyDecoded("sendCommandFeedback", GameRuleType::Boolean, false));
        assert(!loaded.get<bool>(GameRuleId::SendCommandFeedback));
        assert(loaded.applyDecoded("randomTickSpeed", GameRuleType::Int, std::int32_t{7}));
        assert(loaded.get<std::int32_t>(GameRuleId::RandomTickSpeed) == 7);
        // A legacy name is matched case-insensitively like any other, still
        // type-checked, and a name that was never one of ours stays unknown.
        assert(loaded.applyDecoded("KEEPINVENTORY", GameRuleType::Boolean, false));
        assert(!loaded.get<bool>(GameRuleId::KeepInventory));
        assert(!loaded.applyDecoded("keepInventory", GameRuleType::Int, std::int32_t{1}));
        assert(!loaded.applyDecoded("doFireTick", GameRuleType::Boolean, false));
        // Every alias points at a rule that still exists.
        for (const auto& alias : kLegacyGameRuleAliases) {
            assert(alias.id != GameRuleId::Count);
            assert(gameRuleIdFromName(alias.legacyName) == GameRuleId::Count);
        }
    }

    // --- The rules added alongside the rename --------------------------------
    {
        GameRules fresh;
        // Defaults are vanilla's, so an unconfigured world behaves as before.
        assert(fresh.get<bool>(GameRuleId::FallDamage));
        assert(fresh.get<bool>(GameRuleId::FireDamage));
        assert(fresh.get<bool>(GameRuleId::DrowningDamage));
        assert(fresh.get<bool>(GameRuleId::NaturalHealthRegeneration));
        assert(fresh.get<bool>(GameRuleId::BlockDrops));
        assert(fresh.get<bool>(GameRuleId::MobDrops));
        assert(fresh.get<bool>(GameRuleId::SpawnMobs));
        assert(fresh.get<std::int32_t>(GameRuleId::FireSpreadRadiusAroundPlayer) == 128);
        assert(fresh.get<std::int32_t>(GameRuleId::MaxBlockModifications) == 32768);
        assert(fresh.get<std::int32_t>(GameRuleId::MaxCommandForks) == 65536);
        assert(fresh.get<std::int32_t>(GameRuleId::MaxCommandSequenceLength) == 65536);
        // fire_spread_radius_around_player is the one rule whose floor is
        // negative: -1 means "anywhere", 0 means "nowhere".
        assert(fresh.setFromCommand("fire_spread_radius_around_player", "-1").success);
        assert(fresh.get<std::int32_t>(GameRuleId::FireSpreadRadiusAroundPlayer) == -1);
        assert(!fresh.setFromCommand("fire_spread_radius_around_player", "-2").success);
        assert(fresh.setFromCommand("fire_spread_radius_around_player", "0").success);
        // max_block_modifications floors at 1: a zero budget would make /fill
        // unusable rather than merely bounded.
        assert(!fresh.setFromCommand("max_block_modifications", "0").success);
        assert(fresh.setFromCommand("max_block_modifications", "1").success);
        // The `minecraft:` prefix resolves for the new names too.
        assert(fresh.setFromCommand("minecraft:mob_drops", "false").success);
        assert(!fresh.get<bool>(GameRuleId::MobDrops));
    }

    return 0;
}

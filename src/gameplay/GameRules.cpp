#include "gameplay/GameRules.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>

namespace mc::gameplay {
namespace {

[[nodiscard]] std::string lowercase(std::string_view text) {
    std::string result{text};
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

// The command accepts the bare name and the `minecraft:` alias; matching is
// case-insensitive (see ruleNameEqual), the way vanilla's GameRuleCommand
// resolves namespaced rule names.
[[nodiscard]] std::string normalizedRuleName(std::string_view name) {
    std::string normalized{name};
    constexpr std::string_view kVanillaNamespace = "minecraft:";
    if (normalized.starts_with(kVanillaNamespace)) {
        normalized.erase(0, kVanillaNamespace.size());
    }
    return normalized;
}

} // namespace

GameRules::GameRules() {
    for (std::size_t index = 0; index < kGameRuleDefinitions.size(); ++index) {
        values_[index] = kGameRuleDefinitions[index].defaultValue;
    }
}

CommandResult GameRules::setFromCommand(std::string_view name, std::string_view valueText) {
    const auto id = gameRuleIdFromName(normalizedRuleName(name));
    if (id == GameRuleId::Count) {
        return {false, "Unknown game rule: " + std::string{name}};
    }
    const auto& definition = kGameRuleDefinitions[static_cast<std::size_t>(id)];
    if (definition.type == GameRuleType::Boolean) {
        const std::string normalized = lowercase(valueText);
        bool parsed = false;
        if (normalized == "true") {
            parsed = true;
        } else if (normalized != "false") {
            return {false, "Invalid boolean value: " + std::string{valueText}};
        }
        static_cast<void>(set<bool>(id, parsed));
        return {true, "Set " + std::string{definition.name} + " to " + normalized};
    }
    std::int32_t parsed = 0;
    const auto [end, error] = std::from_chars(
        valueText.data(), valueText.data() + valueText.size(), parsed);
    if (error != std::errc{} || end != valueText.data() + valueText.size()) {
        return {false, "Invalid integer value: " + std::string{valueText}};
    }
    if (!set<std::int32_t>(id, parsed)) {
        return {false, std::string{definition.name} + " must be at least " +
                           std::to_string(definition.minimum)};
    }
    // `set` clamps values above the ceiling, so report what was actually stored.
    return {true, "Set " + std::string{definition.name} + " to " +
                      std::to_string(get<std::int32_t>(id))};
}

CommandResult GameRules::query(std::string_view name) const {
    const auto id = gameRuleIdFromName(normalizedRuleName(name));
    if (id == GameRuleId::Count) {
        return {false, "Unknown game rule: " + std::string{name}};
    }
    const auto& definition = kGameRuleDefinitions[static_cast<std::size_t>(id)];
    std::string text;
    if (definition.type == GameRuleType::Boolean) {
        text = get<bool>(id) ? "true" : "false";
    } else {
        text = std::to_string(get<std::int32_t>(id));
    }
    return {true, std::string{definition.name} + ": " + text};
}

bool GameRules::applyDecoded(std::string_view name, GameRuleType type,
                             const GameRuleValueData& decoded) {
    auto id = gameRuleIdFromName(name);
    if (id == GameRuleId::Count) {
        // A world written before this build adopted 26.1's snake_case registry
        // keyed its entries by the old camelCase name. Resolving those here —
        // and only here, never on the command surface — is what keeps a
        // pre-rename save's non-default rules from silently reverting to their
        // defaults on load. Vanilla does the same job in GameRuleRegistryFix.
        id = legacyGameRuleIdFromName(name);
    }
    if (id == GameRuleId::Count) {
        return false;
    }
    const auto& definition = kGameRuleDefinitions[static_cast<std::size_t>(id)];
    if (definition.type != type) {
        return false;
    }
    if (type == GameRuleType::Boolean && !std::holds_alternative<bool>(decoded)) {
        return false;
    }
    if (type == GameRuleType::Int) {
        if (!std::holds_alternative<std::int32_t>(decoded)) {
            return false;
        }
        // Out-of-range values from a newer writer are skipped, keeping the
        // default instead of trusting a value this build cannot honour.
        const auto value = std::get<std::int32_t>(decoded);
        if (value < definition.minimum || value > definition.maximum) {
            return false;
        }
    }
    values_[static_cast<std::size_t>(id)] = decoded;
    return true;
}

} // namespace mc::gameplay

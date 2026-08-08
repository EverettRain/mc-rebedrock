#pragma once

#include <optional>
#include <string_view>

namespace mc::gameplay {

enum class GameMode {
    Survival,
    Creative,
};

[[nodiscard]] constexpr std::string_view gameModeName(GameMode mode) {
    switch (mode) {
    case GameMode::Survival:
        return "survival";
    case GameMode::Creative:
        return "creative";
    }
    return "survival";
}

[[nodiscard]] constexpr std::optional<GameMode> parseGameMode(std::string_view name) {
    if (name == "survival" || name == "s" || name == "0") {
        return GameMode::Survival;
    }
    if (name == "creative" || name == "c" || name == "1") {
        return GameMode::Creative;
    }
    return std::nullopt;
}

} // namespace mc::gameplay

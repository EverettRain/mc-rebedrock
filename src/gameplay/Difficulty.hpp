#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace mc::gameplay {

// Java's net.minecraft.world.Difficulty. It drives the survival damage model,
// peaceful hostile removal and the multiplier that difficulty-scaled damage
// sources carry.
enum class Difficulty : std::uint8_t {
    Peaceful,
    Easy,
    Normal,
    Hard,
};

inline constexpr std::uint8_t kDifficultyCount = 4U;

// The vanilla translation keys, so the options button reads from the 1.16.1
// language files like every other label.
[[nodiscard]] constexpr std::string_view difficultyTranslationKey(Difficulty difficulty) {
    switch (difficulty) {
    case Difficulty::Peaceful: return "options.difficulty.peaceful";
    case Difficulty::Easy: return "options.difficulty.easy";
    case Difficulty::Normal: return "options.difficulty.normal";
    case Difficulty::Hard: return "options.difficulty.hard";
    }
    return "options.difficulty.normal";
}

[[nodiscard]] constexpr std::string_view difficultyName(Difficulty difficulty) {
    switch (difficulty) {
    case Difficulty::Peaceful: return "peaceful";
    case Difficulty::Easy: return "easy";
    case Difficulty::Normal: return "normal";
    case Difficulty::Hard: return "hard";
    }
    return "normal";
}

[[nodiscard]] constexpr std::optional<Difficulty> difficultyFromName(std::string_view name) {
    for (std::uint8_t index = 0; index < kDifficultyCount; ++index) {
        const auto difficulty = static_cast<Difficulty>(index);
        if (difficultyName(difficulty) == name) return difficulty;
    }
    return std::nullopt;
}

// The Difficulty button cycles Peaceful -> Easy -> Normal -> Hard -> Peaceful.
[[nodiscard]] constexpr Difficulty nextDifficulty(Difficulty difficulty) {
    return static_cast<Difficulty>(
        (static_cast<std::uint8_t>(difficulty) + 1U) % kDifficultyCount);
}

// FoodData#tick: how much health starvation is allowed to leave standing.
// Easy stops at five hearts, normal at half a heart, hard starves the player to
// death, and peaceful never lets hunger run out in the first place.
[[nodiscard]] constexpr float starvationHealthFloor(Difficulty difficulty) {
    switch (difficulty) {
    case Difficulty::Peaceful: return 20.0F;
    case Difficulty::Easy: return 10.0F;
    case Difficulty::Normal: return 1.0F;
    case Difficulty::Hard: return 0.0F;
    }
    return 1.0F;
}

// PlayerEntity#tick: peaceful worlds hand back a health point every second and
// a food point every half second, which is why hunger never bites there.
[[nodiscard]] constexpr bool regeneratesFreely(Difficulty difficulty) {
    return difficulty == Difficulty::Peaceful;
}

// LivingEntity#applyDamage, for the sources DamageType#isScaledWithDifficulty
// marks. Hostile mob melee attacks enter the player pipeline through this rule.
[[nodiscard]] constexpr float scaledDamage(Difficulty difficulty, float amount) {
    switch (difficulty) {
    case Difficulty::Peaceful: return 0.0F;
    case Difficulty::Easy: return amount < 2.0F ? amount : amount * 0.5F + 1.0F;
    case Difficulty::Normal: return amount;
    case Difficulty::Hard: return amount * 1.5F;
    }
    return amount;
}

// HuskEntity#tryAttack (AR-M2): `140 * (int) getLocalDifficulty()`, where
// getLocalDifficulty is ServerWorld's regional-difficulty float — a
// world-age/inhabited-time/moon-phase blend this codebase has no equivalent
// of. Approximated here off the world Difficulty setting alone, the same
// simplification scaledDamage above makes for the identical source value:
// Easy/Normal/Hard land on 140/280/420 ticks (7/14/21 seconds), the
// {1,2,3} multiplier getLocalDifficulty averages to across a world's
// lifetime at each setting. Peaceful returns 0 (a husk cannot exist there —
// MobCategoryTraits removes every MONSTER instantly), matching scaledDamage's
// own Peaceful floor.
[[nodiscard]] constexpr std::int32_t huskHungerDurationTicks(Difficulty difficulty) {
    switch (difficulty) {
    case Difficulty::Peaceful: return 0;
    case Difficulty::Easy: return 140;
    case Difficulty::Normal: return 280;
    case Difficulty::Hard: return 420;
    }
    return 280;
}

} // namespace mc::gameplay

#pragma once

// Java 26.1 Player: experienceLevel/experienceProgress/totalExperience/
// enchantmentSeed, the currency state the future enchanting/anvil/mending
// systems will spend. XP-0 only builds the state machine, the levelling
// formula and the wiring into the snapshot/HUD/save paths — no orbs, no
// sources, no commands, no sinks (those are XP-1..4).
//
// Design (XP-DESIGN.md / XP-experience/REGULAR.md):
//   - Storage is integer truth: `level_` and `total_` are the state of
//     record. `progress()` is *derived* every call from "points already
//     earned into the current level" divided by "points the current level
//     needs" (xpToNextLevel(level_)) — never accumulated as its own float,
//     so it cannot drift the way repeatedly adding/subtracting a running
//     float would.
//   - xpToNextLevel/totalForLevel are constexpr pure functions (no state,
//     foldable at compile time), matching Player#getXpNeededForNextLevel's
//     three-segment curve exactly (26.1 source, not the wiki approximation).
//   - enchantmentSeed is reroll-on-demand from a caller-supplied JavaRandom
//     draw (never the wall clock / global RNG), so replay is deterministic
//     the way REGULAR.md's determinism rule requires.
#include "world/gen/JavaRandom.hpp"

#include <algorithm>
#include <cstdint>

namespace mc::gameplay {

// Player#getXpNeededForNextLevel, 26.1 source (Player.java:1542-1548):
//   level >= 30: 112 + (level - 30) * 9
//   level >= 15: 37 + (level - 15) * 5
//   else:        7 + level * 2
// (Algebraically identical to the "9L-158 / 5L-38 / 2L+7" form some notes
// quote with boundaries at 31/16 instead of 30/15 — both forms agree at
// every integer level; this file follows the actual 26.1 source layout.)
[[nodiscard]] constexpr std::int32_t xpToNextLevel(std::int32_t level) {
    const std::int32_t clamped = std::max(level, 0);
    if (clamped >= 30) {
        return 112 + (clamped - 30) * 9;
    }
    if (clamped >= 15) {
        return 37 + (clamped - 15) * 5;
    }
    return 7 + clamped * 2;
}

// The cumulative total experience points required to *reach* `level` from
// zero (i.e. the sum of xpToNextLevel(0..level-1)). Used to convert a raw
// point total into a level/progress pair (setExperiencePoints) and to report
// "points needed to reach level N" for XP-3's future /xp query. Not part of
// vanilla's own API (26.1 walks level-by-level instead), but it is the
// closed-form of exactly that walk, so it agrees with it at every level.
[[nodiscard]] constexpr std::int32_t totalForLevel(std::int32_t level) {
    // The two knot totals (points needed to reach level 15 and level 30 from
    // zero), each the closed form of Sum_{L=0}^{knot-1} xpToNextLevel(L):
    //   totalForLevel(15) = Sum_{L=0}^{14} (7+2L) = 7*15 + 15*14        = 315
    //   totalForLevel(30) = totalForLevel(15) + Sum_{L=15}^{29} (37+5(L-15))
    //                      = 315 + 37*15 + 5*(0+1+...+14) = 315+555+525 = 1395
    constexpr std::int64_t kBaseAtLevel15 = 315;
    constexpr std::int64_t kBaseAtLevel30 = 1395;
    const std::int32_t clamped = std::max(level, 0);
    std::int64_t total = 0;
    if (clamped >= 30) {
        // Sum of levels 30..(clamped-1) at 112+9*(L-30), collapsed to closed
        // form so this stays O(1) instead of walking every level: XP-3's
        // future /xp query and a death-drop clamp both call this per
        // command/tick, and a level-200 lookup would otherwise loop 200
        // times on the hot path.
        const std::int64_t above30 = static_cast<std::int64_t>(clamped) - 30;
        total = kBaseAtLevel30 + above30 * 112 + (above30 * (above30 - 1) / 2) * 9;
    } else if (clamped >= 15) {
        const std::int64_t above15 = static_cast<std::int64_t>(clamped) - 15;
        total = kBaseAtLevel15 + above15 * 37 + (above15 * (above15 - 1) / 2) * 5;
    } else {
        // Sum_{L=0}^{clamped-1} (7 + 2L) = 7*clamped + clamped*(clamped-1).
        total = static_cast<std::int64_t>(7) * clamped +
                static_cast<std::int64_t>(clamped) * (clamped - 1);
    }
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(total, 0, INT32_MAX));
}

// The player's experience currency: level, the points already earned toward
// the next level (stored, not the derived fraction), the running lifetime
// total (vanilla's score-adjacent totalExperience, clamped to
// [0, INT32_MAX]) and the enchantment preview seed. Mirrors 26.1's
// Player.experienceLevel/experienceProgress/totalExperience/enchantmentSeed
// (JC: XpLevel/XpP/XpTotal/XpSeed — near-identical mapping).
class PlayerExperience final {
  public:
    // Player#giveExperiencePoints: adds raw points, walking levels up (or, for
    // a negative amount, down) one at a time exactly like vanilla's while
    // loops, so a giant single addExperience call still lands on the same
    // level/leftover-points pair vanilla's iterative version would.
    void addExperience(std::int32_t points) {
        if (points == 0) {
            return;
        }
        // Player#giveExperiencePoints: `Mth.clamp(totalExperience + i, 0,
        // MAX_VALUE)` runs unconditionally, for both a gain and a cost (a
        // future onEnchantmentPerformed/consumeLevels-style negative call), so
        // total_ tracks this call's delta the same way regardless of sign.
        total_ = clampNonNegative(static_cast<std::int64_t>(total_) + points);
        std::int64_t pointsIntoLevel = static_cast<std::int64_t>(pointsIntoLevel_) + points;
        std::int64_t level = level_;
        bool hitFloor = false;
        while (pointsIntoLevel < 0) {
            if (level <= 0) {
                level = 0;
                pointsIntoLevel = 0;
                hitFloor = true;
                break;
            }
            --level;
            pointsIntoLevel += xpToNextLevel(static_cast<std::int32_t>(level));
        }
        while (level < INT32_MAX) {
            const std::int32_t needed = xpToNextLevel(static_cast<std::int32_t>(level));
            if (needed <= 0 || pointsIntoLevel < needed) {
                break;
            }
            pointsIntoLevel -= needed;
            ++level;
        }
        level_ = static_cast<std::int32_t>(std::clamp<std::int64_t>(level, 0, INT32_MAX));
        pointsIntoLevel_ = static_cast<std::int32_t>(pointsIntoLevel);
        if (hitFloor) {
            // Ran out of levels to borrow from at the floor: vanilla zeroes
            // progress and total together (Player.java:1499-1501/1519-1521,
            // reached through giveExperienceLevels(-1)'s own level<0 clamp).
            total_ = 0;
        }
    }

    // Player#giveExperienceLevels: adds whole levels (saturating add, negative
    // clamps to zero and wipes progress/total, matching 26.1 exactly).
    void giveExperienceLevels(std::int32_t amount) {
        const std::int64_t sum = static_cast<std::int64_t>(level_) + amount;
        if (sum < 0) {
            level_ = 0;
            pointsIntoLevel_ = 0;
            total_ = 0;
            return;
        }
        level_ = static_cast<std::int32_t>(std::clamp<std::int64_t>(sum, 0, INT32_MAX));
    }

    // /xp set <level> levels (XP-3's future entry point): pins the level
    // directly and zeroes the in-level progress, the way the command sets it
    // rather than walking there via addExperience.
    void setExperienceLevel(std::int32_t level) {
        level_ = std::max(level, 0);
        pointsIntoLevel_ = 0;
    }

    // /xp set <amount> points: re-derives level/progress from a raw lifetime
    // point total using the closed-form totalForLevel, an O(log) binary
    // search rather than a walk from zero (a set to a huge total must not
    // become an O(level) loop on the command-dispatch hot path).
    void setExperiencePoints(std::int32_t total) {
        const std::int32_t clampedTotal = std::max(total, 0);
        std::int32_t low = 0;
        std::int32_t high = 1;
        while (totalForLevel(high) <= clampedTotal && high < (INT32_MAX / 2)) {
            high *= 2;
        }
        while (low < high) {
            const std::int32_t mid = low + (high - low + 1) / 2;
            if (totalForLevel(mid) <= clampedTotal) {
                low = mid;
            } else {
                high = mid - 1;
            }
        }
        level_ = low;
        pointsIntoLevel_ = clampedTotal - totalForLevel(low);
        total_ = clampedTotal;
    }

    // Enchanting/anvil consume levels atomically: either the whole cost lands
    // or nothing changes (XP-4's future consume interface; built now so that
    // node does not have to return here).
    [[nodiscard]] bool canAfford(std::int32_t levels) const { return level_ >= levels; }

    [[nodiscard]] bool consumeLevels(std::int32_t levels) {
        if (levels <= 0 || !canAfford(levels)) {
            return false;
        }
        level_ -= levels;
        if (level_ == 0) {
            pointsIntoLevel_ = 0;
        }
        return true;
    }

    // Player#onEnchantmentPerformed / the constructor's `if (seed == 0)`
    // path: reroll from a caller-supplied JavaRandom draw. The caller owns
    // the RNG stream (per-player, seeded off the world seed) so this stays
    // fully deterministic and replay-stable — no wall clock, no
    // std::random_device, ever.
    void rerollEnchantmentSeed(world::gen::JavaRandom& rng) { seed_ = rng.nextInt(); }

    [[nodiscard]] std::int32_t level() const { return level_; }
    [[nodiscard]] std::int32_t totalExperience() const { return total_; }
    [[nodiscard]] std::int32_t enchantmentSeed() const { return seed_; }

    // The stored truth is `pointsIntoLevel_`; this is the derived fraction
    // the HUD bar and the network snapshot want. A level with no XP
    // requirement (should not happen for a non-negative level, but a future
    // level cap change could hit it) reports full so the bar does not divide
    // by zero.
    [[nodiscard]] float progress() const {
        const std::int32_t needed = xpToNextLevel(level_);
        if (needed <= 0) {
            return 1.0F;
        }
        return std::clamp(static_cast<float>(pointsIntoLevel_) / static_cast<float>(needed), 0.0F,
                          1.0F);
    }

    // Restores the four persisted fields verbatim (save load / snapshot
    // decode), bypassing the level-walking arithmetic above: the values were
    // already consistent when written, so there is nothing to recompute.
    void restore(std::int32_t level, std::int32_t pointsIntoLevel, std::int32_t total,
                std::int32_t seed) {
        level_ = std::max(level, 0);
        pointsIntoLevel_ = std::max(pointsIntoLevel, 0);
        total_ = std::max(total, 0);
        seed_ = seed;
    }

    [[nodiscard]] std::int32_t pointsIntoLevel() const { return pointsIntoLevel_; }

    // A respawn does not clear experience in vanilla survival (unlike health
    // and food); XP-4's future death-drop path is the only thing that zeroes
    // it. No reset() here on purpose — PlayerVitals::reset() must not touch
    // this component.

  private:
    [[nodiscard]] static std::int32_t clampNonNegative(std::int64_t value) {
        return static_cast<std::int32_t>(std::clamp<std::int64_t>(value, 0, INT32_MAX));
    }

    std::int32_t level_ = 0;
    // Points already earned into the current level (integer truth; progress()
    // derives the fraction from this + xpToNextLevel(level_)).
    std::int32_t pointsIntoLevel_ = 0;
    std::int32_t total_ = 0;
    std::int32_t seed_ = 0;
};

} // namespace mc::gameplay

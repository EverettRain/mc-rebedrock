// XP-0: the player experience currency state (level/points-into-level/total/
// enchantmentSeed), the constexpr levelling formula and the add/give/set/
// consume API. No orbs, no sources, no commands — those are XP-1..4; this
// only covers the state machine XP-0 owns.

#include "gameplay/PlayerExperience.hpp"
#include "world/gen/JavaRandom.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace mc;

namespace {

// --- xpToNextLevel / totalForLevel: hardcoded 26.1 values (Player.java:1542,
// getXpNeededForNextLevel) at the segment boundaries and a couple of interior
// points, so a swapped `>=` or a flipped boundary constant fails loudly. ---
void testFormulaBoundaries() {
    using gameplay::xpToNextLevel;
    assert(xpToNextLevel(0) == 7);
    assert(xpToNextLevel(1) == 9);
    assert(xpToNextLevel(14) == 35);
    assert(xpToNextLevel(15) == 37);   // low/mid boundary
    assert(xpToNextLevel(16) == 42);   // one past the boundary
    assert(xpToNextLevel(29) == 107);
    assert(xpToNextLevel(30) == 112);  // mid/high boundary
    assert(xpToNextLevel(31) == 121);  // one past the boundary
    assert(xpToNextLevel(40) == 202);
    // A negative level (should never occur, but the formula must not go
    // negative-index UB) clamps to the level-0 requirement.
    assert(xpToNextLevel(-5) == 7);

    using gameplay::totalForLevel;
    assert(totalForLevel(0) == 0);
    assert(totalForLevel(1) == 7);
    assert(totalForLevel(15) == 315);
    assert(totalForLevel(16) == 315 + 37);
    assert(totalForLevel(30) == 1395);
    assert(totalForLevel(31) == 1395 + 112);
    // totalForLevel must be the closed form of walking xpToNextLevel from 0;
    // cross-check a stretch spanning all three segments against that walk.
    std::int64_t walked = 0;
    for (std::int32_t level = 0; level < 200; ++level) {
        assert(totalForLevel(level) == static_cast<std::int32_t>(walked));
        walked += xpToNextLevel(level);
    }
    std::cout << "testFormulaBoundaries OK\n";
}

// --- addExperience: simple within-level and single-level-up/down cases. ---
void testAddExperienceBasic() {
    gameplay::PlayerExperience xp;
    assert(xp.level() == 0);
    assert(xp.pointsIntoLevel() == 0);
    assert(xp.progress() == 0.0F);

    // Level 0 needs 7 points; 3 points in leaves 3/7 progress, no level up.
    xp.addExperience(3);
    assert(xp.level() == 0);
    assert(xp.pointsIntoLevel() == 3);
    assert(xp.totalExperience() == 3);
    assert(std::abs(xp.progress() - 3.0F / 7.0F) < 1e-6F);

    // 4 more points crosses the 7-point threshold: level 1, 0 leftover.
    xp.addExperience(4);
    assert(xp.level() == 1);
    assert(xp.pointsIntoLevel() == 0);
    assert(xp.totalExperience() == 7);

    // A big single addExperience call must land the same place a walk would:
    // level 1 needs 9, level 2 needs 11 -> 9+11=20 crosses two levels with 5
    // left over (25 total added this call).
    xp.addExperience(25);
    assert(xp.level() == 3);
    assert(xp.pointsIntoLevel() == 5);
    assert(xp.totalExperience() == 32);

    // A negative addExperience (future onEnchantmentPerformed-style cost)
    // walks back down one level at a time. Player#giveExperiencePoints clamps
    // totalExperience with the call's raw delta unconditionally
    // (`Mth.clamp(totalExperience + i, 0, MAX_VALUE)`, not gated on the sign
    // of i), so a -6 call subtracts 6 from the running total the same way a
    // +6 call would add it.
    const auto totalBefore = xp.totalExperience();
    xp.addExperience(-6);
    assert(xp.totalExperience() == totalBefore - 6);
    assert(xp.level() == 2);
    assert(xp.pointsIntoLevel() == 10);  // 5 - 6 = -1, borrows level 2's 11 -> 10
    std::cout << "testAddExperienceBasic OK\n";
}

// --- addExperience crossing many levels down to (and clamped at) zero. ---
void testAddExperienceDownToFloor() {
    gameplay::PlayerExperience xp;
    xp.giveExperienceLevels(10);
    xp.addExperience(50);  // some points into level 10
    const auto beforeLevel = xp.level();
    assert(beforeLevel >= 10);

    // A huge negative delta must clamp at level 0 / points 0, never go
    // negative or wrap, and total resets to 0 (vanilla's giveLevels()
    // clamp-to-zero also zeroes total).
    xp.addExperience(-100000);
    assert(xp.level() == 0);
    assert(xp.pointsIntoLevel() == 0);
    assert(xp.totalExperience() == 0);
    assert(xp.progress() == 0.0F);
    std::cout << "testAddExperienceDownToFloor OK\n";
}

// --- addExperience round trip: adding 1000x +1 then giving back exactly that
// many points via -1 steps must return to total==0/progress==0 — the
// sabotage②-target assertion (an independently-accumulated float progress
// would drift here; the integer-derived progress cannot). ---
void testNoDriftOnRepeatedAddSubtract() {
    gameplay::PlayerExperience xp;
    for (int i = 0; i < 1000; ++i) {
        xp.addExperience(1);
    }
    const auto levelAfterUp = xp.level();
    const auto pointsAfterUp = xp.pointsIntoLevel();
    assert(levelAfterUp > 0);  // 1000 points certainly cross several levels

    for (int i = 0; i < 1000; ++i) {
        xp.addExperience(-1);
    }
    assert(xp.level() == 0);
    assert(xp.pointsIntoLevel() == 0);
    assert(xp.totalExperience() == 0);
    assert(xp.progress() == 0.0F);
    static_cast<void>(levelAfterUp);
    static_cast<void>(pointsAfterUp);
    std::cout << "testNoDriftOnRepeatedAddSubtract OK\n";
}

// --- giveExperienceLevels: saturating add, clamp-to-zero wipes progress and
// total together. ---
void testGiveExperienceLevels() {
    gameplay::PlayerExperience xp;
    xp.giveExperienceLevels(5);
    assert(xp.level() == 5);
    xp.addExperience(3);  // some progress into level 5, so the wipe is visible
    assert(xp.pointsIntoLevel() > 0);

    xp.giveExperienceLevels(2);
    assert(xp.level() == 7);
    // giveExperienceLevels does not touch points-into-level in vanilla (only
    // the level field), so the existing in-level progress carries over.
    assert(xp.pointsIntoLevel() > 0);

    xp.giveExperienceLevels(-100);
    assert(xp.level() == 0);
    assert(xp.pointsIntoLevel() == 0);
    assert(xp.totalExperience() == 0);
    std::cout << "testGiveExperienceLevels OK\n";
}

// --- setExperienceLevel: pins the level, zeroes in-level progress. ---
void testSetExperienceLevel() {
    gameplay::PlayerExperience xp;
    xp.addExperience(500);
    xp.setExperienceLevel(20);
    assert(xp.level() == 20);
    assert(xp.pointsIntoLevel() == 0);
    assert(xp.progress() == 0.0F);
    xp.setExperienceLevel(-5);  // negative clamps to zero, matches vanilla intent
    assert(xp.level() == 0);
    std::cout << "testSetExperienceLevel OK\n";
}

// --- setExperiencePoints: re-derives level/progress from a raw total,
// matching totalForLevel's own accounting (points-into-level < the current
// level's requirement, and totalForLevel(level)+pointsIntoLevel == total). ---
void testSetExperiencePoints() {
    gameplay::PlayerExperience xp;
    for (const std::int32_t total : {0, 1, 6, 7, 8, 314, 315, 316, 1394, 1395, 1396, 100000}) {
        xp.setExperiencePoints(total);
        assert(xp.totalExperience() == total);
        const auto needed = gameplay::xpToNextLevel(xp.level());
        assert(xp.pointsIntoLevel() >= 0);
        assert(needed <= 0 || xp.pointsIntoLevel() < needed);
        assert(gameplay::totalForLevel(xp.level()) + xp.pointsIntoLevel() == total);
    }
    std::cout << "testSetExperiencePoints OK\n";
}

// --- consumeLevels / canAfford: atomic, refuses an over-draft, zeroes
// in-level progress when it lands exactly on zero (matching giveLevels'
// clamp-to-zero cosmetic reset — there is nothing left to show progress
// toward at level 0 with no points). ---
void testConsumeLevels() {
    gameplay::PlayerExperience xp;
    xp.giveExperienceLevels(5);
    assert(xp.canAfford(5));
    assert(!xp.canAfford(6));

    // Refused: insufficient levels, nothing changes.
    assert(!xp.consumeLevels(6));
    assert(xp.level() == 5);

    // Refused: zero/negative levels is not a valid charge.
    assert(!xp.consumeLevels(0));
    assert(!xp.consumeLevels(-1));
    assert(xp.level() == 5);

    assert(xp.consumeLevels(2));
    assert(xp.level() == 3);

    assert(xp.consumeLevels(3));
    assert(xp.level() == 0);
    assert(xp.pointsIntoLevel() == 0);
    std::cout << "testConsumeLevels OK\n";
}

// --- enchantmentSeed determinism: the SAME JavaRandom seed, run twice, must
// produce the SAME reroll sequence. This is the sabotage③-target assertion —
// a wall-clock or std::random_device reroll would fail this every run. ---
void testEnchantmentSeedDeterminism() {
    gameplay::PlayerExperience a;
    gameplay::PlayerExperience b;
    world::gen::JavaRandom rngA(0xABCDEF12ULL);
    world::gen::JavaRandom rngB(0xABCDEF12ULL);

    std::vector<std::int32_t> seedsA;
    std::vector<std::int32_t> seedsB;
    for (int i = 0; i < 8; ++i) {
        a.rerollEnchantmentSeed(rngA);
        b.rerollEnchantmentSeed(rngB);
        seedsA.push_back(a.enchantmentSeed());
        seedsB.push_back(b.enchantmentSeed());
    }
    assert(seedsA == seedsB);
    // Not every reroll should coincidentally repeat the same value (guards
    // against a stub that always returns 0 vacuously "matching").
    bool sawVariation = false;
    for (std::size_t i = 1; i < seedsA.size(); ++i) {
        if (seedsA[i] != seedsA[0]) {
            sawVariation = true;
            break;
        }
    }
    assert(sawVariation);
    std::cout << "testEnchantmentSeedDeterminism OK\n";
}

// --- restore(): the save/snapshot decode path bypasses the level-walking
// arithmetic and takes the four fields verbatim, clamping only for
// corrupt/negative input (never crashes on a hostile save). ---
void testRestore() {
    gameplay::PlayerExperience xp;
    xp.restore(12, 34, 500, 987654321);
    assert(xp.level() == 12);
    assert(xp.pointsIntoLevel() == 34);
    assert(xp.totalExperience() == 500);
    assert(xp.enchantmentSeed() == 987654321);

    // Negative/corrupt fields (a hostile or truncated save) clamp rather than
    // going negative — old-world backward compatibility: a save with no XP
    // block at all restores zeros, which must not misbehave either.
    gameplay::PlayerExperience corrupt;
    corrupt.restore(-1, -1, -1, 0);
    assert(corrupt.level() == 0);
    assert(corrupt.pointsIntoLevel() == 0);
    assert(corrupt.totalExperience() == 0);

    gameplay::PlayerExperience fresh;
    fresh.restore(0, 0, 0, 0);
    assert(fresh.level() == 0);
    assert(fresh.progress() == 0.0F);
    std::cout << "testRestore OK\n";
}

}  // namespace

int main() {
    testFormulaBoundaries();
    testAddExperienceBasic();
    testAddExperienceDownToFloor();
    testNoDriftOnRepeatedAddSubtract();
    testGiveExperienceLevels();
    testSetExperienceLevel();
    testSetExperiencePoints();
    testConsumeLevels();
    testEnchantmentSeedDeterminism();
    testRestore();
    std::cout << "player_experience_test: all tests passed\n";
    return 0;
}

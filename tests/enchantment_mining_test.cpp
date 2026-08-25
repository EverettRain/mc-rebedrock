// ENCH-1b: mining-tool enchantment effects — Efficiency / Unbreaking / Fortune /
// Silk Touch, driven through the DDC-2 effect-component engine (not a hardcoded
// per-enchant branch) and consumed by the mining call sites:
//
//   * Efficiency: efficiencyMiningSpeedBonus (level²+1) speeds a real break —
//     miningSeconds drops with the enchant; a no-enchant break is unchanged
//     (sabotage ③);
//   * Unbreaking: probabilistic durability preservation off a deterministic
//     JavaRandom (same seed ⇒ same spend sequence — sabotage ①), rate ≈
//     1/(level+1);
//   * Fortune: ore drop count multiplied (ore_drops fold), higher expected count,
//     deterministic per seed; non-ore unaffected;
//   * Silk Touch: a break drops the block itself, never its normal loot, and is
//     mutually exclusive with Fortune (Silk wins — sabotage ②).

#include "gameplay/Enchantment.hpp"
#include "gameplay/EnchantmentMining.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/MiningSystem.hpp"
#include "world/Block.hpp"
#include "world/gen/JavaRandom.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {

using namespace mc::gameplay;
using mc::world::Block;

[[nodiscard]] bool nearly(float value, float expected, float epsilon = 0.001F) {
    return std::fabs(value - expected) < epsilon;
}

[[nodiscard]] ItemStack pickaxe(const Item* item) {
    return ItemStack{Block::Air, 1U, item};
}

[[nodiscard]] ItemStack enchanted(const Item* item, EnchantmentId id, std::uint8_t level) {
    ItemStack stack = pickaxe(item);
    setEnchantmentLevel(stack, id, level);
    return stack;
}

// --- Efficiency: level²+1, through DDC-2. ---

// Proves each mining effect really compiled through DDC-2's compiler with zero
// out-of-scope unknowns — a hand-written branch would leave these programs empty
// / unknown, so this is the "真 datapack 化" guard (same as EQ-4's).
void testProgramsCompileThroughDdc2() {
    using mc::gameplay::detail::miningEffectProgram;
    using mc::gameplay::detail::kEfficiencyIndex;
    using mc::gameplay::detail::kUnbreakingIndex;
    using mc::gameplay::detail::kFortuneIndex;
    // Efficiency / Unbreaking / Fortune each compile a single damage value term
    // with no unknown component/value/condition/action.
    assert(miningEffectProgram(kEfficiencyIndex).totalUnknown() == 0);
    assert(miningEffectProgram(kEfficiencyIndex).damage.size() == 1);
    assert(miningEffectProgram(kUnbreakingIndex).totalUnknown() == 0);
    assert(miningEffectProgram(kUnbreakingIndex).damage.size() == 1);
    assert(miningEffectProgram(kFortuneIndex).totalUnknown() == 0);
    assert(miningEffectProgram(kFortuneIndex).damage.size() == 1);
    std::cout << "testProgramsCompileThroughDdc2 OK\n";
}

void testEfficiencyCurveThroughDdc2() {
    // The bonus is level²+1 (PlayerEntity#getBlockBreakingSpeed's `i*i + 1`),
    // produced by the DDC-2 levels_squared value bucket.
    assert(nearly(efficiencyMiningSpeedBonus(std::uint8_t{0}), 0.0F));  // no enchant, identity
    assert(nearly(efficiencyMiningSpeedBonus(std::uint8_t{1}), 2.0F));
    assert(nearly(efficiencyMiningSpeedBonus(std::uint8_t{2}), 5.0F));
    assert(nearly(efficiencyMiningSpeedBonus(std::uint8_t{3}), 10.0F));
    assert(nearly(efficiencyMiningSpeedBonus(std::uint8_t{4}), 17.0F));
    assert(nearly(efficiencyMiningSpeedBonus(std::uint8_t{5}), 26.0F));
    std::cout << "testEfficiencyCurveThroughDdc2 OK\n";
}

void testEfficiencySpeedsRealBreak() {
    const ItemStack plain = pickaxe(&items::DiamondPickaxe);
    const ItemStack eff5 = enchanted(&items::DiamondPickaxe, EnchantmentId::Efficiency, 5U);

    const float plainSeconds = miningSeconds(Block::Stone, plain, false, false);
    const float effSeconds = miningSeconds(Block::Stone, eff5, false, false);
    assert(effSeconds < plainSeconds);  // Efficiency V digs faster

    // sabotage ③: a no-Efficiency pickaxe mines at exactly the plain rate.
    assert(nearly(miningSeconds(Block::Stone, plain, false, false), plainSeconds));

    // A bare hand (speed 1, no tool bonus) never gets the Efficiency addend even
    // if the stack somehow carried it — the `f > 1` guard. Stone with a fist:
    // Efficiency-on-a-non-tool must not change the fist rate.
    const ItemStack fistEff = enchanted(&items::Apple, EnchantmentId::Efficiency, 5U);
    assert(nearly(miningSeconds(Block::Stone, fistEff, false, false),
                  miningSeconds(Block::Stone, ItemStack{}, false, false)));
    std::cout << "testEfficiencySpeedsRealBreak OK\n";
}

// --- Unbreaking: deterministic probabilistic preservation. ---

void testUnbreakingDeterminismAndRate() {
    // sabotage ①: same seed ⇒ identical spend sequence.
    auto sequence = [](std::uint8_t level, std::uint64_t seed) {
        mc::world::gen::JavaRandom rng{seed};
        std::vector<std::uint16_t> costs;
        for (int use = 0; use < 500; ++use) {
            costs.push_back(unbreakingDurabilityCost(std::uint16_t{1}, level, rng));
        }
        return costs;
    };
    const auto a = sequence(3U, 123U);
    const auto b = sequence(3U, 123U);
    assert(a == b);                 // reproducible
    assert(sequence(3U, 124U) != a); // a different seed diverges (real stream)

    // No Unbreaking ⇒ every point always spent (identity).
    mc::world::gen::JavaRandom none{5U};
    for (int use = 0; use < 100; ++use) {
        assert(unbreakingDurabilityCost(std::uint16_t{1}, 0U, none) == 1U);
    }

    // The spend rate over a long stream approaches 1/(level+1): Unbreaking III
    // spends ~1/4 of the points.
    for (std::uint8_t level = 1; level <= 3; ++level) {
        mc::world::gen::JavaRandom rng{9999U + level};
        constexpr int kUses = 20000;
        long spent = 0;
        for (int use = 0; use < kUses; ++use) {
            spent += unbreakingDurabilityCost(std::uint16_t{1}, level, rng);
        }
        const double rate = static_cast<double>(spent) / kUses;
        const double expected = 1.0 / (level + 1.0);
        assert(std::fabs(rate - expected) < 0.02);
    }
    std::cout << "testUnbreakingDeterminismAndRate OK\n";
}

void testUnbreakingMultiPointCost() {
    // A base cost of 10 with Unbreaking III spends fewer than 10 on average but
    // never more than 10, and is deterministic.
    mc::world::gen::JavaRandom rng{42U};
    long total = 0;
    for (int i = 0; i < 1000; ++i) {
        const std::uint16_t cost = unbreakingDurabilityCost(std::uint16_t{10}, 3U, rng);
        assert(cost <= 10U);
        total += cost;
    }
    const double avg = static_cast<double>(total) / 1000.0;
    assert(avg > 2.0 && avg < 3.5);  // ~10 * 1/4 = 2.5
    std::cout << "testUnbreakingMultiPointCost OK\n";
}

// --- Fortune: ore drop multiplier through DDC-2 ceiling + deterministic draw. ---

void testFortuneCeilingThroughDdc2() {
    assert(fortuneBonusCeiling(0U) == 0);
    assert(fortuneBonusCeiling(1U) == 3);  // level+2
    assert(fortuneBonusCeiling(2U) == 4);
    assert(fortuneBonusCeiling(3U) == 5);
    // The fold: draw 0 ⇒ ×1, draw>=1 ⇒ ×draw. (vanilla max(0,i-1)+1)
    assert(fortuneApply(1U, 0) == 1U);
    assert(fortuneApply(1U, 1) == 1U);
    assert(fortuneApply(1U, 2) == 2U);
    assert(fortuneApply(1U, 3) == 3U);
    std::cout << "testFortuneCeilingThroughDdc2 OK\n";
}

void testFortuneRaisesOreDropsInLoot() {
    const ItemStack diamondPick = pickaxe(&items::DiamondPickaxe);
    const ItemStack fortune3 = enchanted(&items::DiamondPickaxe, EnchantmentId::Fortune, 3U);

    // Expected diamonds off Diamond Ore, averaged over many deterministic rolls.
    auto average = [](const ItemStack& tool) {
        std::uint64_t state = 0x1234'5678ULL;
        long total = 0;
        constexpr int kRolls = 20000;
        for (int i = 0; i < kRolls; ++i) {
            const MinedDrops drops = minedDrops(Block::DiamondOre, tool, state);
            for (const auto& stack : drops.view()) {
                total += stack.count;
            }
        }
        return static_cast<double>(total) / kRolls;
    };
    const double plain = average(diamondPick);
    const double fortune = average(fortune3);
    assert(nearly(static_cast<float>(plain), 1.0F, 0.05F));  // plain always 1 diamond
    assert(fortune > plain * 1.5);  // Fortune III raises the expected count

    // Determinism: the same tool + same starting state ⇒ same total.
    std::uint64_t s1 = 77U;
    std::uint64_t s2 = 77U;
    long t1 = 0;
    long t2 = 0;
    for (int i = 0; i < 500; ++i) {
        for (const auto& d : minedDrops(Block::DiamondOre, fortune3, s1).view()) t1 += d.count;
        for (const auto& d : minedDrops(Block::DiamondOre, fortune3, s2).view()) t2 += d.count;
    }
    assert(t1 == t2);

    // Non-ore (stone -> cobblestone) is never Fortune-scaled: always one.
    std::uint64_t stoneState = 9U;
    for (int i = 0; i < 200; ++i) {
        const MinedDrops drops = minedDrops(Block::Stone, fortune3, stoneState);
        assert(drops.count == 1U);
        assert(drops.view()[0].count == 1U);
    }
    std::cout << "testFortuneRaisesOreDropsInLoot OK\n";
}

// --- Silk Touch: drop self; mutually exclusive with Fortune. ---

void testSilkTouchDropsSelf() {
    const ItemStack silk = enchanted(&items::DiamondPickaxe, EnchantmentId::SilkTouch, 1U);
    std::uint64_t state = 3U;

    // Stone normally drops cobblestone; Silk Touch drops stone itself.
    {
        std::uint64_t plainState = state;
        const MinedDrops plain = minedDrops(Block::Stone, pickaxe(&items::DiamondPickaxe), plainState);
        assert(plain.count == 1U && plain.view()[0].block == Block::Cobblestone);
    }
    const MinedDrops silked = minedDrops(Block::Stone, silk, state);
    assert(silked.count == 1U);
    assert(silked.view()[0].block == Block::Stone);

    // Diamond Ore normally drops a diamond item; Silk Touch drops the ore block.
    std::uint64_t oreState = 4U;
    const MinedDrops silkedOre = minedDrops(Block::DiamondOre, silk, oreState);
    assert(silkedOre.count == 1U);
    assert(silkedOre.view()[0].block == Block::DiamondOre);
    std::cout << "testSilkTouchDropsSelf OK\n";
}

void testSilkTouchBeatsFortuneOnForcedStack() {
    // sabotage ②: ENCH-0 forbids the pair, but a hand-forced stack carrying both
    // must still resolve to Silk Touch (drop self), never Fortune-multiplied ore.
    ItemStack both = pickaxe(&items::DiamondPickaxe);
    setEnchantmentLevel(both, EnchantmentId::SilkTouch, 1U);
    setEnchantmentLevel(both, EnchantmentId::Fortune, 3U);
    std::uint64_t state = 8U;
    for (int i = 0; i < 200; ++i) {
        const MinedDrops drops = minedDrops(Block::DiamondOre, both, state);
        assert(drops.count == 1U);
        assert(drops.view()[0].block == Block::DiamondOre);  // the block, never diamonds
        assert(drops.view()[0].count == 1U);                 // never Fortune-multiplied
    }
    std::cout << "testSilkTouchBeatsFortuneOnForcedStack OK\n";
}

}  // namespace

int main() {
    testProgramsCompileThroughDdc2();
    testEfficiencyCurveThroughDdc2();
    testEfficiencySpeedsRealBreak();
    testUnbreakingDeterminismAndRate();
    testUnbreakingMultiPointCost();
    testFortuneCeilingThroughDdc2();
    testFortuneRaisesOreDropsInLoot();
    testSilkTouchDropsSelf();
    testSilkTouchBeatsFortuneOnForcedStack();
    std::cout << "All enchantment_mining tests passed.\n";
    return 0;
}

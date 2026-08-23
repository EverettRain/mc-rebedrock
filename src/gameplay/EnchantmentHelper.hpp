#pragma once

// ENCH-0 part 3: deterministic enchanting-table offer generation, headless —
// no block, no UI, no XP spend, no applying the result to a stack. Mirrors
// 1.16.1's EnchantmentScreenHandler#onContentChanged (the three-slot power/
// level computation) + EnchantmentHelper#calculateRequiredExperienceLevel +
// EnchantmentHelper#generateEnchantments + #getPossibleEntries, and
// EnchantmentScreenHandler#generateEnchantments (the private per-slot wrapper
// that reseeds `random` at `seed + slot` a SECOND time — vanilla really does
// call setSeed twice per slot: once in onContentChanged's shared loop to
// compute enchantmentPower[j], and again, independently, the moment a caller
// asks which enchantments that slot offers). Reproducing that double-seed is
// required for byte-exact parity: the level computation burns random draws
// that the offer computation must NOT see, so the offer step needs its own
// fresh stream from the same seed, not a continuation of the first stream.
//
// Source: yarn-mapped 1.16.1 net.minecraft.screen.EnchantmentScreenHandler,
// net.minecraft.enchantment.EnchantmentHelper (see Enchantment.hpp's banner
// for the jar location).

#include "gameplay/Enchantment.hpp"
#include "gameplay/Inventory.hpp"
#include "world/gen/JavaRandom.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mc::gameplay {

// One candidate the weighted picker can draw: an enchantment at a specific
// level, weighted by its rarity (EnchantmentLevelEntry).
struct EnchantmentLevelEntry final {
    EnchantmentId id = EnchantmentId::Count;
    std::int32_t level = 0;
    std::uint32_t weight = 0;
};

// The three enchanting-table slots: each slot's required level (0 = the item
// is too enchantable-poor or the slot's roll landed below `slot+1`, meaning
// the slot is dead and offers nothing, exactly like vanilla zeroing
// enchantmentPower[j]) and the enchantment(s) that slot would apply.
struct EnchantmentTableOffer final {
    std::int32_t requiredLevel = 0;
    std::vector<EnchantmentLevelEntry> enchantments{};
};

struct EnchantmentTableOffers final {
    std::array<EnchantmentTableOffer, 3> slots{};
};

namespace detail {

// EnchantmentHelper#calculateRequiredExperienceLevel, verbatim: burns
// `1 + nextInt(8) + (bookshelfCount>>1) + nextInt(bookshelfCount+1)` draws off
// `random`, then folds slotIndex into the return shape. bookshelfCount is
// clamped to [0,15] first (vanilla clamps only the upper bound; a negative
// count cannot occur from the geometry scan this project will eventually
// wire in, but callers must not pass one).
[[nodiscard]] inline std::int32_t calculateRequiredExperienceLevel(
    world::gen::JavaRandom& random, int slotIndex, int bookshelfCount, std::int32_t enchantability) {
    if (enchantability <= 0) {
        return 0;
    }
    const int clampedShelves = std::min(bookshelfCount, 15);
    const std::int32_t j = random.nextInt(8) + 1 + (clampedShelves >> 1) +
        random.nextInt(clampedShelves + 1);
    if (slotIndex == 0) {
        return std::max(j / 3, 1);
    }
    if (slotIndex == 1) {
        return j * 2 / 3 + 1;
    }
    return std::max(j, clampedShelves * 2);
}

// EnchantmentHelper#getPossibleEntries: for every enchantment that (a) is not
// treasure-only unless treasureAllowed, (b) is available for random selection
// at all (Soul Speed never is), and (c) applies to this item (or the stack is
// a book, which accepts everything) — find its HIGHEST level whose
// [minCost,maxCost] window contains `power`, highest level first (vanilla
// iterates maxLevel down to minLevel and breaks on the first hit).
[[nodiscard]] inline std::vector<EnchantmentLevelEntry> getPossibleEntries(
    std::int32_t power, const ItemStack& stack, bool treasureAllowed) {
    std::vector<EnchantmentLevelEntry> result;
    const bool isBook = stack.item == &items::Book;
    for (std::size_t index = 0; index < kEnchantmentCount; ++index) {
        const auto id = static_cast<EnchantmentId>(index);
        const auto& definition = enchantmentDefinition(id);
        if (definition.treasureOnly && !treasureAllowed) continue;
        if (!definition.availableForRandomSelection) continue;
        if (!isBook && !canEnchant(id, stack)) continue;
        for (std::int32_t level = definition.maxLevel; level >= definition.minLevel; --level) {
            const auto minCost = getMinCost(id, level);
            const auto maxCost = getMaxCost(id, level);
            if (power >= minCost && power <= maxCost) {
                result.push_back(EnchantmentLevelEntry{
                    id, level, enchantmentRarityWeight(enchantmentRarity(id))});
                break;
            }
        }
    }
    return result;
}

// WeightedPicker.getRandom: draw nextInt(weightSum), then walk the list
// subtracting weights until the running remainder goes negative. Returns the
// vector index chosen (rather than a pointer/iterator) so the caller can also
// erase it, matching Util.getLast(list) + removeConflicts' iterator-erase use.
[[nodiscard]] inline std::size_t weightedPickIndex(
    world::gen::JavaRandom& random, const std::vector<EnchantmentLevelEntry>& entries) {
    std::uint32_t weightSum = 0;
    for (const auto& entry : entries) weightSum += entry.weight;
    std::int32_t mark = random.nextInt(static_cast<std::int32_t>(weightSum));
    for (std::size_t index = 0; index < entries.size(); ++index) {
        mark -= static_cast<std::int32_t>(entries[index].weight);
        if (mark < 0) return index;
    }
    return entries.size() - 1U;  // Unreachable in a well-formed weight sum; last as a safe fallback.
}

// Enchantment#canCombine both directions (already symmetric — see
// isCompatibleWith) — removes every remaining candidate incompatible with
// `picked`, including `picked` itself (canCombine(self) is false via the
// `first == second` guard in isCompatibleWith, matching
// Enchantment#canAccept's default `this != other`).
inline void removeConflicts(std::vector<EnchantmentLevelEntry>& entries,
                            const EnchantmentLevelEntry& picked) {
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [&](const EnchantmentLevelEntry& candidate) {
                           return !isCompatibleWith(picked.id, candidate.id);
                       }),
        entries.end());
}

// EnchantmentHelper#generateEnchantments: the enchantability jitter
// (`level += 1 + nextInt(ench/4+1) + nextInt(ench/4+1)`), the +/-15% spread
// (`(nextFloat()+nextFloat()-1)*0.15`), then the weighted-pick-and-conflict-
// removal loop that can add more than one enchantment when
// `nextInt(50) <= level` keeps succeeding (level halves — integer division —
// after every extra pick, matching vanilla's `level /= 2`).
[[nodiscard]] inline std::vector<EnchantmentLevelEntry> generateEnchantments(
    world::gen::JavaRandom& random, const ItemStack& stack, std::int32_t level,
    bool treasureAllowed) {
    std::vector<EnchantmentLevelEntry> result;
    const std::int32_t enchantability = itemEnchantability(stack);
    if (enchantability <= 0) {
        return result;
    }
    level += 1 + random.nextInt(enchantability / 4 + 1) + random.nextInt(enchantability / 4 + 1);
    const float spread = (random.nextFloat() + random.nextFloat() - 1.0F) * 0.15F;
    level = std::max(
        1, static_cast<std::int32_t>(std::lround(static_cast<float>(level) + static_cast<float>(level) * spread)));

    auto candidates = getPossibleEntries(level, stack, treasureAllowed);
    if (candidates.empty()) {
        return result;
    }
    {
        const auto index = weightedPickIndex(random, candidates);
        result.push_back(candidates[index]);
    }
    while (random.nextInt(50) <= level) {
        removeConflicts(candidates, result.back());
        if (candidates.empty()) {
            break;
        }
        const auto index = weightedPickIndex(random, candidates);
        result.push_back(candidates[index]);
        level /= 2;
    }
    return result;
}

} // namespace detail

// EnchantmentScreenHandler#onContentChanged + the private per-slot
// #generateEnchantments wrapper, fused into one deterministic call: given the
// SAME (enchantmentSeed, bookshelfCount, stack) triple, always produces the
// SAME three-slot result (ENCH-0's determinism requirement — no wall clock,
// no std::rand, the only randomness source is the JavaRandom stream this
// function seeds itself from `enchantmentSeed`). `treasureAllowed` is false
// here, matching vanilla's table (only the loot-table/villager-trade path
// ever passes true, and neither exists yet).
[[nodiscard]] inline EnchantmentTableOffers generateTableOffers(
    std::int32_t enchantmentSeed, int bookshelfCount, const ItemStack& stack) {
    EnchantmentTableOffers offers;
    const std::int32_t enchantability = itemEnchantability(stack);

    // Pass 1 (onContentChanged's shared loop): one JavaRandom stream, seeded
    // once at `enchantmentSeed`, computes all three slot power levels in
    // order — the three nextInt-heavy calculateRequiredExperienceLevel calls
    // share ONE continuing stream, not three independent ones.
    {
        world::gen::JavaRandom random(static_cast<std::uint64_t>(enchantmentSeed));
        for (int slot = 0; slot < 3; ++slot) {
            std::int32_t power = detail::calculateRequiredExperienceLevel(
                random, slot, bookshelfCount, enchantability);
            if (power < slot + 1) {
                power = 0;
            }
            offers.slots[static_cast<std::size_t>(slot)].requiredLevel = power;
        }
    }

    // Pass 2 (the private generateEnchantments(stack, slot, power) wrapper):
    // each slot reseeds independently at `enchantmentSeed + slot`, so slot 1's
    // offer computation cannot be perturbed by how many draws slot 0's offer
    // computation happened to burn.
    for (int slot = 0; slot < 3; ++slot) {
        auto& offer = offers.slots[static_cast<std::size_t>(slot)];
        if (offer.requiredLevel <= 0) {
            continue;
        }
        world::gen::JavaRandom random(
            static_cast<std::uint64_t>(static_cast<std::int64_t>(enchantmentSeed) + slot));
        auto picked = detail::generateEnchantments(random, stack, offer.requiredLevel,
                                                    /*treasureAllowed=*/false);
        // A book that rolled more than one enchantment loses one at random
        // (EnchantmentScreenHandler's private wrapper: books never show two
        // enchantments as a table preview even though generateEnchantments
        // itself can pick several — the second+ entries silently apply on
        // purchase in vanilla, but the OFFER always drops down to size 1 for
        // a book by removing a random extra right here).
        if (stack.item == &items::Book && picked.size() > 1U) {
            const auto removeIndex = static_cast<std::size_t>(
                random.nextInt(static_cast<std::int32_t>(picked.size())));
            picked.erase(picked.begin() + static_cast<std::ptrdiff_t>(removeIndex));
        }
        offer.enchantments = std::move(picked);
    }
    return offers;
}

} // namespace mc::gameplay

#pragma once

// ENCH-2: the enchanting table's menu — the two input slots, the bookshelf
// power scan, the three offers and the actual purchase.
//
// 26.1 splits this across EnchantingTableBlock (the BOOKSHELF_OFFSETS scan) and
// EnchantmentMenu (the slots, the derived costs/clues and clickMenuButton).
// Neither half needs a block entity: EnchantmentMenu owns a plain
// `SimpleContainer(2)` and hands its contents back in removed(), so the table
// block itself stores nothing. This file follows that exactly — the menu lives
// on the ServerPlayer that opened it, so there is no block-entity registration,
// no per-table storage and, deliberately, no save-format change.
//
// The offer math itself is NOT here: ENCH-0 already landed it as the pure,
// deterministic `generateTableOffers` (EnchantmentHelper.hpp). This node
// supplies the two inputs that function was always waiting for — a real
// bookshelf count and a real item — plus the spend that ENCH-0 explicitly did
// not do.

#include "gameplay/EnchantmentHelper.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/PlayerExperience.hpp"
#include "world/Block.hpp"
#include "world/World.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>

namespace mc::gameplay {

// EnchantingTableBlock.BOOKSHELF_OFFSETS, evaluated: every cell of the
// 5x5x2 box (-2,0,-2)..(2,1,2) whose |x| or |z| is 2 — i.e. the two square
// rings at the table's own level and one above it. 16 per ring, 32 in all
// (vanilla then clamps the resulting count to 15 inside the cost formula, so
// the extra 17 shelves are real but inert, exactly as in vanilla).
inline constexpr std::size_t kBookshelfOffsetCount = 32U;

[[nodiscard]] constexpr std::array<glm::ivec3, kBookshelfOffsetCount> bookshelfOffsets() {
    std::array<glm::ivec3, kBookshelfOffsetCount> offsets{};
    std::size_t next = 0U;
    // BlockPos.betweenClosedStream iterates x outermost, then y, then z; the
    // order matters only for reproducibility of the scan, never for the count.
    for (int x = -2; x <= 2; ++x) {
        for (int y = 0; y <= 1; ++y) {
            for (int z = -2; z <= 2; ++z) {
                if (x != 2 && x != -2 && z != 2 && z != -2) {
                    continue;
                }
                offsets[next++] = glm::ivec3{x, y, z};
            }
        }
    }
    return offsets;
}

// The `#minecraft:enchantment_power_provider` tag — 26.1 lists exactly one
// block in it. Spelled as a predicate rather than a BlockTag entry because a
// one-member tag would cost a whole tag bit and a builtin-defaults line to say
// what this says in one comparison; ENCH-4/a datapack that ever needs a second
// provider is where the tag earns its keep.
[[nodiscard]] constexpr bool isEnchantmentPowerProvider(world::Block block) {
    return block == world::Block::Bookshelf;
}

// The `#minecraft:enchantment_power_transmitter` tag, which 26.1 defines as
// `#minecraft:replaceable` — the gap between the table and the shelf must be
// air (or another replaceable, e.g. grass). This is the rule that makes a shelf
// behind a wall not count.
//
// Known narrowing: this project's `replaceable` flag covers air/water/the four
// grass-family plants, while vanilla's #replaceable also holds fire, snow,
// vines and the rest. So a table with fire in the gap reads one shelf short of
// vanilla. That gap is the block roster's, not this node's — widening it means
// auditing every replaceable block, which belongs in B/BlockTags.
[[nodiscard]] constexpr bool isEnchantmentPowerTransmitter(world::Block block) {
    return world::isReplaceable(block);
}

// EnchantingTableBlock#isValidBookShelf: the offset cell holds a provider AND
// the cell at (x/2, y, z/2) — Java's truncating integer division, so ±2 maps to
// ±1 and ±1 maps to 0 — transmits. The halved offset is the cell between the
// table and the shelf; the y is NOT halved, matching vanilla.
[[nodiscard]] inline bool isValidBookShelf(const world::World& world, glm::ivec3 table,
                                           glm::ivec3 offset) {
    const glm::ivec3 shelf = table + offset;
    const glm::ivec3 gap = table + glm::ivec3{offset.x / 2, offset.y, offset.z / 2};
    return isEnchantmentPowerProvider(world.state(shelf.x, shelf.y, shelf.z).block()) &&
           isEnchantmentPowerTransmitter(world.state(gap.x, gap.y, gap.z).block());
}

// EnchantmentMenu#slotsChanged's `bookcases` loop: how many of the 32 offsets
// hold a valid shelf. Unclamped (the cost formula clamps to 15 itself), so a
// caller can also use this to show "you have more shelves than the table can
// use".
[[nodiscard]] inline int bookshelfPower(const world::World& world, glm::ivec3 table) {
    int count = 0;
    for (const glm::ivec3& offset : bookshelfOffsets()) {
        if (isValidBookShelf(world, table, offset)) {
            ++count;
        }
    }
    return count;
}

// ItemStack#isEnchantable: the item must be enchantable at all AND carry no
// enchantments yet (26.1 returns false the moment ENCHANTMENTS is non-empty —
// the table never re-enchants an already-enchanted item; that is the anvil's
// job, ENCH-3).
//
// The `count == 1` clause is ours, not vanilla's: vanilla enforces it through
// the slot (`getMaxStackSize() { return 1; }`), and this project's slot model
// has no per-slot stack limit yet. Without it a stack of 64 books would take
// one purchase's cost and come out as 64 enchanted books. Enforced here so the
// rule holds wherever the menu is driven from, not just in the UI.
[[nodiscard]] inline bool isEnchantable(const ItemStack& stack) {
    return !stack.empty() && stack.count == 1U && stack.enchantmentCount == 0U &&
           itemEnchantability(stack) > 0;
}

// The three-option menu one player has open at one table. Values, not
// pointers — this lives on the ServerPlayer and is stowed back into the
// inventory when the screen closes (EnchantmentMenu#removed -> clearContainer).
struct EnchantingMenu final {
    // The table this menu belongs to. Only meaningful while the screen is open.
    glm::ivec3 position{};
    ItemStack item{};
    ItemStack lapis{};
    // The last scanned shelf count (recomputed each tick the screen is open,
    // so building a shelf wall updates the preview live, as in vanilla).
    int bookshelfPower = 0;
    EnchantmentTableOffers offers{};

    // The exact inputs `offers` was derived from. Regenerating is cheap but not
    // free (three JavaRandom streams and a candidate scan per slot), and it must
    // not run on a tick where nothing changed, or a passing tick would look like
    // a reroll to anyone reading the offers.
    std::int32_t derivedSeed = 0;
    int derivedPower = -1;
    ItemStack derivedItem{};
    bool derived = false;

    [[nodiscard]] bool empty() const { return item.empty() && lapis.empty(); }
};

// EnchantmentMenu#slotsChanged: recompute the three offers from the current
// (seed, shelf count, item) triple, or clear them when the item cannot be
// enchanted. A no-op when the triple has not moved since the last call, so this
// is safe to call every tick.
inline void refreshOffers(EnchantingMenu& menu, std::int32_t enchantmentSeed) {
    if (menu.derived && menu.derivedSeed == enchantmentSeed &&
        menu.derivedPower == menu.bookshelfPower && menu.derivedItem == menu.item) {
        return;
    }
    menu.derivedSeed = enchantmentSeed;
    menu.derivedPower = menu.bookshelfPower;
    menu.derivedItem = menu.item;
    menu.derived = true;
    if (!isEnchantable(menu.item)) {
        menu.offers = {};
        return;
    }
    menu.offers = generateTableOffers(enchantmentSeed, menu.bookshelfPower, menu.item);
}

// What a purchase attempt did. `applied` false means nothing at all changed —
// every failure path in clickMenuButton is a plain `return false` with no side
// effects, and so is this.
struct EnchantPurchase final {
    bool applied = false;
    std::int32_t levelsSpent = 0;
    std::uint8_t lapisSpent = 0;
};

// EnchantmentMenu#clickMenuButton, in full.
//
// The two costs are DIFFERENT numbers and mixing them up is the classic bug
// here: `offers.slots[i].requiredLevel` is only a THRESHOLD the player's level
// must clear, while what is actually spent is `i + 1` levels and `i + 1` lapis.
// Vanilla's gate is `experienceLevel >= i+1 && experienceLevel >= costs[i]`,
// both, and creative bypasses both plus the lapis check.
//
// On success: the enchantments land on the item, the lapis shrinks, the levels
// are spent through XP-4's consumeLevels (never by touching the level field),
// and the player's enchantment seed rerolls — so the same table shows three
// different offers the moment the purchase completes.
[[nodiscard]] inline EnchantPurchase purchase(EnchantingMenu& menu, PlayerExperience& experience,
                                              int optionIndex, bool infiniteMaterials,
                                              world::gen::JavaRandom& seedRandom) {
    if (optionIndex < 0 || optionIndex >= 3) {
        return {};
    }
    const auto slot = static_cast<std::size_t>(optionIndex);
    const auto cost = static_cast<std::uint8_t>(optionIndex + 1);
    if (!infiniteMaterials && (menu.lapis.empty() || menu.lapis.count < cost)) {
        return {};
    }
    const EnchantmentTableOffer& offer = menu.offers.slots[slot];
    if (offer.requiredLevel <= 0 || menu.item.empty()) {
        return {};
    }
    if (!infiniteMaterials &&
        (!experience.canAfford(static_cast<std::int32_t>(cost)) ||
         !experience.canAfford(offer.requiredLevel))) {
        return {};
    }
    // vanilla re-rolls the offer list here rather than trusting the preview it
    // showed; an empty list is a silent no-op (`if (!newEnchantment.isEmpty())`)
    // that spends nothing. Our offers were derived from the identical
    // (seed, power, item) triple by the identical function, so this is the same
    // list — read it rather than regenerate it, and treat "no enchantments" as
    // vanilla's empty-list no-op.
    if (offer.enchantments.empty()) {
        return {};
    }

    EnchantPurchase result;
    if (!infiniteMaterials) {
        if (!experience.consumeLevels(static_cast<std::int32_t>(cost))) {
            return {};
        }
        result.levelsSpent = static_cast<std::int32_t>(cost);
        menu.lapis.count = static_cast<std::uint8_t>(menu.lapis.count - cost);
        if (menu.lapis.count == 0U) {
            menu.lapis = {};
        }
        result.lapisSpent = cost;
    }
    // EnchantmentMenu#clickMenuButton's `transmuteCopy(Items.ENCHANTED_BOOK)`:
    // a book that goes through the table comes out as a DIFFERENT item, not as
    // a book carrying enchantments. That is what keeps an enchanted book from
    // stacking with plain books and from being read as a crafting ingredient.
    // Done before the enchantments are written, as vanilla does.
    if (menu.item.item == &items::Book) {
        menu.item.item = &items::EnchantedBook;
    }
    for (const EnchantmentLevelEntry& entry : offer.enchantments) {
        setEnchantmentLevel(menu.item, entry.id, static_cast<std::uint8_t>(entry.level));
    }
    // Player#onEnchantmentPerformed's tail: a fresh seed, so the next open shows
    // a different three. Drawn from a caller-owned deterministic stream — never
    // the wall clock (REGULAR §3).
    experience.rerollEnchantmentSeed(seedRandom);
    // The offers were derived from the old seed and an unenchanted item; both
    // just changed, so force the next refreshOffers to recompute.
    menu.derived = false;
    result.applied = true;
    return result;
}

} // namespace mc::gameplay

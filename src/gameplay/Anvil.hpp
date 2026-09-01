#pragma once

// ENCH-3: the anvil — combining two items, repairing one with its material, and
// what either costs.
//
// Transcribed from 26.1 `AnvilMenu#createResult` / `#onTake`
// (`world/inventory/AnvilMenu.java:117-260`). Like the enchanting table, the
// menu is player-scoped rather than a block entity: vanilla's `ItemCombinerMenu`
// owns its own input container and returns it in `removed()`, so the anvil block
// stores nothing.
//
// The whole thing is a pure function of (left, right, infiniteMaterials) — that
// is the point. `refreshResult` never touches the world, the player or a clock,
// so the cost table, the enchantment merge rules and the 40-level wall are all
// assertable headless. Only `takeResult` mutates, and only the two things it is
// allowed to: the player's levels and the menu's own slots.

#include "gameplay/CustomNames.hpp"
#include "gameplay/Enchantment.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/PlayerExperience.hpp"

#include <glm/vec3.hpp>

#include <algorithm>
#include <cstdint>
#include <string>

namespace mc::gameplay {

// AnvilMenu's "too expensive" wall. An operation priced at or above this is
// refused outright for a survival player (creative ignores it), and it is what
// makes the prior-work penalty terminal rather than merely annoying.
inline constexpr std::int32_t kAnvilMaximumCost = 40;

// Enchantment#getAnvilCost. 26.1 carries it per enchantment in the datapack
// (`anvil_cost`), but every one of the 43 vanilla enchantments derives it from
// its rarity weight and nothing else — verified across the whole 26.1
// enchantment directory: weight 10 -> 1, 5 -> 2, 2 -> 4, 1 -> 8, with no
// exceptions. So this reads the rarity the registry already has rather than
// adding a parallel per-enchantment column that could drift from it. (DDC-4,
// reading the official data pack, is where the literal `anvil_cost` field would
// take over.)
[[nodiscard]] constexpr std::int32_t enchantmentAnvilCost(EnchantmentId id) {
    switch (enchantmentRarity(id)) {
    case EnchantmentRarity::Common: return 1;
    case EnchantmentRarity::Uncommon: return 2;
    case EnchantmentRarity::Rare: return 4;
    case EnchantmentRarity::VeryRare: return 8;
    }
    return 1;
}

// AnvilMenu#calculateIncreasedRepairCost: every trip through the anvil doubles
// the penalty (2n+1), so the sixth trip already prices the item past
// kAnvilMaximumCost and it can never be worked again. Saturates at the byte's
// ceiling, which is unobservable — see ItemStack::repairCost.
[[nodiscard]] constexpr std::uint8_t increasedRepairCost(std::uint8_t base) {
    const std::int32_t raised = static_cast<std::int32_t>(base) * 2 + 1;
    return static_cast<std::uint8_t>(std::min(raised, 255));
}

// EnchantmentHelper#canStoreEnchantments: what the anvil will work on at all.
//
// In 26.1 that is `stack.has(ENCHANTMENTS)`, and every item carries an (empty)
// ENCHANTMENTS component by default — so the answer is "anything non-empty",
// which is why vanilla lets you rename a stack of dirt. This used to require
// enchantability, which silently made renaming an ordinary block impossible;
// I-3 (renaming) is what exposed it.
//
// Loosening it does not loosen COMBINING: that branch still demands the same
// damageable item on both sides (or a book), so two stacks of dirt reach it and
// bounce straight back out.
[[nodiscard]] inline bool acceptsAnvilWork(const ItemStack& stack) {
    return !stack.empty();
}

// Item#isValidRepairItem: the material that repairs a tool or a piece of armor.
// Vanilla keys this off each ToolMaterial/ArmorMaterial's own repair ingredient;
// this build's materials are two small enums, so the mapping is a switch over
// them rather than a per-item ingredient field.
[[nodiscard]] inline const Item* repairMaterialFor(const ItemStack& stack) {
    if (stack.item == nullptr) {
        return nullptr;
    }
    if (stack.item->armorMaterial != ArmorMaterialId::None) {
        switch (stack.item->armorMaterial) {
        case ArmorMaterialId::Leather: return &items::Leather;
        case ArmorMaterialId::Iron: return &items::IronIngot;
        case ArmorMaterialId::Gold: return &items::GoldIngot;
        case ArmorMaterialId::Diamond: return &items::Diamond;
        case ArmorMaterialId::Chainmail: return &items::IronIngot;
        case ArmorMaterialId::None: return nullptr;
        }
        return nullptr;
    }
    switch (stack.item->toolTier) {
    case ToolTier::Stone: return nullptr;   // cobblestone is a block, not an item
    case ToolTier::Iron: return &items::IronIngot;
    case ToolTier::Gold: return &items::GoldIngot;
    case ToolTier::Diamond: return &items::Diamond;
    case ToolTier::Wood: return nullptr;    // planks are a block, not an item
    case ToolTier::None: return nullptr;
    }
    return nullptr;
}

[[nodiscard]] inline bool isValidRepairItem(const ItemStack& target, const ItemStack& material) {
    const Item* expected = repairMaterialFor(target);
    return expected != nullptr && !material.empty() && material.item == expected;
}

// The anvil's two inputs, its derived output and price. Player-scoped: it lives
// on the ServerPlayer beside the crafting grid and the enchanting menu, and
// closeContainerMenu hands the inputs back.
struct AnvilMenu final {
    glm::ivec3 position{};
    ItemStack left{};
    ItemStack right{};
    // I-3: what the rename box holds. Empty means "no rename typed"; vanilla
    // distinguishes that from "typed the item's own name" (both cost nothing)
    // and from "typed something else" (costs one level), and from an EMPTY box
    // over an already-renamed item, which STRIPS the name and also costs one.
    std::string name{};

    // Derived by refreshResult; never written from outside.
    ItemStack result{};
    std::int32_t cost = 0;
    // How many of `right` a material repair consumes (a repair may need fewer
    // ingots than the stack holds). Zero for every non-material-repair.
    std::uint8_t repairItemCountCost = 0U;

    [[nodiscard]] bool empty() const { return left.empty() && right.empty(); }
};

// AnvilMenu#createResult, transcribed. Pure: reads the two inputs, writes the
// menu's derived fields, touches nothing else.
//
// The order of the branches is vanilla's and matters — a material repair is
// tried BEFORE the same-item combine, so an iron pickaxe plus iron ingots is a
// repair (cheap, per-ingot) rather than being rejected for not being another
// pickaxe.
inline void refreshAnvilResult(AnvilMenu& menu, bool infiniteMaterials) {
    menu.result = {};
    menu.cost = 0;
    menu.repairItemCountCost = 0U;
    const ItemStack& input = menu.left;
    const ItemStack& addition = menu.right;
    if (!acceptsAnvilWork(input)) {
        return;
    }

    ItemStack result = input;
    std::int32_t price = 0;
    // The two inputs' accumulated prior-work penalties ride on top of whatever
    // this operation itself costs. This is the term that makes the fifth or
    // sixth combine unaffordable.
    std::int32_t tax = static_cast<std::int32_t>(input.repairCost) +
                       static_cast<std::int32_t>(addition.repairCost);

    if (!addition.empty()) {
        const bool usingBook = addition.item == &items::EnchantedBook;
        if (isDamageable(result) && isValidRepairItem(input, addition)) {
            // Material repair: each ingot mends a quarter of the maximum, and
            // costs one level, stopping as soon as the item is whole or the
            // stack runs out.
            std::int32_t repairAmount =
                std::min<std::int32_t>(result.damage, itemMaximumDamage(result) / 4);
            if (repairAmount <= 0) {
                return; // already undamaged: nothing to sell
            }
            std::uint8_t used = 0U;
            while (repairAmount > 0 && used < addition.count) {
                result.damage = static_cast<std::uint16_t>(
                    static_cast<std::int32_t>(result.damage) - repairAmount);
                ++price;
                ++used;
                repairAmount =
                    std::min<std::int32_t>(result.damage, itemMaximumDamage(result) / 4);
            }
            menu.repairItemCountCost = used;
        } else {
            // Combine. Without a book the two must be the same damageable item;
            // a book may be applied to anything the anvil accepts.
            if (!usingBook && (result.item != addition.item || !isDamageable(result))) {
                return;
            }
            if (isDamageable(result) && !usingBook) {
                // Durability combines, plus a 12%-of-maximum bonus.
                const std::int32_t maximum = itemMaximumDamage(result);
                const std::int32_t remainingLeft = maximum - static_cast<std::int32_t>(input.damage);
                const std::int32_t remainingRight =
                    itemMaximumDamage(addition) - static_cast<std::int32_t>(addition.damage);
                const std::int32_t remaining =
                    remainingLeft + remainingRight + maximum * 12 / 100;
                const std::int32_t resultDamage = std::max(0, maximum - remaining);
                if (resultDamage < static_cast<std::int32_t>(result.damage)) {
                    result.damage = static_cast<std::uint16_t>(resultDamage);
                    price += 2;
                }
            }

            bool anyCompatible = false;
            bool anyIncompatible = false;
            for (std::uint8_t index = 0; index < addition.enchantmentCount; ++index) {
                const auto id = static_cast<EnchantmentId>(addition.enchantments[index].id);
                const std::int32_t offered = addition.enchantments[index].level;
                const std::int32_t current = enchantmentLevel(result, id);
                // Equal levels merge upward by one; otherwise the higher wins.
                std::int32_t level = current == offered ? offered + 1
                                                        : std::max(offered, current);
                bool compatible = canEnchant(id, input);
                if (infiniteMaterials || input.item == &items::EnchantedBook) {
                    compatible = true;
                }
                // An enchantment that conflicts with one the target already
                // carries is refused AND charged for — vanilla prices the
                // wasted slot even though nothing is applied.
                for (std::uint8_t other = 0; other < result.enchantmentCount; ++other) {
                    const auto otherId =
                        static_cast<EnchantmentId>(result.enchantments[other].id);
                    if (otherId != id && !isCompatibleWith(id, otherId)) {
                        compatible = false;
                        ++price;
                    }
                }
                if (!compatible) {
                    anyIncompatible = true;
                    continue;
                }
                anyCompatible = true;
                level = std::min(level, static_cast<std::int32_t>(
                                            enchantmentDefinition(id).maxLevel));
                setEnchantmentLevel(result, id, static_cast<std::uint8_t>(level));
                std::int32_t fee = enchantmentAnvilCost(id);
                if (usingBook) {
                    fee = std::max(1, fee / 2);
                }
                price += fee * level;
                if (input.count > 1U) {
                    price = kAnvilMaximumCost;
                }
            }
            if (anyIncompatible && !anyCompatible) {
                return; // nothing applied and nothing to sell
            }
        }
    }

    // The naming branch, in vanilla's order: a name that differs from what the
    // item already shows costs one level and applies; an empty box over a named
    // item costs one level and strips it. Anything else is not a rename.
    std::int32_t namingCost = 0;
    const std::string_view currentName = customNameOf(result.customNameId);
    if (!menu.name.empty()) {
        if (menu.name != currentName) {
            namingCost = 1;
            price += namingCost;
            result.customNameId = customNames().intern(menu.name);
        }
    } else if (!currentName.empty()) {
        namingCost = 1;
        price += namingCost;
        result.customNameId = kNoCustomName;
    }

    if (price <= 0) {
        return; // the anvil has nothing to do (two pristine items, say)
    }
    menu.cost = tax + price;
    // AnvilMenu's `onlyRenaming`: a pure rename is capped one below the wall, so
    // renaming an item that has been worked to death is still possible. Note it
    // is the COST that is clamped, not the penalty.
    if (namingCost == price && namingCost > 0 && menu.cost >= kAnvilMaximumCost) {
        menu.cost = kAnvilMaximumCost - 1;
    }
    if (menu.cost >= kAnvilMaximumCost && !infiniteMaterials) {
        // "Too expensive!": the price is shown, the result is not.
        menu.result = {};
        return;
    }
    menu.result = result;
}

// What taking the result did. `applied` false means nothing changed at all.
struct AnvilTake final {
    bool applied = false;
    std::int32_t levelsSpent = 0;
};

// AnvilMenu#onTake: pay the levels, consume the inputs, and stamp the result
// with its raised prior-work penalty. The result stack is handed back to the
// caller (the click router puts it on the cursor).
[[nodiscard]] inline AnvilTake takeAnvilResult(AnvilMenu& menu, PlayerExperience& experience,
                                               bool infiniteMaterials, ItemStack& taken) {
    if (menu.result.empty() || menu.cost <= 0) {
        return {};
    }
    if (!infiniteMaterials && !experience.canAfford(menu.cost)) {
        return {};
    }
    AnvilTake outcome;
    if (!infiniteMaterials) {
        if (!experience.consumeLevels(menu.cost)) {
            return {};
        }
        outcome.levelsSpent = menu.cost;
    }

    taken = menu.result;
    // The result inherits the higher of the two inputs' penalties, then doubles
    // it — so the item that comes out is strictly more expensive to work again.
    // A PURE rename is the exception vanilla carves out (`namingCost != price`):
    // renaming does not raise the penalty, or naming a tool would quietly make
    // it dearer to repair forever.
    const bool pureRename = !menu.left.empty() && menu.right.empty() &&
                            customNameOf(menu.left.customNameId) != customNameOf(taken.customNameId);
    taken.repairCost =
        pureRename ? std::max(menu.left.repairCost, menu.right.repairCost)
                   : increasedRepairCost(std::max(menu.left.repairCost, menu.right.repairCost));

    menu.left = {};
    if (menu.repairItemCountCost > 0U && menu.right.count > menu.repairItemCountCost) {
        menu.right.count =
            static_cast<std::uint8_t>(menu.right.count - menu.repairItemCountCost);
    } else {
        menu.right = {};
    }
    refreshAnvilResult(menu, infiniteMaterials);
    outcome.applied = true;
    return outcome;
}

} // namespace mc::gameplay

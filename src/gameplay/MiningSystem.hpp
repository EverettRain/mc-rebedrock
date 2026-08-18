#pragma once

#include "gameplay/Inventory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mc::gameplay {

// What a block asks of the tool held against it. Blocks outside the vanilla
// mineable tags need no tool at all: they drop for a bare hand, and the wrong
// tool brings no speed bonus.
struct HarvestRequirement final {
    ToolType tool = ToolType::None;
    ToolTier tier = ToolTier::None;
};

// What a tool was just swung at. Java charges different durability for the two:
// a sword is cheap to fight with and expensive to dig with, a mining tool the
// other way round. Till is HoeItem#useOn: every tilled block costs one point,
// the same flat cost the vanilla hoe pays.
enum class ToolUse : std::uint8_t {
    BreakBlock,
    AttackEntity,
    Till,
};

// ToolItem#postMine / #postHit: how much durability one use costs. Blocks with
// no hardness at all (tall grass, torches) are free, and anything that is not a
// tool never wears down.
[[nodiscard]] std::uint16_t toolDurabilityCost(
    const ItemStack& stack,
    ToolUse use,
    float blockHardness);

// The tool role a stack's item wields, and the material it is made from.
[[nodiscard]] ToolType toolType(const ItemStack& stack);
[[nodiscard]] ToolTier toolTier(const ItemStack& stack);
[[nodiscard]] HarvestRequirement harvestRequirement(world::Block block);
[[nodiscard]] bool canHarvestBlock(world::Block block, const ItemStack& tool);
[[nodiscard]] float miningSeconds(
    world::Block block, const ItemStack& tool, bool underwater, bool airborne);

// Everything one broken block leaves behind. Oak leaves alone can roll a
// sapling, sticks and an apple off the same break, so a single stack is not
// enough to describe a loot table.
struct MinedDrops final {
    static constexpr std::size_t kMaximumEntries = 3;

    std::array<ItemStack, kMaximumEntries> entries{};
    std::size_t count = 0;

    void add(const ItemStack& stack);
    [[nodiscard]] bool empty() const { return count == 0; }
    [[nodiscard]] std::span<const ItemStack> view() const {
        return {entries.data(), count};
    }
};

// Rolls the block's loot table. `randomState` is the caller's LCG state (the
// stand-in for Level#random) and is advanced by every chance-based entry, so
// the same state sequence always produces the same drops. Pass an empty stack
// as `tool` for a break nobody swung at, such as a torch losing its wall.
// `age` is the crop's AGE property, 0-7, read off the state before the crop was
// removed; the crop loot tables roll against it. `doubledSlab` is set when the
// removed state was a double slab, which drops two slab items instead of one.
[[nodiscard]] MinedDrops minedDrops(
    world::Block block, const ItemStack& tool, std::uint32_t& randomState, int age = 0,
    bool doubledSlab = false);

} // namespace mc::gameplay

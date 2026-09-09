#pragma once

// AR-M4: ComposterBlock's fill/ready cycle.
//
// The block's geometry and its LEVEL axis live in world/ (Block.hpp,
// BlockShape.hpp, ElementModelBaker.hpp); what is here is the part that needs
// to know what an *item* is, which world/ may not (the layering rule that made
// MDL-1's connect family a BlockDefinition field rather than a tag query).
//
// Two consumers, which is why it is a header of its own rather than a lambda in
// PlayerInteraction: the player right-clicking a composter, and — the reason
// AR-M4 exists at all — the farmer villager's WorkAtComposter behaviour, which
// puts its harvest in through exactly the same rule.

#include "gameplay/Inventory.hpp"  // ItemStack: a compostable is an item OR a block
#include "gameplay/Item.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"

#include <array>
#include <cstdint>
#include <utility>

namespace mc::gameplay {

// ComposterBlock.MAX_LEVEL: the highest level a *fill* may reach. Level 8
// (world::kComposterReadyLevel) is only ever reached by the scheduled tick 20
// ticks later, never by an item going in.
inline constexpr int kComposterMaxFillLevel = 7;
// ComposterBlock#addItem's `level.scheduleTick(pos, block, 20)`.
inline constexpr int kComposterReadyDelayTicks = 20;

// COMPOSTABLES, restricted to what this roster actually has. The chances are
// vanilla's own five tiers (0.3 / 0.5 / 0.65 / 0.85 / 1.0); an item absent from
// both tables cannot be composted at all.
//
// Two tables because a compostable is sometimes an Item (bread, wheat) and
// sometimes a block wielded as its BlockItem (leaves, saplings, pumpkins) —
// exactly the split ItemStack itself carries.
struct BlockCompostChance final {
    world::Block block = world::Block::Air;
    float chance = 0.0F;
};

inline constexpr std::array<BlockCompostChance, 29> kBlockCompostables{{
    {world::Block::OakLeaves, 0.3F},        {world::Block::SpruceLeaves, 0.3F},
    {world::Block::BirchLeaves, 0.3F},      {world::Block::JungleLeaves, 0.3F},
    {world::Block::AcaciaLeaves, 0.3F},     {world::Block::DarkOakLeaves, 0.3F},
    {world::Block::MangroveLeaves, 0.3F},   {world::Block::OakSapling, 0.3F},
    {world::Block::SpruceSapling, 0.3F},    {world::Block::BirchSapling, 0.3F},
    {world::Block::JungleSapling, 0.3F},    {world::Block::AcaciaSapling, 0.3F},
    {world::Block::DarkOakSapling, 0.3F},   {world::Block::MangroveRoots, 0.3F},
    {world::Block::GrassPlant, 0.3F},       // `short_grass` in 26.1's names
    {world::Block::SugarCane, 0.5F},        {world::Block::TallGrass, 0.5F},
    {world::Block::Dandelion, 0.65F},       {world::Block::Poppy, 0.65F},
    {world::Block::OxeyeDaisy, 0.65F},      {world::Block::Fern, 0.65F},
    {world::Block::LargeFern, 0.65F},       {world::Block::BrownMushroom, 0.65F},
    {world::Block::RedMushroom, 0.65F},     {world::Block::Melon, 0.65F},
    {world::Block::Pumpkin, 0.65F},         {world::Block::MossBlock, 0.65F},
    {world::Block::HayBlock, 0.85F},        {world::Block::NetherWartBlock, 0.85F},
}};

// The item half. Pointers into the constexpr item table, so a comparison is one
// pointer compare — the same identity rule ItemStack itself uses.
[[nodiscard]] inline float itemCompostChance(const Item* item) {
    if (item == &items::WheatSeeds) return 0.3F;
    if (item == &items::Apple) return 0.65F;
    if (item == &items::Carrot) return 0.65F;
    if (item == &items::Potato) return 0.65F;
    if (item == &items::Wheat) return 0.65F;
    if (item == &items::Bread) return 0.85F;
    return 0.0F;
}

// The compost chance of whatever is being offered, or 0 for something that is
// not compostable. A real item wins over the stack's block, matching the
// "real item beats the same-named block" rule every resolution boundary in this
// build already follows.
[[nodiscard]] inline float compostChance(const ItemStack& stack) {
    if (stack.item != nullptr) {
        return itemCompostChance(stack.item);
    }
    for (const auto& row : kBlockCompostables) {
        if (row.block == stack.block) {
            return row.chance;
        }
    }
    return 0.0F;
}

[[nodiscard]] inline bool isCompostable(const ItemStack& stack) {
    return compostChance(stack) > 0.0F;
}

// ComposterBlock#addItem's roll. The draw is the CALLER's — this takes the
// rolled value — so the rule is a pure function that a test can drive through
// both branches without a world or an RNG.
//
//     if ((level != 0 || !(chance > 0)) && !(random.nextDouble() < chance)) fail
//
// which reads, once the double negative is unpacked: an EMPTY composter always
// accepts a compostable (that first item is free), and every later one rolls.
// Returns the new level, or the old one when the roll failed.
[[nodiscard]] constexpr int composterAddItem(int level, float chance, float roll) {
    if (chance <= 0.0F || level < 0 || level >= kComposterMaxFillLevel) {
        return level;
    }
    if (level != 0 && !(roll < chance)) {
        return level;
    }
    return level + 1;
}

} // namespace mc::gameplay

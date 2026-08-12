#include "gameplay/MiningSystem.hpp"

#include "gameplay/ItemPlacement.hpp"

#include <limits>

namespace mc::gameplay {
namespace {

// The same LCG the entity wander uses, so loot stays reproducible without a
// global generator (see EntitySystem.cpp).
[[nodiscard]] std::uint32_t nextRandom(std::uint32_t& state) {
    state = state * 1664525U + 1013904223U;
    return state;
}

// A value in [0, 1) from the top 24 bits (the low bits of an LCG are weak).
[[nodiscard]] float randomUnit(std::uint32_t& state) {
    return static_cast<float>(nextRandom(state) >> 8) / static_cast<float>(1U << 24);
}

// One roll of a loot-table condition. Always consumes a value so the sequence
// does not depend on earlier entries succeeding.
[[nodiscard]] bool rollChance(std::uint32_t& state, float chance) {
    return randomUnit(state) < chance;
}

// A uniform count in [minimum, maximum], mirroring UniformLootNumberProvider.
[[nodiscard]] std::uint8_t randomCount(std::uint32_t& state, std::uint8_t minimum,
                                       std::uint8_t maximum) {
    const std::uint32_t span = static_cast<std::uint32_t>(maximum - minimum) + 1U;
    return static_cast<std::uint8_t>(minimum + (nextRandom(state) >> 8) % span);
}

// The number of successes from `trials` independent rolls at `probability`,
// mirroring the binomial_with_bonus_count bonus formula the crop loot tables
// use for their extra produce (extra=3, probability=0.5714286 at fortune 0).
[[nodiscard]] std::uint8_t binomialCount(std::uint32_t& state, int trials, float probability) {
    std::uint8_t count = 0U;
    for (int roll = 0; roll < trials; ++roll) {
        if (rollChance(state, probability)) {
            ++count;
        }
    }
    return count;
}

} // namespace

ToolType toolType(const ItemStack& stack) {
    return stack.item == nullptr ? ToolType::None : stack.item->toolType;
}

ToolTier toolTier(const ItemStack& stack) {
    return stack.item == nullptr ? ToolTier::None : stack.item->toolTier;
}

std::uint16_t toolDurabilityCost(
    const ItemStack& stack,
    ToolUse use,
    float blockHardness) {
    if (!isDamageable(stack)) {
        return 0U;
    }
    const bool sword = toolType(stack) == ToolType::Sword;
    if (use == ToolUse::AttackEntity) {
        // SwordItem#postHit spends one, MiningToolItem#postHit spends two.
        return sword ? 1U : 2U;
    }
    if (use == ToolUse::Till) {
        // HoeItem#useOn always pays exactly one point.
        return 1U;
    }
    // Both postMine overloads leave a zero-hardness block free.
    if (blockHardness == 0.0F) {
        return 0U;
    }
    return sword ? 2U : 1U;
}

HarvestRequirement harvestRequirement(world::Block block) {
    switch (block) {
    // The stone family and coal ore call requiresCorrectToolForDrops, so a
    // pickaxe is needed to keep their loot; none of them are in needs_stone_tool,
    // so the wooden tier is enough. Sandstone, bricks, quartz, netherrack and
    // furnaces still drop for any hand here, with the pickaxe only digging them
    // faster (see isPickaxeBlock below) — 26.1 marks those five
    // requiresCorrectToolForDrops as well, which B2' picks up with the tag data.
    case world::Block::Stone:
    case world::Block::Cobblestone:
    case world::Block::MossyCobblestone:
    case world::Block::StoneBricks:
    case world::Block::MossyStoneBricks:
    case world::Block::ChiseledStoneBricks:
    case world::Block::Granite:
    case world::Block::Diorite:
    case world::Block::Andesite:
    case world::Block::PolishedGranite:
    case world::Block::PolishedDiorite:
    case world::Block::PolishedAndesite:
    case world::Block::SmoothStone:
    case world::Block::CoalOre:
        return {ToolType::Pickaxe, ToolTier::Wood};
    case world::Block::IronOre:
    case world::Block::LapisOre:
        return {ToolType::Pickaxe, ToolTier::Stone};
    case world::Block::GoldOre:
    case world::Block::DiamondOre:
    case world::Block::EmeraldOre:
    case world::Block::RedstoneOre:
        return {ToolType::Pickaxe, ToolTier::Iron};
    case world::Block::Obsidian:
        return {ToolType::Pickaxe, ToolTier::Diamond};
    default:
        // Wood (the mineable/axe tag), dirt, gravel, wool, glass, plants: no tool
        // requirement whatsoever. Unlike stone and ore, vanilla wood never calls
        // requiresCorrectToolForDrops, so a bare hand still keeps the block.
        return {};
    }
}

// The tool roles that mine a given block faster. Each list mirrors the vanilla
// mineable tag: pickaxe → the stone/ore family, shovel → dirt/sand/gravel, hoe →
// leaves, axe → wood. This is a separate concern from harvestRequirement above,
// which only gates the blocks marked requiresCorrectToolForDrops: a pickaxe is
// still the fast tool for sandstone, bricks, quartz, netherrack and furnaces
// even where a bare hand keeps their loot. The lit furnace shares the plain
// furnace's entry, the way its blockstate shares one block in vanilla.
[[nodiscard]] bool isPickaxeBlock(world::Block block) {
    using enum world::Block;
    switch (block) {
    case Stone:
    case Cobblestone:
    case MossyCobblestone:
    case StoneBricks:
    case MossyStoneBricks:
    case ChiseledStoneBricks:
    case Granite:
    case Diorite:
    case Andesite:
    case PolishedGranite:
    case PolishedDiorite:
    case PolishedAndesite:
    case SmoothStone:
    case Sandstone:
    case Bricks:
    case QuartzBlock:
    case Netherrack:
    case Furnace:
    case CoalOre:
    case IronOre:
    case LapisOre:
    case GoldOre:
    case DiamondOre:
    case EmeraldOre:
    case RedstoneOre:
    case Obsidian:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool isAxeBlock(world::Block block) {
    using enum world::Block;
    switch (block) {
    case OakLog:
    case SpruceLog:
    case BirchLog:
    case JungleLog:
    case AcaciaLog:
    case DarkOakLog:
    case OakPlanks:
    case SprucePlanks:
    case BirchPlanks:
    case JunglePlanks:
    case AcaciaPlanks:
    case DarkOakPlanks:
    case Bookshelf:
    case CraftingTable:
    case Chest:
    case Pumpkin:
    case Melon:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool isShovelBlock(world::Block block) {
    using enum world::Block;
    switch (block) {
    case Grass:
    case Dirt:
    case CoarseDirt:
    case Podzol:
    case Sand:
    case RedSand:
    case Gravel:
    case Clay:
    case SnowBlock:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool isHoeBlock(world::Block block) {
    return world::isLeaves(block);
}

bool canHarvestBlock(world::Block block, const ItemStack& tool) {
    const auto requirement = harvestRequirement(block);
    if (requirement.tool == ToolType::None) return true;
    if (toolType(tool) != requirement.tool) return false;
    const auto& held = toolAttributes(toolType(tool), toolTier(tool));
    const auto& needed = toolAttributes(requirement.tool, requirement.tier);
    return held.harvestLevel >= needed.harvestLevel;
}

float miningSeconds(
    world::Block block, const ItemStack& tool, bool underwater, bool airborne) {
    const float hardness = world::blockDefinition(block).hardness;
    // AbstractBlock#calcBlockBreakingDelta returns 0 for hardness -1, which never
    // accumulates any progress: the block simply cannot be mined.
    if (hardness < 0.0F) return std::numeric_limits<float>::infinity();
    float speed = 1.0F;
    // A tool only pays off on the blocks it is the right tool for.
    const auto& attributes = toolAttributes(toolType(tool), toolTier(tool));
    switch (toolType(tool)) {
    case ToolType::Pickaxe:
        if (isPickaxeBlock(block)) speed = attributes.miningSpeed;
        break;
    case ToolType::Axe:
        if (isAxeBlock(block)) speed = attributes.miningSpeed;
        break;
    case ToolType::Shovel:
        if (isShovelBlock(block)) speed = attributes.miningSpeed;
        break;
    case ToolType::Hoe:
        if (isHoeBlock(block)) speed = attributes.miningSpeed;
        break;
    default:
        break;
    }
    if (underwater) speed /= 5.0F;
    if (airborne) speed /= 5.0F;
    const float divisor = canHarvestBlock(block, tool) ? 30.0F : 100.0F;
    // Vanilla accumulates speed / hardness / divisor per tick and breaks the block
    // once the total reaches 1. A hardness of 0 makes that delta infinite, so the
    // very tick the swing starts already finishes the block: grass, torches and the
    // like pop without ever showing a destroy stage.
    const float perTickDelta = speed / hardness / divisor;
    if (!(perTickDelta < 1.0F)) return 0.0F;
    return hardness * divisor / speed / 20.0F;
}

void MinedDrops::add(const ItemStack& stack) {
    if (stack.empty() || count >= kMaximumEntries) return;
    entries[count] = stack;
    ++count;
}

MinedDrops minedDrops(world::Block block, const ItemStack& tool, std::uint32_t& randomState,
                      int age) {
    MinedDrops drops;
    // Breaking a block with too weak a tool destroys it without any loot.
    if (!canHarvestBlock(block, tool)) return drops;
    switch (block) {
    // Blocks that turn into something else on the way out.
    case world::Block::Stone:
        drops.add({world::Block::Cobblestone, 1U, blockItemFor(world::Block::Cobblestone)});
        break;
    case world::Block::Grass:
    case world::Block::Podzol:
        // Only silk touch keeps the topsoil layer; everything else yields dirt.
        drops.add({world::Block::Dirt, 1U, blockItemFor(world::Block::Dirt)});
        break;
    case world::Block::CoalOre:
        drops.add({world::Block::Air, 1U, &items::Coal});
        break;
    case world::Block::DiamondOre:
        drops.add({world::Block::Air, 1U, &items::Diamond});
        break;
    case world::Block::EmeraldOre:
        drops.add({world::Block::Air, 1U, &items::Emerald});
        break;
    case world::Block::Bookshelf:
        drops.add({world::Block::Air, 3U, &items::Book});
        break;
    case world::Block::WallTorch:
        // Whatever it was leaning on, a wall torch comes back as the standing
        // one: the facing is a state, and the item has no facing at all.
        drops.add({world::Block::Torch, 1U, blockItemFor(world::Block::Torch)});
        break;

    // Chance-based tables.
    case world::Block::OakLeaves:
    case world::Block::SpruceLeaves:
    case world::Block::BirchLeaves:
    case world::Block::JungleLeaves:
    case world::Block::AcaciaLeaves:
    case world::Block::DarkOakLeaves:
        // Without shears or silk touch the leaves themselves are lost; what is
        // left are the rolls of the vanilla leaves tables. Jungle leaves drop
        // their sapling at 1/40 rather than 1/20, and only oak and dark oak
        // carry the apple roll.
        if (rollChance(randomState,
                       block == world::Block::JungleLeaves ? 0.025F : 0.05F)) {
            drops.add({saplingForLeaves(block), 1U, blockItemFor(saplingForLeaves(block))});
        }
        if (rollChance(randomState, 0.02F)) {
            drops.add({world::Block::Air, randomCount(randomState, 1U, 2U), &items::Stick});
        }
        if ((block == world::Block::OakLeaves || block == world::Block::DarkOakLeaves) &&
            rollChance(randomState, 0.005F)) {
            drops.add({world::Block::Air, 1U, &items::Apple});
        }
        break;
    case world::Block::Gravel:
        // 10% flint, and the gravel itself only when that roll fails.
        if (rollChance(randomState, 0.10F)) {
            drops.add({world::Block::Air, 1U, &items::Flint});
        } else {
            drops.add({world::Block::Gravel, 1U, blockItemFor(world::Block::Gravel)});
        }
        break;

    case world::Block::GrassPlant:
        // Tall grass drops a wheat seed 1/8 of the time (1.16.1's grass.json
        // loot table); the grass plant itself is only kept by shears.
        if (rollChance(randomState, 0.125F)) {
            drops.add({world::Block::Air, 1U, &items::WheatSeeds});
        }
        break;
    case world::Block::Farmland:
        // FarmlandBlock#getDrops: breaking tilled soil always yields dirt.
        drops.add({world::Block::Dirt, 1U, blockItemFor(world::Block::Dirt)});
        break;

    case world::Block::WheatCrops: {
        // Wheat's loot table: at age 7 the guaranteed pool drops wheat and an
        // extra binomial(3, 0.5714) roll of seeds; an immature crop drops a
        // single seed instead.
        const bool mature = age >= 7;
        drops.add({world::Block::Air, 1U, mature ? produceForCrop(block) : seedForCrop(block)});
        if (mature) {
            const auto seeds = binomialCount(randomState, 3, 0.5714286F);
            if (seeds > 0U) {
                drops.add({world::Block::Air, seeds, seedForCrop(block)});
            }
        }
        break;
    }
    case world::Block::Carrots:
    case world::Block::Potatoes: {
        // Carrot/potato share a table: one crop unconditionally (so even a young
        // plant yields one), plus a binomial(3, 0.5714) extra roll at maturity.
        std::uint8_t count = 1U;
        if (age >= 7) {
            count = static_cast<std::uint8_t>(count + binomialCount(randomState, 3, 0.5714286F));
        }
        drops.add({world::Block::Air, count, produceForCrop(block)});
        break;
    }

    // Silk-touch-only blocks: harvestable, but they leave nothing behind.
    case world::Block::Glass:
        break;

    default:
        // dropsItem marks the blocks whose loot is simply themselves; the rest
        // (tall grass, which would need seeds) drop nothing.
        if (world::blockDefinition(block).dropsItem) {
            drops.add({block, 1U, blockItemFor(block)});
        }
        break;
    }
    return drops;
}

} // namespace mc::gameplay

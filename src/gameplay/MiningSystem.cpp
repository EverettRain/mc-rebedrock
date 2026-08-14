#include "gameplay/MiningSystem.hpp"

#include "gameplay/BlockTags.hpp"
#include "gameplay/ItemPlacement.hpp"

#include <limits>
#include <optional>

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

// Which tool a block wants, straight off the `mineable/*` tags.
[[nodiscard]] std::optional<ToolType> mineableTool(world::Block block) {
    const auto& tags = blockTags();
    if (tags.has(block, BlockTag::MineableWithPickaxe)) return ToolType::Pickaxe;
    if (tags.has(block, BlockTag::MineableWithAxe)) return ToolType::Axe;
    if (tags.has(block, BlockTag::MineableWithShovel)) return ToolType::Shovel;
    if (tags.has(block, BlockTag::MineableWithHoe)) return ToolType::Hoe;
    return std::nullopt;
}

HarvestRequirement harvestRequirement(world::Block block) {
    // 26.1 splits this across two independent tag families, and so does this.
    //
    // `mineable/*` says which tool digs the block faster. `needs_*_tool` says
    // which tier keeps its drop. The gate itself — BlockBehaviour's
    // requiresCorrectToolForDrops — is neither: in vanilla it is a block
    // property, expressed in the loot table's `match_tool` condition. Until
    // B2''s loot half reads those tables, `mineable/pickaxe` stands in for it,
    // which is exact for the current block set: every pickaxe-mineable block
    // here is one vanilla marks requiresCorrectToolForDrops, and no axe-,
    // shovel- or hoe-mineable one is. Wood, dirt and leaves therefore keep
    // dropping for a bare hand, as they must.
    const auto& tags = blockTags();
    if (!tags.has(block, BlockTag::MineableWithPickaxe)) {
        return {};
    }
    if (tags.has(block, BlockTag::NeedsDiamondTool)) return {ToolType::Pickaxe, ToolTier::Diamond};
    if (tags.has(block, BlockTag::NeedsIronTool)) return {ToolType::Pickaxe, ToolTier::Iron};
    if (tags.has(block, BlockTag::NeedsStoneTool)) return {ToolType::Pickaxe, ToolTier::Stone};
    return {ToolType::Pickaxe, ToolTier::Wood};
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
    // A tool only pays off on the blocks its own `mineable` tag lists — one bit
    // test instead of the four case lists this replaces.
    const auto& attributes = toolAttributes(toolType(tool), toolTier(tool));
    if (const auto wanted = mineableTool(block);
        wanted.has_value() && *wanted == toolType(tool)) {
        speed = attributes.miningSpeed;
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

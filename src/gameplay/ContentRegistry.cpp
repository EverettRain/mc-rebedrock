#include "gameplay/ContentRegistry.hpp"

#include <array>

namespace mc::gameplay {

bool ContentRegistry::registerBlock(world::Block blockValue, CreativeCategory category) {
    if (!world::isValidBlock(blockValue) || blockValue == world::Block::Air ||
        category == CreativeCategory::Count) return false;
    const auto definition = world::blockDefinition(blockValue);
    // The registry key is the namespaced identifier, "rebedrock:stone".
    auto identifier = definition.identifier.toString();
    if (blockIdentifiers_.contains(identifier)) return false;
    blockIdentifiers_.emplace(std::move(identifier), blocks_.size());
    blocks_.push_back({blockValue, definition, category});
    // The catalog stack is wielded as the block's own BlockItem, the way 1.16.1
    // registers each block into its Items registry.
    const ItemStack stack{blockValue, 1U, blockItemFor(blockValue)};
    catalogs_[static_cast<std::size_t>(category)].push_back(stack);
    allCatalog_.push_back(stack);
    blockCatalog_.push_back(stack);
    return true;
}

bool ContentRegistry::registerItem(const Item* itemValue, CreativeCategory category) {
    if (itemValue == nullptr || category == CreativeCategory::Count) return false;
    auto identifier = itemValue->identifier.toString();
    if (itemIdentifiers_.contains(identifier)) return false;
    itemIdentifiers_.emplace(std::move(identifier), items_.size());
    items_.push_back({itemValue, category});
    const ItemStack stack{world::Block::Air, 1U, itemValue};
    catalogs_[static_cast<std::size_t>(category)].push_back(stack);
    allCatalog_.push_back(stack);
    itemCatalog_.push_back(stack);
    return true;
}

const RegisteredBlock* ContentRegistry::block(std::string_view identifier) const {
    auto found = blockIdentifiers_.find(std::string{identifier});
    if (found == blockIdentifiers_.end()) {
        // Vanilla aliases and bare names resolve through the block registry, so
        // "minecraft:chest" and "chest" both find `rebedrock:chest`.
        const auto resolved = world::blockFromIdentifier(identifier);
        if (!resolved.has_value()) return nullptr;
        found = blockIdentifiers_.find(world::blockDefinition(*resolved).identifier.toString());
        if (found == blockIdentifiers_.end()) return nullptr;
    }
    return &blocks_[found->second];
}

const RegisteredItem* ContentRegistry::item(std::string_view identifier) const {
    auto found = itemIdentifiers_.find(std::string{identifier});
    if (found == itemIdentifiers_.end()) {
        // Vanilla aliases and bare names resolve through the item registry.
        const auto* resolved = itemFromIdentifier(identifier);
        if (resolved == nullptr) return nullptr;
        found = itemIdentifiers_.find(resolved->identifier.toString());
        if (found == itemIdentifiers_.end()) return nullptr;
    }
    return &items_[found->second];
}

std::span<const ItemStack> ContentRegistry::catalog(CreativeCategory category) const {
    if (category == CreativeCategory::Count) return {};
    return catalogs_[static_cast<std::size_t>(category)];
}

const ContentRegistry& contentRegistry() {
    static const ContentRegistry registry = [] {
        ContentRegistry result;
        constexpr std::array building{
            world::Block::Grass, world::Block::Dirt, world::Block::Stone,
            world::Block::Cobblestone, world::Block::OakPlanks,
            world::Block::SprucePlanks, world::Block::BirchPlanks,
            world::Block::JunglePlanks, world::Block::AcaciaPlanks,
            world::Block::DarkOakPlanks,
            world::Block::OakLog, world::Block::SpruceLog, world::Block::BirchLog,
            world::Block::JungleLog, world::Block::AcaciaLog,
            world::Block::DarkOakLog,
            world::Block::Bricks, world::Block::StoneBricks,
            world::Block::MossyStoneBricks, world::Block::ChiseledStoneBricks,
            world::Block::MossyCobblestone, world::Block::Sand,
            world::Block::Gravel, world::Block::Sandstone, world::Block::QuartzBlock,
            world::Block::Obsidian, world::Block::Clay, world::Block::SnowBlock,
            world::Block::Netherrack, world::Block::Granite, world::Block::Diorite,
            world::Block::Andesite, world::Block::PolishedGranite,
            world::Block::PolishedDiorite, world::Block::PolishedAndesite,
            world::Block::SmoothStone,
            world::Block::CoarseDirt, world::Block::Podzol,
            world::Block::RedSand, world::Block::WhiteWool, world::Block::RedWool,
            world::Block::BlackWool, world::Block::Bedrock,
            world::Block::OakSlab, world::Block::SpruceSlab, world::Block::BirchSlab,
            world::Block::JungleSlab, world::Block::AcaciaSlab, world::Block::DarkOakSlab,
            world::Block::StoneSlab, world::Block::CobblestoneSlab,
            world::Block::StoneBrickSlab, world::Block::SmoothStoneSlab,
        };
        constexpr std::array decoration{
            world::Block::Glass, world::Block::OakLeaves, world::Block::SpruceLeaves,
            world::Block::BirchLeaves, world::Block::JungleLeaves,
            world::Block::AcaciaLeaves, world::Block::DarkOakLeaves,
            world::Block::Bookshelf, world::Block::Pumpkin, world::Block::Melon,
            world::Block::GrassPlant, world::Block::Dandelion, world::Block::OakSapling,
            world::Block::SpruceSapling, world::Block::BirchSapling,
            world::Block::JungleSapling, world::Block::AcaciaSapling,
            world::Block::DarkOakSapling,
        };
        constexpr std::array functional{
            world::Block::CraftingTable, world::Block::Furnace, world::Block::Chest,
            world::Block::Glowstone, world::Block::Tnt, world::Block::Torch,
            world::Block::CoalOre, world::Block::IronOre, world::Block::GoldOre,
            world::Block::DiamondOre, world::Block::LapisOre,
            world::Block::RedstoneOre, world::Block::EmeraldOre,
        };
        for (const auto block : building)
            result.registerBlock(block, CreativeCategory::BuildingBlocks);
        for (const auto block : decoration)
            result.registerBlock(block, CreativeCategory::Decoration);
        for (const auto block : functional)
            result.registerBlock(block, CreativeCategory::Functional);

        // Every registered item declares its own creative tab, so registration
        // is a single pass over the registry in declaration order. Spawn eggs
        // live in a separate array (kSpawnEggItems) and are registered by
        // registerSpawnEggItems() at app startup — they need entity headers
        // that cannot be included here without dragging entity sources into
        // every test that links ContentRegistry.cpp.
        for (const Item* item : kItemRegistry)
            result.registerItem(item, item->creativeCategory);
        return result;
    }();
    return registry;
}

} // namespace mc::gameplay

#pragma once

#include "gameplay/Inventory.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace mc::gameplay {

struct RegisteredBlock final {
    world::Block value = world::Block::Air;
    world::BlockDefinition definition = world::blockDefinition(world::Block::Air);
    CreativeCategory category = CreativeCategory::BuildingBlocks;
};

struct RegisteredItem final {
    const Item* value = nullptr;
    CreativeCategory category = CreativeCategory::Ingredients;
};

class ContentRegistry final {
  public:
    bool registerBlock(world::Block block, CreativeCategory category);
    bool registerItem(const Item* item, CreativeCategory category);

    [[nodiscard]] const RegisteredBlock* block(std::string_view identifier) const;
    [[nodiscard]] const RegisteredItem* item(std::string_view identifier) const;
    [[nodiscard]] std::span<const RegisteredBlock> blocks() const { return blocks_; }
    [[nodiscard]] std::span<const RegisteredItem> items() const { return items_; }
    [[nodiscard]] std::span<const ItemStack> catalog(CreativeCategory category) const;
    [[nodiscard]] std::span<const ItemStack> allCatalog() const { return allCatalog_; }
    [[nodiscard]] std::span<const ItemStack> blockCatalog() const { return blockCatalog_; }
    [[nodiscard]] std::span<const ItemStack> itemCatalog() const { return itemCatalog_; }

  private:
    static constexpr std::size_t kCategoryCount =
        static_cast<std::size_t>(CreativeCategory::Count);
    std::vector<RegisteredBlock> blocks_;
    std::vector<RegisteredItem> items_;
    std::unordered_map<std::string, std::size_t> blockIdentifiers_;
    // The item catalog is a view over the ItemRegistry: identity (name -> Item)
    // resolves through that registry, and this map only records which registered
    // items are catalogued here, keyed by the resolved Item pointer. No parallel
    // name -> item map — the registry is the single item-identity source.
    std::unordered_map<const Item*, std::size_t> itemIndex_;
    std::array<std::vector<ItemStack>, kCategoryCount> catalogs_;
    std::vector<ItemStack> allCatalog_;
    std::vector<ItemStack> blockCatalog_;
    std::vector<ItemStack> itemCatalog_;
};

[[nodiscard]] const ContentRegistry& contentRegistry();

} // namespace mc::gameplay

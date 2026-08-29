#include "gameplay/ContentRegistry.hpp"

#include "gameplay/ItemRegistry.hpp"
#include "world/BlockRegistry.hpp"

namespace mc::gameplay {

bool ContentRegistry::registerBlock(world::Block blockValue, CreativeCategory category) {
    // AR-CI: Hidden is the sentinel a technical/unobtainable block's definition
    // carries by default (Air among them) — reject it the same way Count is
    // rejected, so no caller can accidentally list a hidden block by passing an
    // explicit category that disagrees with the block's own declaration.
    if (!world::isValidBlock(blockValue) || blockValue == world::Block::Air ||
        category == CreativeCategory::Count || category == CreativeCategory::Hidden) {
        return false;
    }
    // Identity comes from the block registry, not a parallel copy: this catalog is
    // a creative-tab *view* over the registry, so the definition it stores is the
    // one the registry froze for this block's BlockId.
    const auto& definition = world::blockRegistry().get(world::blockId(blockValue));
    // The registry key is the namespaced identifier, "rebedrock:stone".
    auto identifier = definition.identifier.toString();
    if (blockIdentifiers_.contains(identifier)) return false;
    blockIdentifiers_.emplace(std::move(identifier), blocks_.size());
    blocks_.push_back({blockValue, definition, category});
    // The catalog stack is wielded as the block's own BlockItem, the way vanilla
    // registers each block into its Items registry.
    const ItemStack stack{blockValue, 1U, blockItemFor(blockValue)};
    catalogs_[static_cast<std::size_t>(category)].push_back(stack);
    allCatalog_.push_back(stack);
    blockCatalog_.push_back(stack);
    return true;
}

bool ContentRegistry::registerItem(const Item* itemValue, CreativeCategory category) {
    if (itemValue == nullptr || category == CreativeCategory::Count ||
        category == CreativeCategory::Hidden) {
        return false;
    }
    // Catalog membership is keyed by the Item itself; the ItemRegistry owns the
    // name -> Item mapping this view resolves through.
    if (itemIndex_.contains(itemValue)) return false;
    itemIndex_.emplace(itemValue, items_.size());
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
        // "minecraft:chest" and "chest" both find `rebedrock:chest`. The registry
        // is the identity source; this catalog keys on the canonical name it froze.
        const auto id = world::blockRegistry().byName(identifier);
        if (!id.valid()) return nullptr;
        found = blockIdentifiers_.find(world::blockRegistry().identifier(id).toString());
        if (found == blockIdentifiers_.end()) return nullptr;
    }
    return &blocks_[found->second];
}

const RegisteredItem* ContentRegistry::item(std::string_view identifier) const {
    // Resolve identity through the ItemRegistry (the single item-identity source);
    // this catalog is a view keyed by the resolved Item pointer. A name that
    // resolves to an item outside this catalog (e.g. a block wielded as its
    // BlockItem, which lives in the block catalog) is not listed here.
    const Item* resolved = itemFromIdentifier(identifier);
    if (resolved == nullptr) return nullptr;
    const auto found = itemIndex_.find(resolved);
    if (found == itemIndex_.end()) return nullptr;
    return &items_[found->second];
}

std::span<const ItemStack> ContentRegistry::catalog(CreativeCategory category) const {
    // Count is the array-size sentinel and Hidden sits deliberately outside
    // [0, Count) (see core/CreativeCategory.hpp) — neither indexes a real slot.
    if (category == CreativeCategory::Count || category == CreativeCategory::Hidden) return {};
    return catalogs_[static_cast<std::size_t>(category)];
}

const ContentRegistry& contentRegistry() {
    static const ContentRegistry registry = [] {
        ContentRegistry result;
        // AR-CI: block catalog membership is data-driven off each block's own
        // BlockDefinition::creativeCategory (declared via BlockProperties::
        // creative() in Block.hpp) rather than a parallel hand-maintained list.
        // A single pass over the block registry, in registry (BlockId) order,
        // is both the entire registration and the reachability guarantee: a
        // block that never declares a tab defaults to Hidden and registerBlock
        // rejects it, and a block that does declare one is *automatically*
        // catalogued the moment it lands — there is no second place a future
        // block (a stair, a redstone component, ...) needs to be listed, and so
        // no way for new content to silently drift out of creative again.
        const auto& blocks = world::blockRegistry();
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            const core::BlockId id = core::BlockId::of(static_cast<core::BlockId::Value>(index));
            const world::BlockDefinition& definition = blocks.get(id);
            if (definition.creativeCategory == CreativeCategory::Hidden) continue;
            result.registerBlock(definition.block, definition.creativeCategory);
        }

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

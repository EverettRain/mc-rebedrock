#include "gameplay/LootTable.hpp"

#include "core/Json.hpp"
#include "data/LootFile.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "world/BlockRegistry.hpp"

// The baked constexpr floor of block loot tables, included once, here.
#include "gameplay/LootBakedData.inc"

#include <string>
#include <string_view>
#include <utility>

namespace mc::gameplay {
namespace {

// Resolves a drop's identifier + count to its stack: a block name yields the
// block stack (its BlockItem), an item name the item stack — the same rule the
// recipe outputs use. Returns false for an identifier this build has no content
// for, so an overlay drop naming missing content is dropped rather than resolved
// to a hole.
[[nodiscard]] bool resolveDrop(const data::LootDropDef& drop, ItemStack& out) {
    if (const auto block = world::blockFromIdentifier(drop.id); block.has_value()) {
        out = ItemStack{*block, drop.count, blockItemFor(*block)};
        return true;
    }
    if (const Item* item = itemFromIdentifier(drop.id); item != nullptr) {
        if (const BlockItem* blockItem = asBlockItem(item); blockItem != nullptr) {
            out = ItemStack{blockItem->block(), drop.count, item};
        } else {
            out = ItemStack{world::Block::Air, drop.count, item};
        }
        return true;
    }
    return false;
}

// Resolves a whole table's drops. Returns false if any drop names missing
// content, so the caller can skip the table rather than install a partial one.
[[nodiscard]] bool resolveEntry(const data::LootTableDef& def, LootEntry& out) {
    LootEntry entry;
    entry.stacks.reserve(def.drops.size());
    for (const auto& drop : def.drops) {
        ItemStack stack;
        if (!resolveDrop(drop, stack)) {
            return false;
        }
        entry.stacks.push_back(stack);
    }
    out = std::move(entry);
    return true;
}

// `loot_tables/blocks/oak_planks.json` -> `oak_planks`, the block name half.
[[nodiscard]] std::string_view blockNameFromPath(std::string_view path, std::string_view prefix) {
    if (path.size() >= prefix.size() && path.substr(0, prefix.size()) == prefix) {
        path.remove_prefix(prefix.size());
        if (!path.empty() && path.front() == '/') {
            path.remove_prefix(1U);
        }
    }
    if (path.size() >= 5U && path.substr(path.size() - 5U) == ".json") {
        path.remove_suffix(5U);
    }
    return path;
}

} // namespace

void LootTable::set(world::Block block, LootEntry entry) {
    const auto index = world::blockId(block).index();
    if (index >= entries_.size()) {
        entries_.resize(index + 1U);
    }
    entries_[index] = std::move(entry);
}

void LootTable::loadBuiltinDefaults() {
    entries_.assign(world::blockCount(), std::nullopt);
    for (const auto& baked : data::loot::kBakedLootTables) {
        const auto block = world::blockFromIdentifier(baked.block);
        if (!block.has_value()) {
            continue; // a baked entry for a block this build lacks: skip
        }
        LootEntry entry;
        if (resolveEntry(data::loot::toDef(baked), entry)) {
            set(*block, std::move(entry));
        }
    }
}

void LootTable::load(const assets::ResourceProvider& resources) {
    loadBuiltinDefaults();
    applyOverlay(resources);
}

void LootTable::applyOverlay(const assets::ResourceProvider& resources) {
    for (const auto& location : resources.list("minecraft", "loot_tables/blocks")) {
        const auto bytes = resources.readBytes(location);
        if (bytes.empty()) {
            continue;
        }
        // The block a file drops for comes from its path, the way vanilla names a
        // block's loot table `loot_tables/blocks/<block>.json`.
        const std::string_view name = blockNameFromPath(location.path, "loot_tables/blocks");
        const std::string identifier = location.space + ":" + std::string{name};
        const auto block = world::blockFromIdentifier(identifier);
        if (!block.has_value()) {
            continue; // an overlay table for a block this build lacks: skip
        }
        core::Json root;
        try {
            root = core::Json::parse(std::string_view{
                reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        } catch (const std::exception&) {
            continue; // a malformed table must not take the rest of the pack down
        }
        data::LootTableDef def;
        if (!data::Codec<data::LootTableDef>::read(root, def)) {
            continue;
        }
        LootEntry entry;
        if (resolveEntry(def, entry)) {
            set(*block, std::move(entry));
        }
    }
}

const LootEntry* LootTable::find(world::Block block) const {
    const auto index = world::blockId(block).index();
    if (index >= entries_.size() || !entries_[index].has_value()) {
        return nullptr;
    }
    return &*entries_[index];
}

LootTable& lootTable() {
    static LootTable table = [] {
        LootTable defaults;
        defaults.loadBuiltinDefaults();
        return defaults;
    }();
    return table;
}

} // namespace mc::gameplay

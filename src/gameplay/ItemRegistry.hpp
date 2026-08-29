#pragma once

// The runtime item-identity registry, the item-side counterpart of
// world/BlockRegistry.hpp. It pours the compile-time item tables (kItemRegistry
// and the runtime-extensible spawn-egg slot) into a generic core::Registry, so a
// held item is referenced by a dense ItemId and resolved to its Item by one
// subscript — the DOD "holder = id" rule, the same one BlockId already follows.
//
// Identity, not storage: an ItemStack still points at the constexpr Item (its
// icon, food value and tool attributes read straight off it), and saves still
// persist an item by its `minecraft:` name, never a raw id. What the registry
// adds is a single, authoritative name -> id -> Item mapping with a three-phase
// lifecycle (built-in items in Bootstrap, spawn eggs and future datapack items
// in External, then Freeze), so "add an item" becomes "register a definition",
// not "extend a hand-rolled scan". itemFromIdentifier below is now a view over
// this registry.
//
// Block items are deliberately *not* registry entries. A block wielded as its
// BlockItem draws its identity from the block registry (which is R0 identity
// too), and some blocks — the wheat crop shares the `wheat` name with the wheat
// item — have no item form at all, exactly as vanilla's Items registry omits
// them. itemFromIdentifier bridges to blockItemFor for the block names that name
// no real item, keeping `Items.STONE`-style lookups working without minting a
// colliding ItemId for every block.

#include "core/ContentId.hpp"
#include "core/Registry.hpp"
#include "gameplay/Item.hpp"
#include "world/Block.hpp"

#include <span>
#include <string_view>

namespace mc::gameplay {

// The registry maps ItemId -> the constexpr Item it names. The definition is the
// Item pointer (stable, since items are static globals or the runtime slot's
// long-lived entries): deref is one subscript to the pointer, then follow it.
using ItemRegistry = core::Registry<const Item*, core::ItemId>;

// Runs the three-phase lifecycle once: register every built-in item (Bootstrap),
// open External and register the runtime-slot items (spawn eggs, and later any
// datapack item), then freeze. Built-ins go first so their ids are stable
// regardless of what external content a world carries — the same guarantee
// buildBlockRegistry gives blocks. Extracted from itemRegistry() so a test can
// drive the lifecycle with its own external content without the process
// singleton. Each item's `minecraft:` alias is filed beside its `rebedrock:`
// key, so a vanilla name and a vanilla save both resolve to the same id.
[[nodiscard]] inline ItemRegistry buildItemRegistry(std::span<const Item* const> external) {
    ItemRegistry built;
    // Files the item's `minecraft:` alias beside the `rebedrock:` key just
    // registered, when the two differ (custom content has no alias).
    const auto aliasVanilla = [&built](const Item* item, core::ItemId id) {
        if (!item->vanillaAlias.empty() && item->vanillaAlias != item->identifier) {
            built.alias(item->vanillaAlias, id);
        }
    };
    for (const Item* item : kItemRegistry) {
        aliasVanilla(item, built.registerBuiltin(item->identifier, item));
    }
    built.beginExternal();
    for (const Item* item : external) {
        aliasVanilla(item, built.registerExternal(item->identifier, item));
    }
    built.freeze();
    return built;
}

// Builds and freezes the registry once, on first use, from the built-in table
// and whatever the spawn-egg slot has collected by then (populated at static-init
// time by SpawnEggItems.hpp, before the first gameplay use — the same ordering
// buildBlockRegistry relies on for externalBlockDefs).
[[nodiscard]] inline const ItemRegistry& itemRegistry() {
    static const ItemRegistry registry = buildItemRegistry(extraItemRegistry());
    return registry;
}

// The ItemId a registered item was assigned, or an invalid id for a null pointer
// or an item that never registered (a bare custom Item in a test). Reverse of
// get(): the item's identifier is always namespaced, so byName is an O(1)
// interner hit, not a scan.
[[nodiscard]] inline core::ItemId itemId(const Item* item) {
    if (item == nullptr) return core::ItemId::invalid();
    return itemRegistry().byName(item->identifier);
}

// The item an id names. Aborts on an id this registry never handed out, the same
// as every other registry get() — a bad id is a bug, not a miss.
[[nodiscard]] inline const Item* itemFromId(core::ItemId id) {
    return itemRegistry().get(id);
}

// Resolves a registry key to its item. Accepts `rebedrock:book`, the vanilla
// alias `minecraft:book`, and the bare `book`, through the registry's own name
// resolution. A name that no real item claims falls through to the block bridge:
// every block is also wielded as its BlockItem (vanilla's `Items.STONE` beside
// `Blocks.STONE`), and that identity comes from the block registry. Returns
// nullptr when nothing owns the name.
[[nodiscard]] inline const Item* itemFromIdentifier(std::string_view text) {
    if (const core::ItemId id = itemRegistry().byName(text); id.valid()) {
        return itemFromId(id);
    }
    if (const auto block = world::blockFromIdentifier(text); block.has_value()) {
        return blockItemFor(*block);
    }
    return nullptr;
}

} // namespace mc::gameplay

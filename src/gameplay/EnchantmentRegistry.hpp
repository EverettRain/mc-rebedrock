#pragma once

// DDC-0: the runtime enchantment-identity registry — the enchantment-side
// counterpart of gameplay/ItemRegistry.hpp and gameplay/StatusEffect.cpp's
// StatusEffectRegistry. It pours the compile-time enchantment table
// (kEnchantmentTable, still the baked source of truth in Enchantment.hpp) into a
// generic core::Registry, so an enchantment is referenced by a dense
// EnchantmentTypeId and resolved to its EnchantmentDefinition by one subscript —
// the DOD "holder = id" rule the block and item registries already follow.
//
// Why this exists (the DDC-0 foundation): Enchantment.hpp defines the built-in
// content as a constexpr table (the *baked default*, mandatory so a build with
// no datapack still runs), but until now nothing turned that table into a
// *runtime* registry with an External phase a datapack could register into.
// This file is that missing layer: it registers every baked default in the
// Bootstrap phase (ids assigned in EnchantmentId enum order, so the runtime
// dense id equals the stored enum ordinal — a save's ordinal indexes the table
// directly), opens an External slot for datapack/mod enchantments (populated
// through buildEnchantmentRegistry's `external` argument or the DataStore bridge
// below), then freezes. DDC-1's JSON loader and DDC-3's content migration hang
// their externally-defined enchantments on that slot; the constexpr table stays
// the fallback nothing can remove.
//
// It does NOT replace Enchantment.hpp's constexpr accessors (enchantmentMaxLevel
// etc. stay constexpr for the compile-time and hot-path callers). This is an
// additive identity layer: baked default underneath, a runtime registry with a
// datapack override slot on top — exactly the two-layer shape DDC calls for.

#include "core/ContentId.hpp"
#include "core/Identifier.hpp"
#include "core/Registry.hpp"
#include "data/DataStore.hpp"
#include "gameplay/Enchantment.hpp"

#include <span>
#include <string_view>

namespace mc::gameplay {

// The registry maps EnchantmentTypeId -> the EnchantmentDefinition it names. The
// definition is a trivially-copyable POD (its only non-scalar field is a
// string_view into .rodata), so it is stored by value: deref is one subscript,
// no pointer chase, no allocation.
using EnchantmentRegistry = core::Registry<EnchantmentDefinition, core::EnchantmentTypeId>;

// Registers one enchantment under `rebedrock:<name>` with a `minecraft:<name>`
// alias, so 1.16.1 data and vanilla names resolve to the same id. Kept as a free
// helper so both the Bootstrap and External passes file the alias identically.
inline core::EnchantmentTypeId registerEnchantment(EnchantmentRegistry& registry,
                                                   const EnchantmentDefinition& def,
                                                   bool external) {
    const core::Identifier key{core::kNamespace, def.vanillaName};
    const core::EnchantmentTypeId id =
        external ? registry.registerExternal(key, def) : registry.registerBuiltin(key, def);
    // The vanilla name matches the built-in path for every baked entry; a
    // datapack enchantment under some other namespace has no alias to file.
    if (def.vanillaName == key.path) {
        registry.alias(core::Identifier{core::kVanillaNamespace, def.vanillaName}, id);
    }
    return id;
}

// Runs the three-phase lifecycle once: register every baked default (Bootstrap),
// open External and register whatever external content the caller supplies, then
// freeze. Built-ins go first and in EnchantmentId enum order, so their dense ids
// are stable across runs and equal the enum ordinal an ItemStack stores. This is
// the test-drivable form, mirroring buildItemRegistry: a test drives the
// lifecycle with its own external content without the process singleton.
[[nodiscard]] inline EnchantmentRegistry buildEnchantmentRegistry(
    std::span<const EnchantmentDefinition> external) {
    EnchantmentRegistry registry;
    for (const EnchantmentDefinition& def : kEnchantmentTable) {
        registerEnchantment(registry, def, /*external=*/false);
    }
    registry.beginExternal();
    for (const EnchantmentDefinition& def : external) {
        registerEnchantment(registry, def, /*external=*/true);
    }
    registry.freeze();
    return registry;
}

// Bakes the constexpr table into a DataStore as the built-in floor. This is the
// DDC-0 extension of the generic DataStore/registerExternal bridge to the
// enchantment Def type: the store carries the baked defaults, a datapack overlay
// (DDC-1's JSON, not this node) merges on top by name — replacing a built-in in
// place or appending a new key — and registerAdditionsInto() hangs only the
// *additions* on a registry's External phase, so external content claims a dense
// id beside the built-ins without disturbing baked ids. No JSON is parsed here
// (that is DDC-1); this only proves the Def type flows through the existing
// bridge, baked default underneath.
[[nodiscard]] inline data::DataStore<EnchantmentDefinition> bakedEnchantmentStore() {
    data::DataStore<EnchantmentDefinition> store;
    for (const EnchantmentDefinition& def : kEnchantmentTable) {
        store.bakeBuiltin(std::string{core::kNamespace} + ":" + std::string{def.vanillaName}, def);
    }
    return store;
}

// Builds and freezes the process registry once, on first use, from the baked
// table (no external content in DDC-0 — datapack enchantments arrive with
// DDC-1/3). A later node feeds a rebuilt store's additions here.
[[nodiscard]] inline const EnchantmentRegistry& enchantmentRegistry() {
    static const EnchantmentRegistry registry = buildEnchantmentRegistry({});
    return registry;
}

// The EnchantmentTypeId a stored EnchantmentId ordinal maps to. Because the
// baked pass registers in enum order, the runtime id equals the ordinal for
// every built-in — a save's ordinal is a valid subscript with no lookup. A
// datapack enchantment (past kEnchantmentCount) has no EnchantmentId enum value
// and is reached by name instead.
[[nodiscard]] inline core::EnchantmentTypeId enchantmentTypeId(EnchantmentId id) {
    return core::EnchantmentTypeId::of(static_cast<core::EnchantmentTypeId::Value>(id));
}

// Resolves a registry key to its type id. Accepts `rebedrock:sharpness`, the
// vanilla alias `minecraft:sharpness`, and the bare `sharpness`. An unknown name
// is an invalid id, never an abort — the save/command boundary form.
[[nodiscard]] inline core::EnchantmentTypeId enchantmentTypeByName(std::string_view text) {
    return enchantmentRegistry().byName(text);
}

// The stable `rebedrock:` name an id was registered under — what a save stores
// instead of the per-run dense id, exactly as StatusEffect and the block/item
// registries do. Empty for an invalid id.
[[nodiscard]] inline std::string_view enchantmentTypeName(core::EnchantmentTypeId id) {
    if (!id.valid()) return {};
    return enchantmentRegistry().identifier(id).path;
}

} // namespace mc::gameplay

#pragma once

// DDC-1: the enchantment content store — the two-layer DataStore for the
// JE-compatible enchantment JSON EnchantmentFile.hpp reads.
//
// This is the load-time entry point that ties DDC-1's codec to DDC-0's DataStore
// bridge. It builds a DataStore<EnchantmentDef> whose baked floor a datapack
// overlay merges onto, exactly the way recipes and loot load: a `#tag`/id set
// and cost curves are compiled once here at load into the compact EnchantmentDef
// POD, and consumers read that POD by subscript at runtime — never re-parse JSON
// on a hot path (the DOD "bake, don't parse" rule DDC's second indicator
// demands).
//
// The overlay is enumerated under `data/<space>/enchantment/`, JE 26.1's exact
// folder (`data/minecraft/enchantment/sharpness.json` ⇒ key
// `minecraft:sharpness`), so the store loads Mojang's shipped files unchanged.
// registerAdditionsInto() is DataStore's own R0 bridge (DDC-0), reused verbatim:
// an overlay-added enchantment claims a dense External id beside the built-ins.
//
// DDC-1 stays a *definition* node: it parses static fields and carries `effects`
// through as raw text. Turning EnchantmentDef into gameplay's runtime
// EnchantmentDefinition (and compiling `effects`) is DDC-2/DDC-3. This header
// therefore has no gameplay dependency — it is the data-side loader only.

#include "assets/ResourceProvider.hpp"
#include "data/DataStore.hpp"
#include "data/EnchantmentFile.hpp"

#include <cstddef>
#include <string_view>

namespace mc::data {

// JE 26.1's registry folder for enchantments (under `data/<namespace>/`).
inline constexpr std::string_view kEnchantmentPrefix = "enchantment";

// Merges the enchantment JSON overlay served by `resources` onto `store`'s baked
// floor, under the JE `enchantment/` folder. Returns how many files applied —
// zero is the no-`data/` case (the floor is the whole table). A malformed or
// wrong-shape file is skipped, never fatal (DataStore::applyOverlay's contract),
// so a JE datapack introducing an unrelated file does not take the pack down.
inline std::size_t applyEnchantmentOverlay(DataStore<EnchantmentDef>& store,
                                           const assets::ResourceProvider& resources,
                                           std::string_view space = "minecraft") {
    return store.applyOverlay(resources, space, kEnchantmentPrefix);
}

} // namespace mc::data

#pragma once

// The block-entity type identity: the single runtime source of "which kinds of
// block entity exist", hosted on the shared R0 core::Registry so block-entity
// types walk the same identity machine as blocks, items and entity types — the
// three-phase Bootstrap/External/Freeze lifecycle, the `minecraft:` alias beside
// the `rebedrock:` key, the freeze-then-abort guard, and (later) the network
// registry-sync remap.
//
// Before BE1 the `core::BlockEntityTypeId` phantom id was defined but never
// used: each block-entity kind (chest, furnace) carried its own position type,
// store, tick and save section, and a cell had no way to say which kind it hosts
// without a system testing the block identity. This header stands the identity
// up. The block->BE mapping (BlockDefinition::blockEntityType) bakes the id a
// block hosts straight from the enum ordinal below, exactly the way a block
// bakes its BlockId from its Block enum ordinal, so a placement reads the kind
// in one subscript.
//
// BE1 keeps the definition to identity only. The ticker/lifecycle behaviour
// tables (a fn-ptr `hasTicker` slot in the DOD shape of kRandomTickTable) are
// BE2's to hang here; the struct is shaped so that adding them is a field, not a
// format churn.

#include "core/ContentId.hpp"
#include "core/Identifier.hpp"
#include "core/Registry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mc::world {

using core::Identifier;
using core::kNamespace;
using core::kVanillaNamespace;

// The built-in block-entity kinds, in registration order. A built-in type's
// dense BlockEntityTypeId equals its ordinal here (buildBlockEntityTypeRegistry
// asserts it), so BlockDefinition can bake the id as a constexpr — the same
// enum-ordinal-equals-id guarantee the block registry gives BlockId.
enum class BlockEntityKind : std::uint8_t {
    Chest,
    Furnace,
    // The trapped chest's block entity (BE3). Structurally a chest — it reuses
    // the ChestSystem storage/tick/spill wholesale — but a distinct identity so
    // the trapped chest and the chest never share a container or a save section,
    // and (later) the redstone output can be tied to it.
    TrappedChest,
    Count,
};

// Everything the engine knows about one block-entity type. BE1 needs identity
// only: the `rebedrock:` key and the `minecraft:` alias behind it (so a 1.16.1
// save's `minecraft:chest` still resolves), plus a fallback name. Instances live
// in the constexpr table below, never built by hand.
struct BlockEntityTypeDefinition final {
    BlockEntityKind kind = BlockEntityKind::Chest;
    // The registry key, always in this project's namespace.
    Identifier identifier{};
    // The vanilla block entity this one mirrors; drives 1.16.1 save/asset lookups.
    Identifier vanilla{};
    // Fallback English name when a translation key is missing.
    const char* displayName = "";
};

// The compile-time table, one row per kind in enum order. Adding a built-in
// block entity is: add the enum value, add the line here, done — the registry
// asserts the ordinal/id equality on load.
inline constexpr std::array<BlockEntityTypeDefinition,
                            static_cast<std::size_t>(BlockEntityKind::Count)>
    kBlockEntityTypeRegistry{
        BlockEntityTypeDefinition{BlockEntityKind::Chest,
                                  {kNamespace, "chest"},
                                  {kVanillaNamespace, "chest"},
                                  "Chest"},
        BlockEntityTypeDefinition{BlockEntityKind::Furnace,
                                  {kNamespace, "furnace"},
                                  {kVanillaNamespace, "furnace"},
                                  "Furnace"},
        BlockEntityTypeDefinition{BlockEntityKind::TrappedChest,
                                  {kNamespace, "trapped_chest"},
                                  {kVanillaNamespace, "trapped_chest"},
                                  "Trapped Chest"},
    };

// The number of built-in block-entity types — the size a compile-time
// BE-type-keyed table cuts itself to (BE2's ticker table). Equals
// blockEntityTypeRegistry().size() for a build with no external content.
inline constexpr std::size_t kBuiltinBlockEntityTypeCount =
    static_cast<std::size_t>(BlockEntityKind::Count);

// The dense id a built-in kind derefs to: its enum ordinal, guaranteed equal to
// the registry id by the assert in buildBlockEntityTypeRegistry. A cast, not a
// lookup — cheap enough to bake into a BlockDefinition at compile time.
[[nodiscard]] constexpr core::BlockEntityTypeId blockEntityTypeId(BlockEntityKind kind) {
    return core::BlockEntityTypeId::of(static_cast<core::BlockEntityTypeId::Value>(kind));
}

// The table is indexed by the enum value, so a misplaced line would silently
// hand out another type's identity. Checked here rather than left for a save bug.
constexpr bool blockEntityTypeRegistryIsWellFormed() {
    for (std::size_t index = 0; index < kBlockEntityTypeRegistry.size(); ++index) {
        const auto& definition = kBlockEntityTypeRegistry[index];
        if (static_cast<std::size_t>(definition.kind) != index) {
            return false;
        }
        if (definition.identifier.space != kNamespace || definition.identifier.path.empty()) {
            return false;
        }
        for (std::size_t other = 0; other < index; ++other) {
            if (kBlockEntityTypeRegistry[other].identifier == definition.identifier) {
                return false;
            }
        }
    }
    return true;
}
static_assert(blockEntityTypeRegistryIsWellFormed(),
              "kBlockEntityTypeRegistry must list every BlockEntityKind once, in enum order, "
              "with unique identifiers");

using BlockEntityTypeRegistry =
    core::Registry<BlockEntityTypeDefinition, core::BlockEntityTypeId>;

// A block-entity type contributed by something other than this build's own
// content (a datapack/mod BE a loader parsed into a definition). It registers in
// the External phase, after every built-in has claimed its id, so built-in
// (vanilla-mirroring) ids stay stable regardless of external content. Parsing a
// definition out of JSON is out of scope for BE1 — the registration target
// merely exists and is tested. Wired now so the phase guard is real.
struct ExternalBlockEntityTypeDef final {
    BlockEntityTypeDefinition definition{};
};

// The process-wide list of external block-entity types to register once the
// registry builds. A content loader appends to it before the first
// blockEntityTypeRegistry() call (the only mutation window; once built, the
// registry is frozen). Empty in a build with no external content.
[[nodiscard]] inline std::vector<ExternalBlockEntityTypeDef>& externalBlockEntityTypes() {
    static std::vector<ExternalBlockEntityTypeDef> defs;
    return defs;
}

// Runs the three-phase lifecycle once: register every built-in (Bootstrap), open
// the External phase and register the supplied external types, then freeze.
// Built-ins go first so their ids equal their enum ordinals no matter what
// external content exists — the id-stability guarantee the block->BE bake and
// the save layer rest on. Extracted so a test can drive the same lifecycle with
// its own external content without touching the process singleton.
[[nodiscard]] inline BlockEntityTypeRegistry buildBlockEntityTypeRegistry(
    std::span<const ExternalBlockEntityTypeDef> external) {
    BlockEntityTypeRegistry built;
    for (const BlockEntityTypeDefinition& definition : kBlockEntityTypeRegistry) {
        const core::BlockEntityTypeId id = built.registerBuiltin(definition.identifier, definition);
        // Ids are handed out in registration order, so this mirrors the enum
        // ordinal — the equality BlockDefinition::blockEntityType is baked from.
        // Catch a drift the moment a kind is added out of order, not in a save bug.
        if (id != blockEntityTypeId(definition.kind)) {
            core::registryAbort("block entity type registered out of enum order");
        }
        // The `minecraft:` name resolves to the same id, so 1.16.1 saves reach the
        // type through either key.
        if (!definition.vanilla.empty() && definition.vanilla != definition.identifier) {
            built.alias(definition.vanilla, id);
        }
    }
    built.beginExternal();
    for (const ExternalBlockEntityTypeDef& def : external) {
        built.registerExternal(def.definition.identifier, def.definition);
    }
    built.freeze();
    return built;
}

// Builds and freezes the registry once, on first use. Frozen means the ids are
// final: every consumer downstream sees the same stable BlockEntityTypeId for a
// name.
[[nodiscard]] inline const BlockEntityTypeRegistry& blockEntityTypeRegistry() {
    static const BlockEntityTypeRegistry registry =
        buildBlockEntityTypeRegistry(externalBlockEntityTypes());
    return registry;
}

// The number of registered block-entity identities. Equals
// kBuiltinBlockEntityTypeCount for a build with no external content, and grows
// with the registry once the External phase can add types.
[[nodiscard]] inline std::size_t blockEntityTypeCount() {
    return blockEntityTypeRegistry().size();
}

} // namespace mc::world

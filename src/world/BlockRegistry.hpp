#pragma once

// The runtime block-identity registry, built by pouring the compile-time
// kBlockRegistry table into a generic core::Registry.
//
// This runs *alongside* the `enum class Block` for now: R0-1 stands the
// registry up and proves it round-trips, but nothing derefs a BlockId on a hot
// path yet. R0-2 is where the state tables, tags and shapes switch from
// `Block::Count`-sized arrays to BlockId indexing and the enum becomes a set of
// constexpr aliases. Because built-in blocks register in enum order, the id a
// block gets here equals its enum ordinal (asserted below), which is exactly
// what lets R0-2 alias `Stone` to its BlockId with no renumbering.

#include "core/ContentId.hpp"
#include "core/Registry.hpp"
#include "world/Block.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace mc::world {

using BlockRegistry = core::Registry<BlockDefinition, core::BlockId>;

// A block contributed by something other than this build's own content: a
// datapack block whose JSON a loader (D module) parsed into a BlockDefinition, or
// a native plugin's. It registers in the External phase, after every built-in has
// claimed its id, which is what keeps built-in (vanilla-mirroring) ids stable no
// matter how much external content a world carries. Parsing JSON into one of
// these is deliberately out of scope here — R0 only guarantees the registration
// target exists; see docs/content-dev/R0-identity/R0-DESIGN.md §2.4.
struct ExternalBlockDef final {
    BlockDefinition definition{};
};

// The process-wide list of external blocks to register once the registry builds.
// A datapack/native content loader appends to it before the first blockRegistry()
// call (the only mutation window; once built, the registry is frozen). Empty in a
// build with no external content, so the real app registers exactly the built-ins
// it does today — the External phase simply opens and closes with nothing in it.
[[nodiscard]] inline std::vector<ExternalBlockDef>& externalBlockDefs() {
    static std::vector<ExternalBlockDef> defs;
    return defs;
}

// Runs the three-phase lifecycle once: register every built-in (Bootstrap),
// open the External phase and register the supplied external blocks, then freeze.
// Built-ins go first so their ids equal their enum ordinals regardless of what
// external content exists — the id-stability guarantee R0-5's freeze gate rests
// on. Extracted from blockRegistry() so a test can drive the same lifecycle with
// its own external content without touching the process singleton.
[[nodiscard]] inline BlockRegistry buildBlockRegistry(std::span<const ExternalBlockDef> external) {
    BlockRegistry built;
    for (const BlockDefinition& definition : kBlockRegistry) {
        const core::BlockId id = built.registerBuiltin(definition.identifier, definition);
        // The registry hands ids out in registration order, so this mirrors the
        // enum ordinal. R0-2 leans on the equality; catch a drift the moment a
        // block is added out of order rather than in a save bug.
        if (id.index() != static_cast<std::size_t>(definition.block))
            core::registryAbort("block registered out of enum order");
        // The `minecraft:` name resolves to the same id, so vanilla content and
        // 1.16.1 saves reach the block through either key.
        if (!definition.vanilla.empty() && definition.vanilla != definition.identifier)
            built.alias(definition.vanilla, id);
    }
    built.beginExternal();
    for (const ExternalBlockDef& def : external) {
        built.registerExternal(def.definition.identifier, def.definition);
    }
    built.freeze();
    return built;
}

// Builds and freezes the registry once, on first use. Frozen means the ids are
// final: every consumer downstream sees the same stable BlockId for a name.
[[nodiscard]] inline const BlockRegistry& blockRegistry() {
    static const BlockRegistry registry = buildBlockRegistry(externalBlockDefs());
    return registry;
}

// The number of registered block identities — the size the runtime BlockId-keyed
// tables (block tags, the save palette) cut themselves to. Equals
// `kBuiltinBlockCount` for a build with no external content, and grows with the
// registry once the External phase can add blocks (R0-5). A constexpr built-in
// table cannot bake behaviour for a block that does not exist at compile time,
// so those stay `kBuiltinBlockCount` wide; only the runtime tables read this.
[[nodiscard]] inline std::size_t blockCount() { return blockRegistry().size(); }

} // namespace mc::world

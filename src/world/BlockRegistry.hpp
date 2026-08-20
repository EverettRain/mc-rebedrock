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

namespace mc::world {

using BlockRegistry = core::Registry<BlockDefinition, core::BlockId>;

// Builds and freezes the registry once, on first use. Frozen means the ids are
// final: every consumer downstream sees the same stable BlockId for a name.
[[nodiscard]] inline const BlockRegistry& blockRegistry() {
    static const BlockRegistry registry = [] {
        BlockRegistry built;
        for (const BlockDefinition& definition : kBlockRegistry) {
            const core::BlockId id = built.registerBuiltin(definition.identifier, definition);
            // The registry hands ids out in registration order, so this mirrors
            // the enum ordinal. R0-2 leans on the equality; catch a drift the
            // moment a block is added out of order rather than in a save bug.
            if (id.index() != static_cast<std::size_t>(definition.block))
                core::registryAbort("block registered out of enum order");
            // The `minecraft:` name resolves to the same id, so vanilla content
            // and 1.16.1 saves reach the block through either key.
            if (!definition.vanilla.empty() && definition.vanilla != definition.identifier)
                built.alias(definition.vanilla, id);
        }
        built.freeze();
        return built;
    }();
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

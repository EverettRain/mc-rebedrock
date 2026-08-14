#pragma once

// Block tags as a bit mask per block, loaded from the 26.1 data pack.
//
// This replaces five hand-written switch chains in MiningSystem (`mineable`
// with each tool, plus the harvest-tier gate). Two things are wrong with those
// switches and neither is style: they are a second copy of vanilla data that
// drifts every time a block is added — the 五种石材 stopgap in T0.1 exists
// precisely because the copies disagreed — and answering "is this block
// mineable with a pickaxe?" walks a case list instead of testing a bit.
//
// The data is vanilla's own: `data/minecraft/tags/block/…` out of the standard
// pack, through the same layered provider the client half uses, so a pack can
// override one tag without shadowing the rest.

#include "assets/ResourceProvider.hpp"
#include "world/Block.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mc::gameplay {

// The tags this project actually asks about. Adding one is an entry here plus
// its data path in kBlockTagPaths; the storage does not change until there are
// more than 64.
enum class BlockTag : std::uint8_t {
    MineableWithPickaxe,
    MineableWithAxe,
    MineableWithShovel,
    MineableWithHoe,
    NeedsStoneTool,
    NeedsIronTool,
    NeedsDiamondTool,
    Leaves,
    Logs,
    Count,
};

inline constexpr std::size_t kBlockTagCount = static_cast<std::size_t>(BlockTag::Count);
static_assert(kBlockTagCount <= 64U, "BlockTag membership is stored as one uint64_t per block");

// The pack-relative data path each tag loads from, indexed by BlockTag.
[[nodiscard]] std::string_view blockTagPath(BlockTag tag);

class BlockTagTable final {
  public:
    // Reads every tag in the enum. Tag files reference other tags with a `#`
    // prefix (26.1's `logs` is nothing but three such references), so entries
    // are expanded recursively; a cycle terminates rather than hanging.
    //
    // Membership merges across the provider stack low-to-high, the way vanilla
    // merges data packs, and a file with `"replace": true` discards what the
    // packs below it contributed. Identifiers this build has no block for are
    // skipped: a vanilla tag names hundreds of blocks that do not exist here,
    // and that is expected, not an error.
    void load(const assets::ResourceProvider& resources);

    [[nodiscard]] bool has(world::Block block, BlockTag tag) const {
        const auto index = static_cast<std::size_t>(block);
        return index < masks_.size() && (masks_[index] & bit(tag)) != 0U;
    }

    // Fills the table with 26.1's own tag contents, compiled in. This is the
    // floor load() starts from, and the whole table for a headless caller with
    // no pack at all.
    void loadBuiltinDefaults();

    // Whether a pack actually supplied this tag, as opposed to it coming from
    // the built-in defaults. Diagnostics only — behaviour never branches on it,
    // because both sources are equally authoritative once loaded.
    [[nodiscard]] bool dataDriven(BlockTag tag) const {
        return (dataDrivenTags_ & bit(tag)) != 0U;
    }

    // For tests, and for building a table by hand.
    void set(world::Block block, BlockTag tag);
    void clear(BlockTag tag);

  private:
    [[nodiscard]] static constexpr std::uint64_t bit(BlockTag tag) {
        return std::uint64_t{1} << static_cast<std::uint64_t>(tag);
    }

    std::array<std::uint64_t, static_cast<std::size_t>(world::Block::Count)> masks_{};
    std::uint64_t dataDrivenTags_ = 0U;
};

// The process-level table the mining rules read, alongside the block and entity
// registries this project already keeps that way. Application loads it once the
// pack stack is up; a headless caller that never loads gets the built-in
// defaults on first use, so mining works without any wiring.
[[nodiscard]] BlockTagTable& blockTags();

} // namespace mc::gameplay

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
#include <vector>

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

// A fixed-width bit set over the tag ids — one bit per BlockTag, per block. Kept
// as an array of 64-bit words rather than a single uint64_t so the tag
// vocabulary can grow past 64 without changing the storage's shape or the O(1)
// membership test: `test` is still one indexed load and one bit-and, it just
// picks the word first. This is R0-5's generalisation — Java scans a HolderSet,
// this stays a per-id mask. `Bits` is the tag count; the word count rounds up,
// so today's nine tags are one word and a build that adds a 65th spills into a
// second with no other change.
template <std::size_t Bits>
class TagBitset final {
  public:
    [[nodiscard]] constexpr bool test(std::size_t bit) const {
        return bit < Bits && (words_[bit / 64U] & mask(bit)) != 0U;
    }
    constexpr void set(std::size_t bit) {
        if (bit < Bits) {
            words_[bit / 64U] |= mask(bit);
        }
    }
    constexpr void reset(std::size_t bit) {
        if (bit < Bits) {
            words_[bit / 64U] &= ~mask(bit);
        }
    }
    [[nodiscard]] constexpr bool operator==(const TagBitset&) const = default;

  private:
    static constexpr std::size_t kWords = (Bits + 63U) / 64U;
    [[nodiscard]] static constexpr std::uint64_t mask(std::size_t bit) {
        return std::uint64_t{1} << (bit % 64U);
    }
    std::array<std::uint64_t, kWords> words_{};
};

// One block's whole tag membership.
using BlockTagMask = TagBitset<kBlockTagCount>;

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
        return has(world::blockId(block), tag);
    }
    [[nodiscard]] bool has(world::BlockId block, BlockTag tag) const {
        const auto index = block.index();
        return index < masks_.size() && masks_[index].test(static_cast<std::size_t>(tag));
    }

    // Fills the table with 26.1's own tag contents, compiled in. This is the
    // floor load() starts from, and the whole table for a headless caller with
    // no pack at all.
    void loadBuiltinDefaults();

    // Whether a pack actually supplied this tag, as opposed to it coming from
    // the built-in defaults. Diagnostics only — behaviour never branches on it,
    // because both sources are equally authoritative once loaded.
    [[nodiscard]] bool dataDriven(BlockTag tag) const {
        return dataDrivenTags_.test(static_cast<std::size_t>(tag));
    }

    // For tests, and for building a table by hand.
    void set(world::Block block, BlockTag tag);
    void set(world::BlockId block, BlockTag tag);
    void clear(BlockTag tag);

  private:
    // One membership mask per BlockId, sized to the block registry rather than
    // the enum's 256 ceiling so external content (R0-5) gets tag slots too.
    // Indexed by BlockId::index(); an id past the vector reads as untagged.
    std::vector<BlockTagMask> masks_;
    BlockTagMask dataDrivenTags_;
};

// The process-level table the mining rules read, alongside the block and entity
// registries this project already keeps that way. Application loads it once the
// pack stack is up; a headless caller that never loads gets the built-in
// defaults on first use, so mining works without any wiring.
[[nodiscard]] BlockTagTable& blockTags();

} // namespace mc::gameplay

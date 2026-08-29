#pragma once

#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/NibbleArray.hpp"
#include "world/WorldConstants.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc::world {

class ChunkSection final {
  public:
    ChunkSection();

    // The whole cell in one read. The three accessors below decode from this
    // and stay for the callers that only want one field.
    [[nodiscard]] BlockState state(int x, int y, int z) const;
    void setState(int x, int y, int z, BlockState value);

    [[nodiscard]] Block block(int x, int y, int z) const;
    void setBlock(int x, int y, int z, Block value);
    // Palette-level batch replace: rewrite every cell currently holding `from`
    // (in its default placement state, as setBlock stores it) to `to`, in
    // O(palette) rather than O(4096). Only that exact block changes — air (caves),
    // bedrock and other blocks are left alone — so the deepslate post-pass can
    // convert a whole uniform sub-layer of stone/ore to its deepslate form without
    // visiting each cell. Both blocks must be non-air; the non-air count is
    // preserved (a solid block becomes another solid block), not recomputed.
    void remapBlock(Block from, Block to);
    [[nodiscard]] BlockOrientation orientation(int x, int y, int z) const;
    void setOrientation(int x, int y, int z, BlockOrientation value);
    [[nodiscard]] std::uint8_t fluidLevel(int x, int y, int z) const;
    void setFluidLevel(int x, int y, int z, std::uint8_t value);
    [[nodiscard]] bool empty() const { return nonAirBlockCount_ == 0U; }
    [[nodiscard]] std::uint8_t skyLight(int x, int y, int z) const;
    [[nodiscard]] std::uint8_t blockLight(int x, int y, int z) const;
    [[nodiscard]] std::uint8_t directSkyLight(int x, int y, int z) const;
    bool setSkyLight(int x, int y, int z, std::uint8_t value);
    bool setBlockLight(int x, int y, int z, std::uint8_t value);
    bool setDirectSkyLight(int x, int y, int z, std::uint8_t value);

    // Heap bytes the state storage holds right now (palette + packed indices,
    // excluding the light arrays). Zero for an all-air section. Exposed so a
    // test can pin the memory contract: an empty section costs nothing and a
    // terrain section stays a fraction of the flat 8 KB array it replaced.
    [[nodiscard]] std::size_t stateHeapBytes() const;
    // Heap bytes the three light nibble arrays hold right now. A uniform array
    // keeps no backing allocation, so an all-uniform section costs nothing.
    [[nodiscard]] std::size_t lightHeapBytes() const;
    // The distinct states this section currently interns and how many bits each
    // cell's index takes. Diagnostics for the same test — a uniform section is
    // 0 bits with no packed data at all.
    [[nodiscard]] std::size_t paletteSize() const { return palette_.size(); }
    [[nodiscard]] std::uint8_t bitsPerEntry() const { return bitsPerEntry_; }

  private:
    static constexpr std::size_t kBlockCount =
        static_cast<std::size_t>(kSectionSize) *
        static_cast<std::size_t>(kSectionSize) *
        static_cast<std::size_t>(kSectionSize);

    [[nodiscard]] static std::size_t index(int x, int y, int z);
    [[nodiscard]] static bool inBounds(int x, int y, int z);

    // PalettedContainer, the layout vanilla stores a section in. A section holds
    // far fewer distinct states than its 4096 cells, so instead of one 16-bit
    // interned id per cell (the flat array this replaced — 8 KB the moment a
    // single non-air block landed, even for a section that is one stone and the
    // rest air), each cell stores a small index into a per-section palette of
    // the states actually present, bit-packed to just enough bits to name them.
    //
    //  - An all-air section is uniform: bitsPerEntry_ == 0, no palette, no
    //    packed data, zero heap. Surface worlds are mostly these.
    //  - The first non-air write expands to a multi-state section whose palette
    //    starts { air, <that state> }; bitsPerEntry_ grows (1,2,4,5,...) only as
    //    new distinct states appear. Ordinary terrain settles around 4 bits, so
    //    the 4096 indices pack into ~2 KB rather than 8 KB.
    //
    // The packing is SimpleBitStorage: each 64-bit word holds 64/bits indices
    // with no entry straddling a word boundary, so a read is one shift-and-mask.
    // The palette only grows — a removed state may linger unreferenced, which
    // costs a couple of bytes and saves scanning 4096 cells on every break.
    // readIndex/writeIndex speak the *local* palette index (a section holds at
    // most 4096 cells, so ≤4096 palette entries) — it stays u16. internState takes
    // a *global* rawId (BlockState::rawId, now u32) and returns its local index.
    [[nodiscard]] std::uint16_t readIndex(std::size_t cell) const;
    void writeIndex(std::size_t cell, std::uint16_t paletteIndex);
    [[nodiscard]] std::uint16_t internState(std::uint32_t rawId);
    void growBits(std::uint8_t newBits);
    [[nodiscard]] static std::uint8_t bitsFor(std::size_t paletteSize);
    [[nodiscard]] static std::size_t longsFor(std::uint8_t bits);

    // The per-section palette maps a local index to a *global* BlockState rawId
    // (u32). Each cell in `data_` stores the local index bit-packed, so widening
    // the rawId to u32 costs +2 bytes per palette entry (a handful per section),
    // not per cell — the section's memory footprint is effectively unchanged.
    std::vector<std::uint32_t> palette_;
    std::vector<std::uint64_t> data_;
    std::uint8_t bitsPerEntry_ = 0U;
    NibbleArray skyLight_;
    NibbleArray blockLight_;
    NibbleArray directSkyLight_;
    std::size_t nonAirBlockCount_ = 0U;
};

} // namespace mc::world

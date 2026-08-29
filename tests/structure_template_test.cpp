// STRUCT-0: the structure `.nbt` reader.
//
// What is pinned here: the schema-directed NBT cursor walks a hand-built
// structure template and lowers it to the flat POD (size, palette resolved to
// Block + packed state, blocks as local coords indexing the palette, a chest's
// LootTable lifted onto a block entity); a `facing` property folds into the block
// orientation while an unknown property is tolerated; a palette entry naming a
// block this build lacks is kept-but-unresolved so palette indices stay stable; a
// file whose DataVersion is not this build's is refused whole; and the same bytes
// gzip-compressed parse identically (the shipped form). No vanilla asset is read —
// the fixtures are built in memory so the test is hermetic.

#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/StructureTemplate.hpp"

#include <miniz.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using mc::world::Block;
using mc::world::BlockOrientation;
using mc::world::BlockState;
using mc::world::kNoBlockEntity;
using mc::world::kStructureDataVersion;
using mc::world::parseStructureTemplate;
using mc::world::StructureTemplateDef;

// --- a tiny big-endian NBT writer, enough for the structure schema -----------

struct NbtWriter final {
    std::vector<std::uint8_t> bytes;

    void u8(std::uint8_t value) { bytes.push_back(value); }
    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value >> 8));
        u8(static_cast<std::uint8_t>(value));
    }
    void i32(std::int32_t value) {
        const auto raw = static_cast<std::uint32_t>(value);
        u8(static_cast<std::uint8_t>(raw >> 24));
        u8(static_cast<std::uint8_t>(raw >> 16));
        u8(static_cast<std::uint8_t>(raw >> 8));
        u8(static_cast<std::uint8_t>(raw));
    }
    void str(std::string_view value) {
        u16(static_cast<std::uint16_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    // tag id + name, for a compound member.
    void named(std::uint8_t tag, std::string_view name) {
        u8(tag);
        str(name);
    }
    void end() { u8(0); } // TAG_End
};

constexpr std::uint8_t kInt = 3;
constexpr std::uint8_t kString = 8;
constexpr std::uint8_t kList = 9;
constexpr std::uint8_t kCompound = 10;

// Builds the reference template used across the assertions:
//   size [1,2,1]
//   palette: 0=stone, 1=furnace(facing=east, plus an unknown property), 2=<absent block>
//   blocks: {0,0,0 -> 0}, {0,1,0 -> 1 with a chest-like nbt LootTable}
//   DataVersion (caller-supplied so the reject path can pass a wrong one)
std::vector<std::uint8_t> buildTemplate(std::int32_t dataVersion) {
    NbtWriter w;
    w.u8(kCompound); // root, unnamed
    w.str("");

    // size
    w.named(kList, "size");
    w.u8(kInt);
    w.i32(3);
    w.i32(1);
    w.i32(2);
    w.i32(1);

    // palette (list of compounds)
    w.named(kList, "palette");
    w.u8(kCompound);
    w.i32(3);
    // 0: stone
    w.named(kString, "Name");
    w.str("minecraft:stone");
    w.end();
    // 1: furnace with facing=east and an unmodelled property
    w.named(kString, "Name");
    w.str("minecraft:furnace");
    w.named(kCompound, "Properties");
    w.named(kString, "facing");
    w.str("east");
    w.named(kString, "lit");
    w.str("false");
    w.end(); // Properties
    w.end(); // palette entry 1
    // 2: a block this build lacks
    w.named(kString, "Name");
    w.str("minecraft:totally_made_up_block");
    w.end();

    // blocks (list of compounds)
    w.named(kList, "blocks");
    w.u8(kCompound);
    w.i32(3);
    // block A: pos 0,0,0 -> palette 0
    w.named(kList, "pos");
    w.u8(kInt);
    w.i32(3);
    w.i32(0);
    w.i32(0);
    w.i32(0);
    w.named(kInt, "state");
    w.i32(0);
    w.end();
    // block B: pos 0,1,0 -> palette 1, with a chest nbt
    w.named(kList, "pos");
    w.u8(kInt);
    w.i32(3);
    w.i32(0);
    w.i32(1);
    w.i32(0);
    w.named(kInt, "state");
    w.i32(1);
    w.named(kCompound, "nbt");
    w.named(kString, "id");
    w.str("minecraft:chest");
    w.named(kString, "LootTable");
    w.str("minecraft:chests/igloo_chest");
    w.end(); // nbt
    w.end(); // block B
    // block C: a data structure block carrying a `metadata` marker (the igloo-style
    // loot binding), state palette 0 (stone).
    w.named(kList, "pos");
    w.u8(kInt);
    w.i32(3);
    w.i32(0);
    w.i32(0);
    w.i32(1);
    w.named(kInt, "state");
    w.i32(0);
    w.named(kCompound, "nbt");
    w.named(kString, "id");
    w.str("minecraft:structure_block");
    w.named(kString, "metadata");
    w.str("chest");
    w.end(); // nbt
    w.end(); // block C

    // DataVersion
    w.named(kInt, "DataVersion");
    w.i32(dataVersion);

    w.end(); // root
    return w.bytes;
}

// Wraps raw bytes in a minimal gzip container so the reader's inflate path runs.
std::vector<std::uint8_t> gzip(const std::vector<std::uint8_t>& raw) {
    // deflate the payload (raw deflate; miniz level default).
    mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::vector<std::uint8_t> deflated(bound);
    mz_stream stream{};
    // negative window bits -> raw deflate, no zlib header.
    int rc = mz_deflateInit2(&stream, MZ_DEFAULT_COMPRESSION, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS,
                             9, MZ_DEFAULT_STRATEGY);
    assert(rc == MZ_OK);
    stream.next_in = raw.data();
    stream.avail_in = static_cast<unsigned int>(raw.size());
    stream.next_out = deflated.data();
    stream.avail_out = static_cast<unsigned int>(deflated.size());
    rc = mz_deflate(&stream, MZ_FINISH);
    assert(rc == MZ_STREAM_END);
    deflated.resize(stream.total_out);
    mz_deflateEnd(&stream);

    // gzip header (10 bytes, no extra fields) + deflate + CRC32 + ISIZE.
    std::vector<std::uint8_t> out;
    out.insert(out.end(), {0x1F, 0x8B, 0x08, 0x00, 0, 0, 0, 0, 0, 0xFF});
    out.insert(out.end(), deflated.begin(), deflated.end());
    const std::uint32_t crc =
        static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, raw.data(), raw.size()));
    const std::uint32_t isize = static_cast<std::uint32_t>(raw.size());
    for (std::uint32_t value : {crc, isize}) {
        out.push_back(static_cast<std::uint8_t>(value));
        out.push_back(static_cast<std::uint8_t>(value >> 8));
        out.push_back(static_cast<std::uint8_t>(value >> 16));
        out.push_back(static_cast<std::uint8_t>(value >> 24));
    }
    return out;
}

void checkParsed(const StructureTemplateDef& def) {
    assert(def.sizeX == 1 && def.sizeY == 2 && def.sizeZ == 1);

    assert(def.palette.size() == 3);
    // 0: stone -> default state.
    assert(def.palette[0].resolved);
    assert(def.palette[0].block == Block::Stone);
    assert(def.palette[0].stateIndex == BlockState{Block::Stone}.rawId());
    // 1: furnace with facing=east folded in; the unknown `lit` value tolerated.
    assert(def.palette[1].resolved);
    assert(def.palette[1].block == Block::Furnace);
    assert(def.palette[1].stateIndex ==
           BlockState{Block::Furnace}.with(BlockOrientation::East).rawId());
    // 2: absent block -> kept but unresolved, so indices below still line up.
    assert(!def.palette[2].resolved);

    assert(def.blocks.size() == 3);
    assert(def.blocks[0].x == 0 && def.blocks[0].y == 0 && def.blocks[0].z == 0);
    assert(def.blocks[0].paletteIndex == 0);
    assert(def.blocks[0].blockEntityIndex == kNoBlockEntity);
    assert(def.blocks[1].y == 1);
    assert(def.blocks[1].paletteIndex == 1);
    assert(def.blocks[1].blockEntityIndex != kNoBlockEntity);

    assert(def.blockEntities.size() == 2);
    // block B: an embedded LootTable (the shipwreck/mineshaft mechanism).
    const auto& chest = def.blockEntities[def.blocks[1].blockEntityIndex];
    assert(chest.id == "minecraft:chest");
    assert(chest.lootTable == "minecraft:chests/igloo_chest");
    assert(chest.metadata.empty());
    // block C: a data-block metadata marker (the igloo mechanism), no LootTable.
    const auto& marker = def.blockEntities[def.blocks[2].blockEntityIndex];
    assert(marker.id == "minecraft:structure_block");
    assert(marker.metadata == "chest");
    assert(marker.lootTable.empty());
}

void spanOf(const std::vector<std::uint8_t>& v, std::span<const std::uint8_t>& out) {
    out = std::span<const std::uint8_t>{v.data(), v.size()};
}

} // namespace

int main() {
    // 1) Raw (uncompressed) NBT parses and lowers correctly.
    {
        const auto raw = buildTemplate(kStructureDataVersion);
        std::span<const std::uint8_t> bytes;
        spanOf(raw, bytes);
        const auto def = parseStructureTemplate(bytes);
        assert(def.has_value());
        checkParsed(*def);
    }

    // 2) The same bytes, gzip-compressed (the shipped form), parse identically.
    {
        const auto raw = buildTemplate(kStructureDataVersion);
        const auto compressed = gzip(raw);
        assert(compressed.size() >= 2 && compressed[0] == 0x1F && compressed[1] == 0x8B);
        std::span<const std::uint8_t> bytes;
        spanOf(compressed, bytes);
        const auto def = parseStructureTemplate(bytes);
        assert(def.has_value());
        checkParsed(*def);
    }

    // 3) A wrong DataVersion is refused whole (no DataFixerUpper ported).
    {
        const auto raw = buildTemplate(kStructureDataVersion - 1);
        std::span<const std::uint8_t> bytes;
        spanOf(raw, bytes);
        assert(!parseStructureTemplate(bytes).has_value());
    }

    // 4) Truncated bytes fail cleanly (no crash, no partial acceptance).
    {
        auto raw = buildTemplate(kStructureDataVersion);
        raw.resize(raw.size() / 2);
        std::span<const std::uint8_t> bytes;
        spanOf(raw, bytes);
        assert(!parseStructureTemplate(bytes).has_value());
    }

    // 5) Not a compound root -> not a structure template.
    {
        const std::vector<std::uint8_t> junk{0x03, 0x00, 0x00, 0x00, 0x00};
        std::span<const std::uint8_t> bytes;
        spanOf(junk, bytes);
        assert(!parseStructureTemplate(bytes).has_value());
    }

    return 0;
}

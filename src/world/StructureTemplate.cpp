#include "world/StructureTemplate.hpp"

#include "data/NbtReader.hpp"

#include <miniz.h>

#include <array>
#include <cstring>

namespace mc::world {
namespace {

using data::NbtReader;
using data::NbtTag;

// --- gzip -----------------------------------------------------------------

// Structure `.nbt` files ship gzip-compressed (magic 1f 8b). miniz has no gzip
// front-end, so the header is stripped by hand and the raw deflate body handed to
// tinfl. A stream without the gzip magic is assumed to be plain NBT already (so
// the reader is testable without a compressor) and returned as-is.
std::optional<std::vector<std::uint8_t>> inflateIfGzip(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 2 || bytes[0] != 0x1FU || bytes[1] != 0x8BU) {
        return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    }
    // 10-byte fixed header + at least the 8-byte trailer (CRC32 + ISIZE).
    if (bytes.size() < 18 || bytes[2] != 0x08U) {
        return std::nullopt; // not deflate-compressed gzip
    }
    const std::uint8_t flags = bytes[3];
    std::size_t offset = 10;
    const auto remaining = [&](std::size_t need) { return offset + need <= bytes.size() - 8; };
    if ((flags & 0x04U) != 0U) { // FEXTRA
        if (!remaining(2)) return std::nullopt;
        const std::size_t extra =
            static_cast<std::size_t>(bytes[offset]) | (static_cast<std::size_t>(bytes[offset + 1]) << 8);
        offset += 2 + extra;
    }
    if ((flags & 0x08U) != 0U) { // FNAME, zero-terminated
        while (offset < bytes.size() - 8 && bytes[offset] != 0U) ++offset;
        ++offset;
    }
    if ((flags & 0x10U) != 0U) { // FCOMMENT, zero-terminated
        while (offset < bytes.size() - 8 && bytes[offset] != 0U) ++offset;
        ++offset;
    }
    if ((flags & 0x02U) != 0U) { // FHCRC
        offset += 2;
    }
    if (offset > bytes.size() - 8) {
        return std::nullopt;
    }
    const std::size_t deflateLength = bytes.size() - 8 - offset;
    std::size_t outLength = 0;
    void* out = tinfl_decompress_mem_to_heap(bytes.data() + offset, deflateLength, &outLength, 0);
    if (out == nullptr) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> result(static_cast<const std::uint8_t*>(out),
                                     static_cast<const std::uint8_t*>(out) + outLength);
    mz_free(out);
    return result;
}

// --- palette resolution ---------------------------------------------------

std::optional<BlockOrientation> facingFromString(std::string_view value) {
    if (value == "north") return BlockOrientation::North;
    if (value == "east") return BlockOrientation::East;
    if (value == "south") return BlockOrientation::South;
    if (value == "west") return BlockOrientation::West;
    if (value == "up") return BlockOrientation::Up;
    if (value == "down") return BlockOrientation::Down;
    return std::nullopt;
}

// A jigsaw block's `orientation` is a FrontAndTop, e.g. "west_up" or "up_north":
// the part before the underscore is the front — the direction the connection
// points. That is all the jigsaw engine needs (the top only matters for a
// rollable joint's roll, handled at expansion).
BlockOrientation jigsawFrontFromOrientation(std::string_view orientation) {
    const auto underscore = orientation.find('_');
    const std::string_view front =
        underscore == std::string_view::npos ? orientation : orientation.substr(0, underscore);
    return facingFromString(front).value_or(BlockOrientation::North);
}

// One palette compound: `{Name: string, Properties?: {key: string}}`. Resolves to
// a Block + packed state. Unknown blocks and unmodelled properties fall back the
// tolerant way: an unknown Name leaves the entry unresolved; a `facing` property
// folds into the block orientation (the one axis with a clean, universal mapping),
// and every other property is left at the block's default state — STRUCT-2's
// rotation table is where the full property fold lands. The cursor is left just
// past the compound's closing End tag.
StructurePaletteEntry readPaletteEntry(NbtReader& reader) {
    StructurePaletteEntry entry;
    std::string name;
    std::optional<BlockOrientation> facing;
    std::string orientation; // FrontAndTop, for a jigsaw block
    for (;;) {
        const auto member = reader.readNamed();
        if (reader.failed() || member.tag == NbtTag::End) break;
        if (member.tag == NbtTag::String && member.name == "Name") {
            name = reader.readString();
        } else if (member.tag == NbtTag::Compound && member.name == "Properties") {
            for (;;) {
                const auto property = reader.readNamed();
                if (reader.failed() || property.tag == NbtTag::End) break;
                if (property.tag == NbtTag::String && property.name == "facing") {
                    facing = facingFromString(reader.readString());
                } else if (property.tag == NbtTag::String && property.name == "orientation") {
                    orientation = reader.readString();
                } else {
                    reader.skipPayload(property.tag);
                }
            }
        } else {
            reader.skipPayload(member.tag);
        }
    }
    if (name == "minecraft:jigsaw") {
        // Not a placeable block (unresolved), but its connection direction is
        // carried so readBlock can lift the jigsaw connection.
        entry.isJigsaw = true;
        entry.jigsawFront = jigsawFrontFromOrientation(orientation);
        return entry;
    }
    if (const auto block = blockFromIdentifier(name); block.has_value()) {
        BlockState state{*block};
        if (facing.has_value()) {
            state = state.with(*facing);
        }
        entry.block = *block;
        entry.stateIndex = state.rawId();
        entry.resolved = true;
    }
    return entry;
}

// One block compound: `{pos: [int,int,int], state: int, nbt?: {...}}`. Appends the
// block info and, when an `nbt` compound is present, a block entity carrying its
// `id` and `LootTable` (all the placement/loot path needs from STRUCT-0).
// `jigsawPaletteIdx` records, per lifted jigsaw, the palette index its front must
// be read from — resolved after the whole template is parsed, because `blocks` is
// stored before `palette` in the file (offsets confirmed), so the palette is not
// yet populated here.
void readBlock(NbtReader& reader, StructureTemplateDef& out,
               std::vector<std::uint16_t>& jigsawPaletteIdx) {
    StructureBlockInfo info;
    bool hasBlockEntity = false;
    StructureBlockEntity blockEntity;
    // Jigsaw fields, collected in case this block's nbt is a jigsaw connection.
    StructureJigsawBlock jigsaw;
    for (;;) {
        const auto member = reader.readNamed();
        if (reader.failed() || member.tag == NbtTag::End) break;
        if (member.tag == NbtTag::List && member.name == "pos") {
            const auto header = reader.readListHeader();
            std::array<std::int32_t, 3> pos{0, 0, 0};
            for (std::int32_t index = 0; index < header.length; ++index) {
                const std::int32_t value = reader.readInt();
                if (index < 3) pos[static_cast<std::size_t>(index)] = value;
            }
            info.x = static_cast<std::int8_t>(pos[0]);
            info.y = static_cast<std::int8_t>(pos[1]);
            info.z = static_cast<std::int8_t>(pos[2]);
        } else if (member.tag == NbtTag::Int && member.name == "state") {
            info.paletteIndex = static_cast<std::uint16_t>(reader.readInt());
        } else if (member.tag == NbtTag::Compound && member.name == "nbt") {
            hasBlockEntity = true;
            for (;;) {
                const auto field = reader.readNamed();
                if (reader.failed() || field.tag == NbtTag::End) break;
                if (field.tag == NbtTag::String && field.name == "id") {
                    blockEntity.id = reader.readString();
                } else if (field.tag == NbtTag::String && field.name == "LootTable") {
                    blockEntity.lootTable = reader.readString();
                } else if (field.tag == NbtTag::String && field.name == "metadata") {
                    blockEntity.metadata = reader.readString();
                } else if (field.tag == NbtTag::String && field.name == "pool") {
                    jigsaw.pool = reader.readString();
                } else if (field.tag == NbtTag::String && field.name == "target") {
                    jigsaw.target = reader.readString();
                } else if (field.tag == NbtTag::String && field.name == "name") {
                    jigsaw.name = reader.readString();
                } else if (field.tag == NbtTag::String && field.name == "joint") {
                    jigsaw.rollable = reader.readString() == "rollable";
                } else if (field.tag == NbtTag::String && field.name == "final_state") {
                    jigsaw.finalState = reader.readString();
                } else {
                    reader.skipPayload(field.tag);
                }
            }
        } else {
            reader.skipPayload(member.tag);
        }
    }
    // A jigsaw connection (marker block entity id == minecraft:jigsaw): lift it into
    // `jigsaws` with the front direction from its palette state. The marker block
    // itself is unresolved and skipped at placement; STRUCT-3b writes its
    // final_state. Everything else with an nbt is a regular block entity.
    // The palette is not parsed yet (blocks precede it in the file), so detect a
    // jigsaw by its block-entity id and defer the front to the post-pass.
    const bool isJigsaw = blockEntity.id == "minecraft:jigsaw";
    if (hasBlockEntity && isJigsaw) {
        jigsaw.x = info.x;
        jigsaw.y = info.y;
        jigsaw.z = info.z;
        out.jigsaws.push_back(std::move(jigsaw));
        jigsawPaletteIdx.push_back(info.paletteIndex);
    } else if (hasBlockEntity) {
        info.blockEntityIndex = static_cast<std::uint32_t>(out.blockEntities.size());
        out.blockEntities.push_back(std::move(blockEntity));
    }
    out.blocks.push_back(info);
}

// A `palette` list (element = compound). Some structures store `palettes` (a list
// of alternative palettes for random variants); the first is read and the rest
// skipped, matching what a single deterministic placement uses.
void readPalette(NbtReader& reader, StructureTemplateDef& out) {
    const auto header = reader.readListHeader();
    for (std::int32_t index = 0; index < header.length && !reader.failed(); ++index) {
        out.palette.push_back(readPaletteEntry(reader));
    }
}

} // namespace

std::optional<StructureTemplateDef> parseStructureTemplate(std::span<const std::uint8_t> fileBytes) {
    const auto inflated = inflateIfGzip(fileBytes);
    if (!inflated.has_value()) {
        return std::nullopt;
    }
    NbtReader reader{*inflated};

    // Root: an unnamed compound. Consume its tag + (empty) name header.
    const auto root = reader.readNamed();
    if (reader.failed() || root.tag != NbtTag::Compound) {
        return std::nullopt;
    }

    StructureTemplateDef def;
    std::vector<std::uint16_t> jigsawPaletteIdx; // aligned with def.jigsaws
    bool sawSize = false;
    bool dataVersionOk = false;
    for (;;) {
        const auto member = reader.readNamed();
        if (reader.failed() || member.tag == NbtTag::End) break;
        if (member.tag == NbtTag::List && member.name == "size") {
            const auto header = reader.readListHeader();
            std::array<std::int32_t, 3> size{0, 0, 0};
            for (std::int32_t index = 0; index < header.length; ++index) {
                const std::int32_t value = reader.readInt();
                if (index < 3) size[static_cast<std::size_t>(index)] = value;
            }
            def.sizeX = size[0];
            def.sizeY = size[1];
            def.sizeZ = size[2];
            sawSize = true;
        } else if (member.tag == NbtTag::List && member.name == "palette") {
            readPalette(reader, def);
        } else if (member.tag == NbtTag::List && member.name == "palettes") {
            // list-of-lists: read the first palette, skip the alternatives.
            const auto outer = reader.readListHeader();
            for (std::int32_t index = 0; index < outer.length && !reader.failed(); ++index) {
                if (index == 0) {
                    readPalette(reader, def);
                } else {
                    reader.skipPayload(NbtTag::List);
                }
            }
        } else if (member.tag == NbtTag::List && member.name == "blocks") {
            const auto header = reader.readListHeader();
            for (std::int32_t index = 0; index < header.length && !reader.failed(); ++index) {
                readBlock(reader, def, jigsawPaletteIdx);
            }
        } else if (member.tag == NbtTag::Int && member.name == "DataVersion") {
            dataVersionOk = reader.readInt() == kStructureDataVersion;
        } else {
            // entities and any other tag: not consumed by STRUCT-0.
            reader.skipPayload(member.tag);
        }
    }

    if (reader.failed() || !sawSize || !dataVersionOk) {
        return std::nullopt;
    }
    // Post-pass: now the palette is populated, read each jigsaw's front from its
    // palette entry's FrontAndTop orientation.
    for (std::size_t i = 0; i < def.jigsaws.size(); ++i) {
        const std::uint16_t paletteIndex = jigsawPaletteIdx[i];
        if (paletteIndex < def.palette.size()) {
            def.jigsaws[i].front = def.palette[paletteIndex].jigsawFront;
        }
    }
    return def;
}

StructureTemplateDef toDef(const BakedStructureTemplate& baked) {
    StructureTemplateDef def;
    def.sizeX = baked.sizeX;
    def.sizeY = baked.sizeY;
    def.sizeZ = baked.sizeZ;
    def.palette.reserve(baked.palette.size());
    for (const auto& slot : baked.palette) {
        StructurePaletteEntry entry;
        if (const auto block = blockFromIdentifier(slot.name); block.has_value()) {
            entry.block = *block;
            entry.stateIndex = BlockState{*block}.rawId();
            entry.resolved = true;
        }
        def.palette.push_back(entry);
    }
    def.blocks.assign(baked.blocks.begin(), baked.blocks.end());
    return def;
}

} // namespace mc::world

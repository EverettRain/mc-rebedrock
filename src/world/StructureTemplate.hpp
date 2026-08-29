#pragma once

// STRUCT-0: the in-memory form of a vanilla structure `.nbt` template, and the
// reader that fills it. The Java-side is `StructureTemplate` (a live object graph
// of `Palette` + `StructureBlockInfo` lists, block states resolved through the
// registry by reflection at *placement* time). This is the DOD lowering of that:
// the whole Java/NBT representation is a load-boundary format, reduced once into a
// flat, index-addressed POD whose consumer (the chunk-gen structure step, STRUCT-2)
// never touches NBT, JSON, or a block-state string.
//
// Concretely, mirroring the loot two-representation split (data/LootFile.hpp):
//   - `StructureTemplateDef` is the runtime, parsed form: a palette resolved to
//     `world::Block` + a packed state id, and blocks as flat `int8` local
//     coordinates indexing that palette. This is what a datapack overlay produces.
//   - `BakedStructureTemplate` is the constexpr-ready seam (string_view/span): a
//     future build may bake a curated structure into `.rodata` and hand the same
//     consumer a `toDef()` of it, with no change to placement. Content loads at
//     runtime now (decision, 2026-08-26); the *seam* stays baked-ready.
//
// The palette is resolved to blocks *here, at load* (not carried as
// `{Name, Properties}` strings into the hot path): `blockFromIdentifier` +
// BlockState. A palette entry naming a block this build lacks is kept but marked
// unresolved — its blocks are skipped at placement, the same forward-compatible
// "refuse what you can't represent" stance jeBlockLoot takes. A file whose
// DataVersion is not this build's is rejected whole (no DataFixerUpper is ported;
// JC deviation registered).

#include "world/Block.hpp"
#include "world/BlockState.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mc::world {

// Every shipped 26.1 structure template carries this DataVersion. A file with a
// different one is refused rather than upgraded (REGULAR bracket 8).
inline constexpr std::int32_t kStructureDataVersion = 4786;

// One resolved palette slot. `resolved` is false when this build has no block for
// the identifier; `block`/`stateIndex` are then Air/0 and placement skips any
// block pointing here (palette indices must stay stable, so the slot is not
// dropped).
struct StructurePaletteEntry final {
    Block block = Block::Air;
    std::uint32_t stateIndex = 0U; // BlockState::rawId of the resolved state (u32)
    bool resolved = false;
    // STRUCT-3a: a `minecraft:jigsaw` palette entry carries its connection
    // direction (the FrontAndTop `orientation`'s front) here — the block itself is
    // unresolved (not a registered block; it is replaced by its final_state), but
    // the jigsaw engine needs the direction the connection points. Meaningless for
    // non-jigsaw entries (left at the default).
    bool isJigsaw = false;
    BlockOrientation jigsawFront = BlockOrientation::North;
};

// Sentinel for "no block entity attached" in StructureBlockInfo::blockEntityIndex.
inline constexpr std::uint32_t kNoBlockEntity = 0xFFFFFFFFU;

// One block of the template. Coordinates are local to the template origin and fit
// in int8 (vanilla caps a template at 48 per axis; the largest shipped is well
// under 128). `paletteIndex` selects the state; `blockEntityIndex` points into
// `blockEntities` when this block carried an `nbt` compound (a chest, spawner…).
struct StructureBlockInfo final {
    std::int8_t x = 0;
    std::int8_t y = 0;
    std::int8_t z = 0;
    std::uint16_t paletteIndex = 0U;
    std::uint32_t blockEntityIndex = kNoBlockEntity;
};

// The minimum a block entity needs at load: its type id and, for a container, the
// loot table it rolls (STRUCT-1 fills it, STRUCT-2 places it). The full component
// payload is deferred — STRUCT-0 only lifts what the placement/loot path consumes.
struct StructureBlockEntity final {
    std::string id;        // e.g. "minecraft:chest"
    std::string lootTable; // the referenced chest loot table, or empty
    // A data/jigsaw structure block's marker (`metadata`, e.g. "chest"), the way
    // igloo/many B-family structures name where the piece code binds loot rather
    // than embedding a LootTable in the template. Empty when absent. STRUCT-2/4
    // reads this to place+fill those containers; STRUCT-0 only lifts the string.
    std::string metadata;
};

// STRUCT-3a: a jigsaw connection point lifted from the template. `front` is the
// direction the connection faces (from the block state's FrontAndTop orientation);
// `pool` is the template pool the piece on the other side is drawn from; `target`
// is the jigsaw name it must match there; `finalState` names the block that
// replaces the jigsaw marker once placed; `rollable` is the joint type (a rollable
// joint may rotate about the connection axis, an aligned one may not). The
// expansion algorithm (STRUCT-3b) consumes these.
struct StructureJigsawBlock final {
    std::int8_t x = 0;
    std::int8_t y = 0;
    std::int8_t z = 0;
    BlockOrientation front = BlockOrientation::North;
    std::string name;
    std::string target;
    std::string pool;
    std::string finalState; // block identifier that replaces the marker
    bool rollable = false;
};

struct StructureTemplateDef final {
    std::int32_t sizeX = 0;
    std::int32_t sizeY = 0;
    std::int32_t sizeZ = 0;
    std::vector<StructurePaletteEntry> palette;
    std::vector<StructureBlockInfo> blocks;
    std::vector<StructureBlockEntity> blockEntities;
    std::vector<StructureJigsawBlock> jigsaws;

    [[nodiscard]] bool operator==(const StructureTemplateDef&) const = default;
};

// Reads one structure `.nbt` file (gzip-compressed NBT, as shipped; a plain,
// uncompressed NBT stream is also accepted so the reader is testable without a
// compressor). Returns nullopt when the bytes are not a structure template, are
// truncated/malformed, or carry a DataVersion this build does not read. Never
// throws — a bad file is a skipped file.
[[nodiscard]] std::optional<StructureTemplateDef> parseStructureTemplate(
    std::span<const std::uint8_t> fileBytes);

// --- baked seam (constexpr-ready, currently unused floor) -----------------
//
// The string_view/span mirror of the Def, so a build can define a structure in
// `.rodata` and hand placement `toDef()` of it. Kept minimal and content-free for
// now (decision: content loads at runtime); present so the seam exists the day a
// milestone structure is baked, exactly as data/LootFile.hpp keeps BakedLootTable.
struct BakedStructurePaletteEntry final {
    std::string_view name; // vanilla block identifier, resolved by toDef
};
struct BakedStructureTemplate final {
    std::int32_t sizeX = 0;
    std::int32_t sizeY = 0;
    std::int32_t sizeZ = 0;
    std::span<const BakedStructurePaletteEntry> palette;
    std::span<const StructureBlockInfo> blocks;
};
[[nodiscard]] StructureTemplateDef toDef(const BakedStructureTemplate& baked);

} // namespace mc::world

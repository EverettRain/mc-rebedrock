#include "world/StructurePlacer.hpp"

#include "world/WorldConstants.hpp"

#include <algorithm>
#include <optional>
#include <string_view>

namespace mc::world {
namespace {

[[nodiscard]] std::optional<BlockOrientation> facingOf(std::string_view value) {
    if (value == "north") return BlockOrientation::North;
    if (value == "east") return BlockOrientation::East;
    if (value == "south") return BlockOrientation::South;
    if (value == "west") return BlockOrientation::West;
    if (value == "up") return BlockOrientation::Up;
    if (value == "down") return BlockOrientation::Down;
    return std::nullopt;
}

// A jigsaw block's `final_state` — the block that replaces the marker — e.g.
// "minecraft:oak_planks" or "minecraft:oak_stairs[facing=east,half=bottom,...]".
// Resolves the block and folds `facing` (the property most final_states carry that
// this build models); nullopt when the block is not registered.
[[nodiscard]] std::optional<BlockState> resolveFinalState(std::string_view final) {
    const auto bracket = final.find('[');
    const std::string_view name = bracket == std::string_view::npos ? final : final.substr(0, bracket);
    const auto block = blockFromIdentifier(name);
    if (!block.has_value()) {
        return std::nullopt;
    }
    BlockState state{*block};
    if (bracket != std::string_view::npos) {
        const std::string_view props = final.substr(bracket + 1);
        if (const auto facingPos = props.find("facing="); facingPos != std::string_view::npos) {
            std::string_view value = props.substr(facingPos + 7);
            value = value.substr(0, value.find_first_of(",]"));
            if (const auto facing = facingOf(value); facing.has_value()) {
                state = state.with(*facing);
            }
        }
    }
    return state;
}

} // namespace

void placeStructure(Chunk& chunk, int chunkX, int chunkZ, const StructureTemplateDef& tmpl,
                    int originX, int originY, int originZ, StructureRotation rotation,
                    std::vector<gen::TreeBorderBlock>& border,
                    std::vector<StructureLootPlacement>& loot, bool clip) {
    const int baseX = chunkX * kChunkWidth;
    const int baseZ = chunkZ * kChunkDepth;

    const auto writeCell = [&](int worldX, int worldY, int worldZ, BlockState state) {
        if (!isWorldYInRange(worldY)) {
            return;
        }
        const int localX = worldX - baseX;
        const int localZ = worldZ - baseZ;
        if (localX >= 0 && localX < kChunkWidth && localZ >= 0 && localZ < kChunkDepth) {
            chunk.setState(localX, worldY, localZ, state);
        } else if (!clip) {
            border.push_back(gen::TreeBorderBlock{worldX, worldY, worldZ, state});
        }
    };

    // Carve the structure's footprint to air *above the floor* before stamping, so
    // uneven terrain that rises into the structure (a column whose ground is higher
    // than the origin's) does not poke through the cells the template leaves as
    // structure void. The floor row (local y = 0) is left alone, so terrain still
    // fills any floor cell the template does not cover — no ring of holes under the
    // structure. This is a minimal terrain adaptation; full bearding is a later
    // refinement.
    const int footprintX = rotatedSizeX(tmpl.sizeX, tmpl.sizeZ, rotation);
    const int footprintZ = rotatedSizeZ(tmpl.sizeX, tmpl.sizeZ, rotation);
    // The footprint's intersection with this chunk, as local (dx, dz) offsets into
    // the piece. The canopy sweep only ever touches these cells; the carve sweep is
    // confined to them when clipping. A piece that only clips this chunk (most of a
    // multi-chunk village) sweeps just its slice instead of its whole footprint.
    const int inChunkDxLo = std::max(0, baseX - originX);
    const int inChunkDxHi = std::min(footprintX, baseX + kChunkWidth - originX);
    const int inChunkDzLo = std::max(0, baseZ - originZ);
    const int inChunkDzHi = std::min(footprintZ, baseZ + kChunkDepth - originZ);
    // Carve confines to the intersection when clipping; the non-clip path (the
    // origin chunk stamping its own structure, out-of-chunk cells routed to the
    // border stream by writeCell) still sweeps the full footprint.
    const int carveDxLo = clip ? inChunkDxLo : 0;
    const int carveDxHi = clip ? inChunkDxHi : footprintX;
    const int carveDzLo = clip ? inChunkDzLo : 0;
    const int carveDzHi = clip ? inChunkDzHi : footprintZ;
    const BlockState air{Block::Air};
    for (int dy = 1; dy < tmpl.sizeY; ++dy) {
        for (int dz = carveDzLo; dz < carveDzHi; ++dz) {
            for (int dx = carveDxLo; dx < carveDxHi; ++dx) {
                writeCell(originX + dx, originY + dy, originZ + dz, air);
            }
        }
    }

    // Above the structure, drop a tree canopy that would otherwise poke through the
    // roof: within the footprint, clear logs/leaves for a few rows above the top.
    // Only in-chunk cells are read (a neighbour's canopy is out of reach — that,
    // and trees the origin never sees, are the deeper gen-order fix: place
    // structures before decoration). Terrain is left alone; only trees are dropped.
    constexpr int kCanopyClearHeight = 8;
    for (int dy = tmpl.sizeY; dy < tmpl.sizeY + kCanopyClearHeight; ++dy) {
        const int worldY = originY + dy;
        if (!isWorldYInRange(worldY)) {
            break;
        }
        for (int dz = inChunkDzLo; dz < inChunkDzHi; ++dz) {
            for (int dx = inChunkDxLo; dx < inChunkDxHi; ++dx) {
                const int localX = originX + dx - baseX;
                const int localZ = originZ + dz - baseZ;
                if (localX < 0 || localX >= kChunkWidth || localZ < 0 || localZ >= kChunkDepth) {
                    continue;
                }
                const Block above = chunk.block(localX, worldY, localZ);
                if (isLog(above) || isLeaves(above)) {
                    chunk.setState(localX, worldY, localZ, air);
                }
            }
        }
    }

    for (const auto& info : tmpl.blocks) {
        if (info.paletteIndex >= tmpl.palette.size()) {
            continue;
        }
        const StructurePaletteEntry& entry = tmpl.palette[info.paletteIndex];
        if (!entry.resolved) {
            continue; // a block this build lacks: palette slot kept, cell skipped
        }
        const LocalPos local =
            rotateLocal({info.x, info.y, info.z}, tmpl.sizeX, tmpl.sizeZ, rotation);
        const int worldX = originX + local.x;
        const int worldY = originY + local.y;
        const int worldZ = originZ + local.z;
        if (!isWorldYInRange(worldY)) {
            continue;
        }
        const BlockState state = rotateState(BlockState::fromRawId(entry.stateIndex), rotation);
        writeCell(worldX, worldY, worldZ, state);

        if (info.blockEntityIndex != kNoBlockEntity &&
            info.blockEntityIndex < tmpl.blockEntities.size()) {
            const StructureBlockEntity& blockEntity = tmpl.blockEntities[info.blockEntityIndex];
            if (!blockEntity.lootTable.empty() || !blockEntity.metadata.empty()) {
                loot.push_back(StructureLootPlacement{worldX, worldY, worldZ,
                                                      blockEntity.lootTable, blockEntity.metadata});
            }
        }
    }

    // Jigsaw markers are unresolved (skipped above), leaving a hole; fill each with
    // its final_state (the block that replaces the marker), rotated with the piece.
    // A no-op for a single-template structure (no jigsaws).
    for (const StructureJigsawBlock& jigsaw : tmpl.jigsaws) {
        const auto finalState = resolveFinalState(jigsaw.finalState);
        if (!finalState.has_value()) {
            continue;
        }
        const LocalPos local =
            rotateLocal({jigsaw.x, jigsaw.y, jigsaw.z}, tmpl.sizeX, tmpl.sizeZ, rotation);
        writeCell(originX + local.x, originY + local.y, originZ + local.z,
                  rotateState(*finalState, rotation));
    }
}

} // namespace mc::world

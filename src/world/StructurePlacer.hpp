#pragma once

// STRUCT-2: writing a resolved structure template into the world.
//
// Given a template, an origin and a rotation, this stamps the template's blocks
// into one chunk, exactly the way TreeGrower stamps a tree: cells inside the
// chunk's x/z box are written, cells past the border are handed back as
// `gen::TreeBorderBlock`s so the streamer's existing pendingBorderBlocks_ path
// finishes them when the neighbour chunk arrives (STRUCT reuses that mechanism
// rather than building a second one). Chest/marker block entities are emitted as
// `StructureLootPlacement`s for the caller to bind against the chest loot table
// (STRUCT-1) and BlockEntityStore — the placer itself stays free of those deps so
// it is a pure template→cells function, headless-testable against a bare Chunk.

#include "world/Chunk.hpp"
#include "world/StructureRotation.hpp"
#include "world/StructureTemplate.hpp"
#include "world/gen/TreeGrower.hpp"

#include <string>
#include <vector>

namespace mc::world {

// A container/marker the placement produced. `lootTable` is the chest table a
// block entity embedded (the shipwreck/mineshaft mechanism); `metadata` is a
// data-block marker (the igloo mechanism) that STRUCT-4's piece logic binds. One
// of the two is non-empty.
struct StructureLootPlacement final {
    int worldX = 0;
    int worldY = 0;
    int worldZ = 0;
    std::string lootTable;
    std::string metadata;
};

// Stamps `tmpl` into `chunk` (grid coords chunkX/chunkZ; its cells span world
// [chunkX*16 .. +16) on x/z), with the template origin at world
// (originX,originY,originZ) and rotated by `rotation`. Cells outside this chunk go
// to `border`; chests/markers go to `loot`. Unresolved palette slots and cells
// outside the world height are skipped.
// When `clip` is set, cells outside this chunk are dropped rather than pushed to
// `border`. A jigsaw structure places every chunk's own share independently (each
// chunk re-derives the layout and stamps only its intersecting pieces, clipped),
// so no cross-chunk border stream is needed and no whole-structure backlog builds
// up under the world lock — the per-chunk cost stays bounded. A single-template
// structure (igloo) leaves `clip` false and overflows through `border` as before.
void placeStructure(Chunk& chunk, int chunkX, int chunkZ, const StructureTemplateDef& tmpl,
                    int originX, int originY, int originZ, StructureRotation rotation,
                    std::vector<gen::TreeBorderBlock>& border,
                    std::vector<StructureLootPlacement>& loot, bool clip = false);

} // namespace mc::world

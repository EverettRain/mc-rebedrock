#pragma once

// STRUCT-2 (wiring, second half): the generation step that stamps structures into
// a freshly generated chunk.
//
// It runs after terrain/surface/features, the way vanilla's structure step runs
// after the noise and surface steps. For each registered structure set it asks the
// random_spread placement "does a structure start in *this* chunk?"; if so, it
// gates on the origin biome, resolves the template, picks a deterministic rotation,
// finds the ground height from this chunk's own column, and stamps the template.
//
// Cross-chunk handling reuses the tree mechanism exactly: only the *origin* chunk
// stamps a structure (so the ground height is read from the origin's own column —
// no neighbour chunk is consulted, keeping generation order-independent), and cells
// that fall outside the origin chunk are handed to `border` (the same
// TreeBorderBlock stream the streamer already threads to neighbours through
// pendingBorderBlocks_). With no registered sets/templates this is a no-op, so a
// build without structure content generates byte-for-byte as before.

#include "world/Chunk.hpp"
#include "world/StructureManager.hpp"
#include "world/StructurePlacer.hpp"
#include "world/gen/TreeGrower.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace mc::world::gen {

// Stamps every structure whose origin is (chunkX, chunkZ) into `chunk`. `biomeAt`
// samples the biome at a world column (SurfaceGenerator::biomeAt). Out-of-chunk
// cells go to `border`. Chests/markers the placement emits are dropped here — the
// container-content binding to BlockEntityStore is a separate world-gen→gameplay
// seam (the next STRUCT-2 step); the chest *block* is still placed and visible.
void placeStructures(Chunk& chunk, int chunkX, int chunkZ, std::uint64_t worldSeed,
                     const StructureManager& manager,
                     const std::function<Biome(int worldX, int worldZ)>& biomeAt,
                     const std::function<int(int worldX, int worldZ)>& heightAt,
                     std::vector<TreeBorderBlock>& border);

// The chests/markers a structure whose origin is (chunkX, chunkZ) places, with
// world Y resolved against `groundY` (the origin column's surface height). This is
// a deterministic replay of the block pass's placement, used gameplay-side to
// create the chest block entities and fill their loot — no state is threaded out
// of the generation worker; both sides derive from the same placement + rotation.
[[nodiscard]] std::vector<StructureLootPlacement> structureChestsForChunk(
    int chunkX, int chunkZ, std::uint64_t worldSeed, const StructureManager& manager, int groundY,
    const std::function<Biome(int worldX, int worldZ)>& biomeAt);

} // namespace mc::world::gen

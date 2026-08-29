#include "world/gen/StructureGenerator.hpp"

#include "gameplay/Random.hpp"
#include "world/BlockShape.hpp"
#include "world/StructurePlacer.hpp"
#include "world/StructureRotation.hpp"
#include "world/WorldConstants.hpp"
#include "world/gen/JigsawExpansion.hpp"

#include <mutex>
#include <unordered_map>

#include <algorithm>
#include <string>

namespace mc::world::gen {
namespace {

// Approximate overworld sea level; a jigsaw structure whose origin ground is below
// this is treated as under water and skipped (a coarse guard that complements the
// biome gate for the biome-agnostic debug placement).
constexpr int kApproxSeaLevel = 62;

// The top-most *terrain ground* cell in a column, or kMinY-1 when the column has
// none. Vanilla's WORLD_SURFACE_WG heightmap the igloo projects onto is the
// terrain before decoration, but the structure step runs after the surface
// generator has already grown trees and plants, so those must be stepped over to
// reach the ground they stand on: `hasCollision` drops grass tufts, flowers and
// water (all non-colliding), and `isLog`/`isLeaves` drop a tree the origin column
// happens to sit under — otherwise the structure would perch on a treetop.
[[nodiscard]] int surfaceHeight(const Chunk& chunk, int localX, int localZ) {
    for (int y = kMaxY - 1; y >= kMinY; --y) {
        const Block block = chunk.block(localX, y, localZ);
        if (block != Block::Air && hasCollision(block) && !isLog(block) && !isLeaves(block)) {
            return y;
        }
    }
    return kMinY - 1;
}

// Whether the ground at `groundY` is under water — a column a land structure must
// not build on (an igloo in the ocean). Water is non-colliding, so surfaceHeight
// already skipped past it to the sea floor; this catches that the floor it found
// is submerged.
[[nodiscard]] bool groundIsSubmerged(const Chunk& chunk, int localX, int localZ, int groundY) {
    return isWorldYInRange(groundY + 1) &&
           chunk.block(localX, groundY + 1, localZ) == Block::Water;
}

[[nodiscard]] bool biomeAllowed(const StructureSet& set, Biome biome) {
    return set.biomes.empty() ||
           std::find(set.biomes.begin(), set.biomes.end(), biome) != set.biomes.end();
}

// One jigsaw piece with everything a chunk needs precomputed: the resolved
// template pointer (so the per-chunk stamp skips the string-keyed lookup) and the
// piece's world footprint on x/z (so intersection is a bare AABB test, no rotated-
// size recompute). Templates outlive the cache (they live in the StructureManager
// for the world's lifetime), so the raw pointer is stable.
struct PlacedPiece final {
    const StructureTemplateDef* tmpl = nullptr;
    JigsawPiece piece;
    int minX = 0;  // world footprint, half-open [minX, maxX) x [minZ, maxZ)
    int minZ = 0;
    int maxX = 0;
    int maxZ = 0;
};

// The jigsaw layout of one village, expanded once and reused by every chunk it
// touches. A jigsaw structure spans many chunks; without this cache each of those
// chunks would re-run the (multi-millisecond) expansion, and — worse — the origin
// chunk alone used to stamp the whole village and push thousands of cells through
// the cross-chunk border stream under the world lock, stalling the render thread
// on movement. Now the expansion runs once, keyed by (seed, origin), and each
// chunk stamps only its own intersecting pieces (clipped). Thread-safe because
// chunk generation is parallel; entries are never erased and std::unordered_map
// keeps element pointers stable across inserts/rehash, so cachedVillageLayout hands
// back a pointer into the map (no per-chunk copy) that stays valid while other
// workers insert. Keyed on a hash of seed + origin chunk.
struct VillageLayout final {
    int groundY = 0;
    std::vector<PlacedPiece> pieces;
};
std::mutex g_villageCacheMutex;
std::unordered_map<std::uint64_t, VillageLayout> g_villageCache;

[[nodiscard]] std::uint64_t villageKey(std::uint64_t worldSeed, int originChunkX, int originChunkZ) {
    std::uint64_t key = worldSeed;
    key = key * 1099511628211ULL + static_cast<std::uint64_t>(static_cast<std::uint32_t>(originChunkX));
    key = key * 1099511628211ULL + static_cast<std::uint64_t>(static_cast<std::uint32_t>(originChunkZ));
    return key;
}

// The rotation a structure at this origin is turned by — deterministic from the
// origin + salt, so the block pass and the chest replay derive the same one.
[[nodiscard]] StructureRotation structureRotationAt(int originX, int originZ,
                                                    std::uint64_t worldSeed, std::int32_t salt) {
    std::uint64_t state = mc::rng::seedFromValue(
        static_cast<std::uint64_t>(static_cast<std::int64_t>(originX) * 341873128712LL +
                                   static_cast<std::int64_t>(originZ) * 132897987541LL +
                                   static_cast<std::int64_t>(worldSeed) + salt));
    return static_cast<StructureRotation>(mc::rng::nextInt(state, 4U));
}

// The expanded layout of the village whose origin chunk is (originChunkX,
// originChunkZ), computed once and cached. Returns a pointer into the cache; the
// element (and the template pointers it holds) outlive the call, and the map keeps
// element pointers stable across concurrent inserts, so the caller may hold it
// across the placement loop.
[[nodiscard]] const VillageLayout* cachedVillageLayout(std::uint64_t worldSeed, int originChunkX,
                                                       int originChunkZ, const StructureSet& set,
                                                       const StructureManager& manager,
                                                       int groundY) {
    const std::uint64_t key = villageKey(worldSeed, originChunkX, originChunkZ);
    {
        const std::lock_guard<std::mutex> guard{g_villageCacheMutex};
        if (const auto found = g_villageCache.find(key); found != g_villageCache.end()) {
            return &found->second;
        }
    }
    // Expand outside the lock (the multi-millisecond step); resolve each piece's
    // template + footprint once here so the per-chunk stamp is pointer-only.
    std::uint64_t rng = mc::rng::seedFromValue(static_cast<std::uint64_t>(
        static_cast<std::int64_t>(originChunkX) * 341873128712LL +
        static_cast<std::int64_t>(originChunkZ) * 132897987541LL +
        static_cast<std::int64_t>(worldSeed) + set.placement.salt + 1));
    const std::vector<JigsawPiece> expanded =
        jigsawExpand(manager, set.startPool, originChunkX * kChunkWidth, groundY,
                     originChunkZ * kChunkDepth, set.size, set.maxDistance, rng);
    VillageLayout layout;
    layout.groundY = groundY;
    layout.pieces.reserve(expanded.size());
    for (const JigsawPiece& piece : expanded) {
        const StructureTemplateDef* tmpl = manager.find(piece.templateId);
        if (tmpl == nullptr) {
            continue; // this build lacks the piece template: nothing to stamp
        }
        const int sizeX = rotatedSizeX(tmpl->sizeX, tmpl->sizeZ, piece.rotation);
        const int sizeZ = rotatedSizeZ(tmpl->sizeX, tmpl->sizeZ, piece.rotation);
        layout.pieces.push_back(PlacedPiece{tmpl, piece, piece.originX, piece.originZ,
                                            piece.originX + sizeX, piece.originZ + sizeZ});
    }
    const std::lock_guard<std::mutex> guard{g_villageCacheMutex};
    return &g_villageCache.emplace(key, std::move(layout)).first->second;
}

} // namespace

void placeStructures(Chunk& chunk, int chunkX, int chunkZ, std::uint64_t worldSeed,
                     const StructureManager& manager,
                     const std::function<Biome(int, int)>& biomeAt,
                     const std::function<int(int, int)>& heightAt,
                     std::vector<TreeBorderBlock>& border) {
    std::vector<StructureLootPlacement> loot; // dropped this pass (chests bound later)

    for (const StructureSet& set : manager.sets()) {
        if (set.kind == StructureKind::Jigsaw) {
            // A village spans many chunks, so this chunk stamps its own share of any
            // village whose origin is close enough to reach it: for every candidate
            // origin within the structure's block radius, expand the layout (cached,
            // once per village) and clip-stamp the pieces that overlap this chunk. No
            // whole-village stamp, no cross-chunk border backlog — the per-chunk cost
            // stays bounded. The origin Y comes from the standalone heightmap so the
            // layout is the same however the player reaches the village.
            const int reachChunks = set.maxDistance / kChunkWidth + 2;
            for (int deltaChunkX = -reachChunks; deltaChunkX <= reachChunks; ++deltaChunkX) {
                for (int deltaChunkZ = -reachChunks; deltaChunkZ <= reachChunks; ++deltaChunkZ) {
                    const int originChunkX = chunkX + deltaChunkX;
                    const int originChunkZ = chunkZ + deltaChunkZ;
                    if (!set.placement.isStructureChunk(originChunkX, originChunkZ, worldSeed)) {
                        continue;
                    }
                    const int originX = originChunkX * kChunkWidth;
                    const int originZ = originChunkZ * kChunkDepth;
                    if (!biomeAllowed(set, biomeAt(originX, originZ))) {
                        continue;
                    }
                    const int groundY = heightAt(originX, originZ);
                    // A surface below sea level is under water; a land structure skips
                    // it (the biome gate already excludes oceans in vanilla, this also
                    // covers the biome-agnostic debug placement).
                    if (groundY < kMinY || groundY < kApproxSeaLevel) {
                        continue;
                    }
                    const VillageLayout* layout =
                        cachedVillageLayout(worldSeed, originChunkX, originChunkZ, set, manager,
                                            groundY);
                    const int chunkMinX = chunkX * kChunkWidth;
                    const int chunkMinZ = chunkZ * kChunkDepth;
                    const int chunkMaxX = chunkMinX + kChunkWidth;
                    const int chunkMaxZ = chunkMinZ + kChunkDepth;
                    for (const PlacedPiece& placed : layout->pieces) {
                        // Bare AABB test against this chunk on x/z (template + footprint
                        // were resolved once at expansion time).
                        if (placed.minX >= chunkMaxX || placed.maxX <= chunkMinX ||
                            placed.minZ >= chunkMaxZ || placed.maxZ <= chunkMinZ) {
                            continue;
                        }
                        const JigsawPiece& piece = placed.piece;
                        placeStructure(chunk, chunkX, chunkZ, *placed.tmpl, piece.originX,
                                       piece.originY, piece.originZ, piece.rotation, border, loot,
                                       /*clip=*/true);
                    }
                }
            }
            continue;
        }

        // Single template (igloo): only its origin chunk places it, overflowing
        // through the border stream to neighbours.
        if (!set.placement.isStructureChunk(chunkX, chunkZ, worldSeed)) {
            continue;
        }
        const int originX = chunkX * kChunkWidth;
        const int originZ = chunkZ * kChunkDepth;
        if (!biomeAllowed(set, biomeAt(originX, originZ))) {
            continue;
        }
        const int groundY = surfaceHeight(chunk, 0, 0);
        if (groundY < kMinY || groundIsSubmerged(chunk, 0, 0, groundY)) {
            continue;
        }
        const StructureTemplateDef* tmpl = manager.find(set.templateId);
        if (tmpl == nullptr) {
            continue;
        }
        const StructureRotation rotation =
            structureRotationAt(originX, originZ, worldSeed, set.placement.salt);
        placeStructure(chunk, chunkX, chunkZ, *tmpl, originX, groundY, originZ, rotation, border,
                       loot);
    }
}

std::vector<StructureLootPlacement> structureChestsForChunk(
    int chunkX, int chunkZ, std::uint64_t worldSeed, const StructureManager& manager, int groundY,
    const std::function<Biome(int, int)>& biomeAt) {
    std::vector<StructureLootPlacement> chests;
    for (const StructureSet& set : manager.sets()) {
        if (!set.placement.isStructureChunk(chunkX, chunkZ, worldSeed)) {
            continue;
        }
        const int originX = chunkX * kChunkWidth;
        const int originZ = chunkZ * kChunkDepth;
        if (!biomeAllowed(set, biomeAt(originX, originZ))) {
            continue;
        }
        const StructureTemplateDef* tmpl = manager.find(set.templateId);
        if (tmpl == nullptr) {
            continue;
        }
        const StructureRotation rotation =
            structureRotationAt(originX, originZ, worldSeed, set.placement.salt);
        for (const StructureBlockInfo& info : tmpl->blocks) {
            if (info.blockEntityIndex == kNoBlockEntity ||
                info.blockEntityIndex >= tmpl->blockEntities.size()) {
                continue;
            }
            const StructureBlockEntity& blockEntity = tmpl->blockEntities[info.blockEntityIndex];
            if (blockEntity.lootTable.empty() && blockEntity.metadata.empty()) {
                continue;
            }
            const LocalPos local =
                rotateLocal({info.x, info.y, info.z}, tmpl->sizeX, tmpl->sizeZ, rotation);
            chests.push_back(StructureLootPlacement{originX + local.x, groundY + local.y,
                                                    originZ + local.z, blockEntity.lootTable,
                                                    blockEntity.metadata});
        }
    }
    return chests;
}

} // namespace mc::world::gen

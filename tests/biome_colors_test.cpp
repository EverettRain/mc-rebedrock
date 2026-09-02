#include "render/MeshData.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkMesher.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"
#include "world/gen/Biome.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>

// BM-1: biome colour is a per-vertex tint averaged over vanilla's blend window,
// not one baked atlas layer per biome.
//
// The scheme this replaced tinted the grass and leaf textures at atlas build time
// and gave each biome its own layer — 175 layers for 25 biomes, and 26.1 has 66.
// Being a discrete layer it could not blend across a biome border (the colour
// stepped at a 4-block cell edge) and could not tint water at all.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"biome_colors_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using mc::world::Block;
using mc::world::Chunk;
using mc::world::World;
using mc::world::gen::Biome;
using mc::world::gen::biomeDefinition;
using mc::world::gen::GrassColorModifier;

// The five BiomeSpecialEffects fields, checked against 26.1's own numbers
// (OverworldBiomes.java: baseBiome defaults to waterColor 4159204; the swamp and
// dark forest are the only overworld biomes here that depart from it).
void checkSpecialEffects() {
    constexpr std::uint32_t kDefaultWater = 4159204U;  // 0x3F76E4
    for (int index = 0; index < static_cast<int>(Biome::Count); ++index) {
        const auto biome = static_cast<Biome>(index);
        const auto& definition = biomeDefinition(biome);
        if (biome != Biome::Swamp) {
            REQUIRE(definition.waterColor == kDefaultWater);
        }
    }
    // Swamp: waterColor 6388580, foliageColorOverride 6975545,
    // dryFoliageColorOverride 8082228, grassColorModifier SWAMP.
    const auto& swamp = biomeDefinition(Biome::Swamp);
    REQUIRE(swamp.waterColor == 6388580U);
    REQUIRE(swamp.foliageColorOverride == 6975545U);
    REQUIRE(swamp.dryFoliageColorOverride == 8082228U);
    REQUIRE(swamp.grassColorModifier == GrassColorModifier::Swamp);
    // Dark forest keeps the default water and darkens grass through the modifier
    // rather than an override, so its colour still follows the colour map.
    const auto& darkForest = biomeDefinition(Biome::DarkForest);
    REQUIRE(darkForest.waterColor == kDefaultWater);
    REQUIRE(darkForest.grassColorOverride == 0U);
    REQUIRE(darkForest.dryFoliageColorOverride == 8082228U);
    REQUIRE(darkForest.grassColorModifier == GrassColorModifier::DarkForest);
    // The savanna's temperature is 2.0 in 26.1 (it was still carrying 1.2, a
    // 1.16 value, which indexes a different cell of the grass colour map).
    REQUIRE(biomeDefinition(Biome::Savanna).temperature == 2.0F);
    REQUIRE(biomeDefinition(Biome::Savanna).downfall == 0.0F);
}

// GrassColorModifier#modifyColor, vanilla's arithmetic including the shift
// binding looser than the addition.
void checkGrassColorModifier() {
    constexpr std::uint32_t base = 0x91BD59U;
    REQUIRE(mc::world::gen::applyGrassColorModifier(GrassColorModifier::None, base, 0, 0) == base);
    const auto dark =
        mc::world::gen::applyGrassColorModifier(GrassColorModifier::DarkForest, base, 0, 0);
    REQUIRE(dark == ((((base & 0xFEFEFEU) + 0x28340AU) >> 1U) & 0xFFFFFFU));
    // The swamp picks one of two fixed tones by noise; both must be reachable
    // and nothing else may come out.
    bool sawLight = false;
    bool sawDark = false;
    for (int x = -256; x < 256; x += 7) {
        const auto color =
            mc::world::gen::applyGrassColorModifier(GrassColorModifier::Swamp, base, x, x * 3);
        REQUIRE(color == 0x4C763CU || color == 0x6A7039U);
        sawLight = sawLight || color == 0x6A7039U;
        sawDark = sawDark || color == 0x4C763CU;
    }
    REQUIRE(sawLight && sawDark);
    // Deterministic: the same position always answers the same tone.
    REQUIRE(mc::world::gen::applyGrassColorModifier(GrassColorModifier::Swamp, base, 40, 90) ==
            mc::world::gen::applyGrassColorModifier(GrassColorModifier::Swamp, base, 40, 90));
    // The noise is sampled at x * 0.0225, which is a period of roughly 44 blocks
    // — the swamp mottles in patches, it does not dither. Sampling every fourth
    // block, the tone should hold far more often than it flips; a scale that is
    // off by a factor of ten turns the patches into noise and this catches it.
    int flips = 0;
    std::uint32_t previous =
        mc::world::gen::applyGrassColorModifier(GrassColorModifier::Swamp, base, 0, 0);
    for (int x = 4; x <= 256; x += 4) {
        const auto color =
            mc::world::gen::applyGrassColorModifier(GrassColorModifier::Swamp, base, x, 0);
        if (color != previous) {
            ++flips;
        }
        previous = color;
    }
    REQUIRE(flips <= 8);
}

// A flat grass floor at y=0, chunks -1..2 in x and -1..1 in z, with a biome
// border down the middle of the world: everything at x < 16 is `west`, the rest
// is `east`.
World makeBorderWorld(Biome west, Biome east) {
    World world;
    for (int chunkZ = -1; chunkZ <= 1; ++chunkZ) {
        for (int chunkX = -1; chunkX <= 2; ++chunkX) {
            Chunk chunk;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    chunk.setBlock(x, mc::world::kMinY + 1, z, Block::Grass);
                    chunk.setColumnBiome(x, z, chunkX * 16 + x < 16 ? west : east);
                }
            }
            world.setChunk({chunkX, chunkZ}, std::move(chunk));
        }
    }
    return world;
}

// The tint of the up-facing vertex at exactly (worldX, worldZ) — the corner the
// mesher resolved, not an average of the face. Positions decode relative to the
// section origin, so a chunk other than (0,0) needs its origin subtracted.
[[nodiscard]] std::array<std::uint8_t, 3> topVertexTint(const mc::render::MeshData& mesh,
                                                        int worldX, int worldZ,
                                                        int originX = 0, int originZ = 0) {
    for (const auto& vertex : mesh.vertices) {
        // normalIndex 2 is {0, 1, 0}: the up face.
        if (vertex.normalIndex != 2U) {
            continue;
        }
        const auto position = mc::render::decodeLocalPosition(vertex);
        if (std::abs(position.x - static_cast<float>(worldX - originX)) < 0.01F &&
            std::abs(position.z - static_cast<float>(worldZ - originZ)) < 0.01F &&
            std::abs(position.y - 2.0F) < 0.01F) {
            return {vertex.tintR, vertex.tintG, vertex.tintB};
        }
    }
    throw std::runtime_error{"no up-facing vertex at (" + std::to_string(worldX) + ", " +
                             std::to_string(worldZ) + ")"};
}

// Crossing a biome border, the tint has to walk from one biome's colour to the
// other instead of stepping. This is the whole point of BM-1: with the baked
// atlas layers the colour changed in one jump at a 4-block cell edge.
void checkBorderBlend() {
    // Two flat, distinguishable colours, so the assertions read as arithmetic
    // rather than as whatever the colour map happens to hold.
    mc::world::gen::setBiomeSurfaceColors(Biome::Plains, {0x000000U, 0U, 0U, 0U});
    mc::world::gen::setBiomeSurfaceColors(Biome::Desert, {0x640000U, 0U, 0U, 0U});
    const auto world = makeBorderWorld(Biome::Plains, Biome::Desert);
    const auto mesh = mc::world::ChunkMesher::buildSection(world, {0, 0}, 0).mesh;

    // Far from the border, inside the window, the colour is the biome's own —
    // the 25 samples agree, which is also the early-out path.
    REQUIRE(topVertexTint(mesh, 5, 5)[0] == 0x00U);

    // Across the border the red channel rises monotonically and strictly, and
    // never jumps the whole way at once.
    int previous = -1;
    bool sawIntermediate = false;
    for (int x = 12; x <= 16; ++x) {
        const int red = topVertexTint(mesh, x, 5)[0];
        REQUIRE(red >= previous);
        if (red > 0x00 && red < 0x64) {
            sawIntermediate = true;
        }
        previous = red;
    }
    REQUIRE(sawIntermediate);
    // The window is vanilla's blend radius of 2, so the average is over the five
    // columns x-2..x+2 and the arithmetic is exact: a vertex three blocks inside
    // the western biome still sees none of the eastern colour, and one at the
    // border sees the three eastern columns of its five.
    REQUIRE(topVertexTint(mesh, 13, 5)[0] == 0x00U);
    REQUIRE(topVertexTint(mesh, 14, 5)[0] == 0x64U / 5U);
    REQUIRE(topVertexTint(mesh, 16, 5)[0] == 0x64U * 3U / 5U);
}

// A face at the chunk's edge names the column one block past it, which is what
// the halo covers. Without it the last column's tint clamps and the two sides of
// a chunk seam disagree — a visible line down the world.
void checkHaloAcrossChunkSeam() {
    mc::world::gen::setBiomeSurfaceColors(Biome::Plains, {0x000000U, 0U, 0U, 0U});
    mc::world::gen::setBiomeSurfaceColors(Biome::Desert, {0x640000U, 0U, 0U, 0U});
    const auto world = makeBorderWorld(Biome::Plains, Biome::Desert);
    const auto west = mc::world::ChunkMesher::buildSection(world, {0, 0}, 0).mesh;
    const auto east = mc::world::ChunkMesher::buildSection(world, {1, 0}, 0).mesh;
    // x = 16 is the seam: the last column of chunk 0 reaches it and the first
    // column of chunk 1 starts there. Both must resolve the same colour.
    REQUIRE(topVertexTint(west, 16, 5) == topVertexTint(east, 16, 5, 16, 0));
    REQUIRE(topVertexTint(west, 16, 9) == topVertexTint(east, 16, 9, 16, 0));
}

// Water takes the biome's water colour. It used to be tinted with one fixed blue
// in two places at once (the atlas frames and the fragment shader), so a swamp's
// murky green and a cold ocean's deep blue could not exist.
void checkWaterTint() {
    mc::world::gen::setBiomeSurfaceColors(Biome::Swamp,
                                          {0U, 0U, 0U, biomeDefinition(Biome::Swamp).waterColor});
    mc::world::gen::setBiomeSurfaceColors(Biome::Plains, {0U, 0U, 0U, 0x3F76E4U});
    World world;
    for (int chunkZ = -1; chunkZ <= 1; ++chunkZ) {
        for (int chunkX = -1; chunkX <= 1; ++chunkX) {
            Chunk chunk;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    chunk.setBlock(x, mc::world::kMinY + 1, z, Block::Water);
                    chunk.setColumnBiome(x, z, Biome::Swamp);
                }
            }
            world.setChunk({chunkX, chunkZ}, std::move(chunk));
        }
    }
    const auto mesh = mc::world::ChunkMesher::buildSection(world, {0, 0}, 0).translucentMesh;
    bool sawSwampWater = false;
    for (const auto& vertex : mesh.vertices) {
        if (vertex.normalIndex != 2U) {
            continue;
        }
        REQUIRE(vertex.tintR == ((6388580U >> 16U) & 0xFFU));
        REQUIRE(vertex.tintG == ((6388580U >> 8U) & 0xFFU));
        REQUIRE(vertex.tintB == (6388580U & 0xFFU));
        sawSwampWater = true;
    }
    REQUIRE(sawSwampWater);
}

// Every plant vanilla tints through the grass colour map has to be tinted here
// too. Leaving them out is not subtle: the mesher now reads the *untinted* atlas
// layers, so an untinted blade of grass renders as its raw greyscale texture,
// which on screen is white.
void checkPlantsAreTinted() {
    constexpr std::uint32_t kGrass = 0x91BD59U;
    mc::world::gen::setBiomeSurfaceColors(Biome::Plains, {kGrass, 0U, 0U, 0U});
    // Give the terrain grass layers non-zero ids so the mesher takes the
    // untinted-layer path the renderer takes.
    mc::world::gen::setTerrainGrassLayers(11.0F, 12.0F, 13.0F, 14.0F);

    constexpr std::array<Block, 5> kPlants{Block::GrassPlant, Block::TallGrass, Block::Fern,
                                           Block::LargeFern, Block::SugarCane};
    for (const auto plant : kPlants) {
        World world;
        for (int chunkZ = -1; chunkZ <= 1; ++chunkZ) {
            for (int chunkX = -1; chunkX <= 1; ++chunkX) {
                Chunk chunk;
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        chunk.setBlock(x, mc::world::kMinY + 1, z, Block::Grass);
                        chunk.setBlock(x, mc::world::kMinY + 2, z, plant);
                        chunk.setColumnBiome(x, z, Biome::Plains);
                    }
                }
                world.setChunk({chunkX, chunkZ}, std::move(chunk));
            }
        }
        const auto built = mc::world::ChunkMesher::buildSection(world, {0, 0}, 0);
        bool sawTintedPlant = false;
        for (const auto& mesh : {built.mesh, built.cutoutMesh, built.translucentMesh}) {
            for (const auto& vertex : mesh.vertices) {
                // The plant's own layer, not the grass block's faces.
                if (std::abs(static_cast<float>(vertex.textureLayer) - 12.0F) > 0.01F) {
                    continue;
                }
                REQUIRE(vertex.tintR == ((kGrass >> 16U) & 0xFFU));
                REQUIRE(vertex.tintG == ((kGrass >> 8U) & 0xFFU));
                REQUIRE(vertex.tintB == (kGrass & 0xFFU));
                sawTintedPlant = true;
            }
        }
        // short_grass is the one that uses the dedicated plant layer; the others
        // carry their own texture, so only assert reach for that one.
        if (plant == Block::GrassPlant) {
            REQUIRE(sawTintedPlant);
        }
        // Whatever layer they ended up on, no vertex of a tinted plant may be
        // left white — that is exactly the bug this test exists for.
        for (const auto& vertex : built.cutoutMesh.vertices) {
            const bool white = vertex.tintR == 255U && vertex.tintG == 255U &&
                               vertex.tintB == 255U;
            REQUIRE(!white);
        }
    }
    mc::world::gen::setTerrainGrassLayers(0.0F, 0.0F, 0.0F, 0.0F);
}

// A grass block's four sides carry a second, tinted quad — vanilla's overlay
// element. Without it the sides render as the plain dirt texture and the ground
// reads much darker from a distance.
void checkGrassSideOverlay() {
    mc::world::gen::setBiomeSurfaceColors(Biome::Plains, {0x91BD59U, 0U, 0U, 0U});
    mc::world::gen::setTerrainGrassLayers(11.0F, 12.0F, 13.0F, 14.0F);
    World world;
    Chunk chunk;
    // One grass block with air all round, so all four sides are drawn.
    chunk.setBlock(8, mc::world::kMinY + 4, 8, Block::Grass);
    chunk.setColumnBiome(8, 8, Biome::Plains);
    world.setChunk({0, 0}, std::move(chunk));
    const auto built = mc::world::ChunkMesher::buildSection(world, {0, 0}, 0);
    int overlayVertices = 0;
    for (const auto& vertex : built.cutoutMesh.vertices) {
        if (std::abs(static_cast<float>(vertex.textureLayer) - 14.0F) < 0.01F) {
            ++overlayVertices;
            REQUIRE(vertex.tintR == 0x91U);
            // The overlay never sits on the top or bottom face.
            REQUIRE(vertex.normalIndex != 2U && vertex.normalIndex != 3U);
        }
    }
    REQUIRE(overlayVertices == 16);  // four sides, four corners each
    REQUIRE(built.cutoutMesh.indices.size() == 24U);

    // The overlay must be EXACTLY coplanar with the dirt quad underneath it —
    // the same packed corner positions, to the bit.
    //
    // It used to be pushed 0.001 blocks along the face normal to win the depth
    // test. That cannot work at range: resolvable depth falls off as d^2, so on
    // a D32_SFLOAT buffer with a 0.1 near plane the offset drops below one float
    // ULP past roughly 40 blocks, the two quads quantise to the same depth, and
    // VK_COMPARE_OP_LESS then rejects the overlay — the grass edge on every
    // distant slope flickers back to the green baked into grass_block_side.png
    // as the camera moves. Coplanar geometry plus the cutout pipeline's
    // LESS_OR_EQUAL is distance-independent; grass_overlay_depth_test guards the
    // pipeline half, this guards the geometry half.
    std::multiset<std::array<std::uint16_t, 3>> basePositions;
    for (const auto& vertex : built.mesh.vertices) {
        // 13.0F is the terrain grass side layer set above; skip the top/bottom.
        if (std::abs(static_cast<float>(vertex.textureLayer) - 13.0F) > 0.01F) {
            continue;
        }
        basePositions.insert({vertex.positionX, vertex.positionY, vertex.positionZ});
    }
    std::multiset<std::array<std::uint16_t, 3>> overlayPositions;
    for (const auto& vertex : built.cutoutMesh.vertices) {
        if (std::abs(static_cast<float>(vertex.textureLayer) - 14.0F) > 0.01F) {
            continue;
        }
        overlayPositions.insert({vertex.positionX, vertex.positionY, vertex.positionZ});
    }
    REQUIRE(basePositions.size() == 16U);
    REQUIRE(overlayPositions == basePositions);
    mc::world::gen::setTerrainGrassLayers(0.0F, 0.0F, 0.0F, 0.0F);
}

// A chunk at the edge of the loaded area must not average in the "Plains"
// default World::biomeAt answers for an absent chunk: that paints a band of
// plains green along the edge, and the band moves as chunks stream in.
void checkUnloadedNeighbourDoesNotBleed() {
    mc::world::gen::setBiomeSurfaceColors(Biome::Plains, {0x00FF00U, 0U, 0U, 0U});
    mc::world::gen::setBiomeSurfaceColors(Biome::Desert, {0xBFB755U, 0U, 0U, 0U});
    // One chunk, no neighbours at all.
    World lonely;
    Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, mc::world::kMinY + 1, z, Block::Grass);
            chunk.setColumnBiome(x, z, Biome::Desert);
        }
    }
    lonely.setChunk({0, 0}, std::move(chunk));
    const auto mesh = mc::world::ChunkMesher::buildSection(lonely, {0, 0}, 0).mesh;
    // Every up-facing vertex, including the ones on the chunk border, must be
    // the desert's own colour — no plains green averaged in from the void.
    int checked = 0;
    for (const auto& vertex : mesh.vertices) {
        if (vertex.normalIndex != 2U) {
            continue;
        }
        REQUIRE(vertex.tintR == 0xBFU);
        REQUIRE(vertex.tintG == 0xB7U);
        REQUIRE(vertex.tintB == 0x55U);
        ++checked;
    }
    REQUIRE(checked > 0);
}

} // namespace

int main() {
    checkSpecialEffects();
    checkGrassColorModifier();
    checkBorderBlend();
    checkHaloAcrossChunkSeam();
    checkWaterTint();
    checkPlantsAreTinted();
    checkGrassSideOverlay();
    checkUnloadedNeighbourDoesNotBleed();
    return 0;
}

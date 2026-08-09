#include "world/ChunkMesher.hpp"

#include "world/WorldConstants.hpp"
#include "world/WorldLighting.hpp"
#include "world/gen/JavaRandom.hpp"
#include "world/gen/NoiseSampler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <glm/geometric.hpp>
#include <unordered_map>

namespace mc::world {
namespace {

using render::packVertex;

enum class Face : std::uint8_t { PositiveX, NegativeX, PositiveY, NegativeY, PositiveZ, NegativeZ };

struct FaceDefinition final {
    Face face;
    int dx;
    int dy;
    int dz;
    glm::vec3 normal;
    std::array<glm::vec3, 4> corners;
};

constexpr std::array<FaceDefinition, 6> kFaces{{
    {Face::PositiveX, 1, 0, 0, {1, 0, 0}, {{{1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}}}},
    {Face::NegativeX, -1, 0, 0, {-1, 0, 0}, {{{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}}},
    {Face::PositiveY, 0, 1, 0, {0, 1, 0}, {{{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}}},
    {Face::NegativeY, 0, -1, 0, {0, -1, 0}, {{{0, 0, 1}, {0, 0, 0}, {1, 0, 0}, {1, 0, 1}}}},
    {Face::PositiveZ, 0, 0, 1, {0, 0, 1}, {{{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}}},
    {Face::NegativeZ, 0, 0, -1, {0, 0, -1}, {{{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}}}},
}};

constexpr std::array<glm::vec2, 4> kUvs{{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
// Fixed layers of the atlas's special section: water still 0-31 / flow 32-63,
// furnace front 167 and the lit front 168. The block-texture layers after them
// are resolved at startup from the registry (world::textureLayers).
constexpr float kWaterStillLayer = 0.0F;
constexpr float kWaterFlowLayer = 32.0F;
constexpr float kFurnaceFrontLayer = 167.0F;
constexpr float kFurnaceFrontOnLayer = 168.0F;

[[nodiscard]] constexpr bool faceMatchesOrientation(Face face, BlockOrientation orientation) {
    switch (orientation) {
    case BlockOrientation::East: return face == Face::PositiveX;
    case BlockOrientation::West: return face == Face::NegativeX;
    case BlockOrientation::Up: return face == Face::PositiveY;
    case BlockOrientation::Down: return face == Face::NegativeY;
    case BlockOrientation::South: return face == Face::PositiveZ;
    case BlockOrientation::North: return face == Face::NegativeZ;
    }
    return false;
}

[[nodiscard]] constexpr bool faceSharesAxis(Face face, BlockOrientation orientation) {
    if (orientation == BlockOrientation::East || orientation == BlockOrientation::West) {
        return face == Face::PositiveX || face == Face::NegativeX;
    }
    if (orientation == BlockOrientation::Up || orientation == BlockOrientation::Down) {
        return face == Face::PositiveY || face == Face::NegativeY;
    }
    return face == Face::PositiveZ || face == Face::NegativeZ;
}

[[nodiscard]] constexpr glm::vec3 rotateLogModelVector(
    glm::vec3 value, BlockOrientation orientation) {
    // Mirror the blockstate model rotations used by 1.16.1 oak_log:
    // axis=x rotates the model's local Y pillar onto world X, and axis=z
    // rotates local Y onto world Z. East/West and North/South represent the
    // same undirected log axes.
    if (orientation == BlockOrientation::East ||
        orientation == BlockOrientation::West) {
        return {value.y, -value.x, value.z};
    }
    if (orientation == BlockOrientation::South ||
        orientation == BlockOrientation::North) {
        return {value.x, -value.z, value.y};
    }
    return value;
}

[[nodiscard]] FaceDefinition orientedModelFace(
    Block block, BlockOrientation orientation, const FaceDefinition& source) {
    if (!isLog(block) || orientation == BlockOrientation::Up ||
        orientation == BlockOrientation::Down) {
        return source;
    }
    FaceDefinition result = source;
    const glm::vec3 delta = rotateLogModelVector(
        glm::vec3{static_cast<float>(source.dx), static_cast<float>(source.dy),
                  static_cast<float>(source.dz)},
        orientation);
    result.dx = static_cast<int>(std::lround(delta.x));
    result.dy = static_cast<int>(std::lround(delta.y));
    result.dz = static_cast<int>(std::lround(delta.z));
    result.normal = rotateLogModelVector(source.normal, orientation);
    if (result.dx > 0) result.face = Face::PositiveX;
    else if (result.dx < 0) result.face = Face::NegativeX;
    else if (result.dy > 0) result.face = Face::PositiveY;
    else if (result.dy < 0) result.face = Face::NegativeY;
    else if (result.dz > 0) result.face = Face::PositiveZ;
    else result.face = Face::NegativeZ;
    for (auto& corner : result.corners) {
        corner = rotateLogModelVector(corner - glm::vec3{0.5F}, orientation) +
            glm::vec3{0.5F};
    }
    return result;
}

[[nodiscard]] float waterCellHeight(const World& world, int x, int y, int z) {
    if (!isFluid(world.block(x, y, z))) {
        return -1.0F;
    }
    if (isFluid(world.block(x, y + 1, z))) {
        return 1.0F;
    }
    const std::uint8_t level = world.fluidLevel(x, y, z);
    if (level >= 8U) {
        // Java's falling state is getFlowing(8, true), whose visible height is
        // still 8/9 unless another water cell is directly above it.
        return 8.0F / 9.0F;
    }
    return static_cast<float>(8U - level) / 9.0F;
}

[[nodiscard]] int floorDiv(int value, int divisor) {
    int quotient = value / divisor;
    const int remainder = value % divisor;
    if (remainder < 0) {
        --quotient;
    }
    return quotient;
}

// Whether the cell at worldX/worldZ lies inside a chunk the streamer has loaded.
// The water surface corners, flow direction and optical depth all sample one
// cell past a section border; a neighbour chunk that has not streamed in yet
// reads as Air, which would dip the whole row of surface corners along the seam
// and render it as a line of same-direction flowing water. Such a sample is
// treated as part of the current water body instead (the border section is
// remeshed once the neighbour arrives).
[[nodiscard]] bool waterSampleLoaded(const World& world, int worldX, int worldZ) {
    return world.hasChunk({floorDiv(worldX, kChunkWidth), floorDiv(worldZ, kChunkDepth)});
}

[[nodiscard]] float waterColumnDepth(
    const World& world,
    int x,
    int y,
    int z) {
    constexpr int maximumOpticalDepth = 32;
    int depth = 0;
    for (int sampleY = y;
         sampleY >= 0 && depth < maximumOpticalDepth &&
         isFluid(world.block(x, sampleY, z));
         --sampleY) {
        ++depth;
    }
    return static_cast<float>(std::max(depth, 1));
}

[[nodiscard]] float waterCornerColumnDepth(
    const World& world,
    int x,
    int y,
    int z,
    int cornerX,
    int cornerZ) {
    float total = 0.0F;
    float samples = 0.0F;
    for (int dz = cornerZ - 1; dz <= cornerZ; ++dz) {
        for (int dx = cornerX - 1; dx <= cornerX; ++dx) {
            const int sampleX = x + dx;
            const int sampleZ = z + dz;
            if (!waterSampleLoaded(world, sampleX, sampleZ)) {
                // Unstreamed neighbour chunk: reuse this cell's own depth so the
                // optical tint does not step at the chunk seam.
                total += waterColumnDepth(world, x, y, z);
                samples += 1.0F;
                continue;
            }
            if (!isFluid(world.block(sampleX, y, sampleZ))) {
                continue;
            }
            total += waterColumnDepth(world, sampleX, y, sampleZ);
            samples += 1.0F;
        }
    }
    return samples > 0.0F
        ? total / samples
        : waterColumnDepth(world, x, y, z);
}

[[nodiscard]] float waterDepthBelowSurface(
    const World& world,
    int x,
    int y,
    int z) {
    constexpr int maximumOpticalDepth = 32;
    int depth = 1;
    for (int sampleY = y + 1;
         sampleY < kWorldHeight && depth < maximumOpticalDepth &&
         isFluid(world.block(x, sampleY, z));
         ++sampleY) {
        ++depth;
    }
    return static_cast<float>(depth);
}

[[nodiscard]] float waterCornerHeight(
    const World& world,
    int x,
    int y,
    int z,
    int cornerX,
    int cornerZ) {
    float total = 0.0F;
    float weight = 0.0F;
    for (int dz = cornerZ - 1; dz <= cornerZ; ++dz) {
        for (int dx = cornerX - 1; dx <= cornerX; ++dx) {
            const int sampleX = x + dx;
            const int sampleZ = z + dz;
            const float height = waterSampleLoaded(world, sampleX, sampleZ)
                ? waterCellHeight(world, sampleX, y, sampleZ)
                // A neighbour chunk that has not streamed in yet reads as Air
                // and would push this corner to zero, breaking the water
                // surface into a straight crack along the chunk seam. Sample
                // the current cell instead so the surface stays flat until the
                // border is remeshed with real data.
                : waterCellHeight(world, x, y, z);
            if (height < 0.0F) {
                // FluidRenderer counts non-solid empty neighbors as a zero-
                // height sample. Skipping them kept edge corners artificially
                // high and produced the tall triangular spikes seen around a
                // newly opened seabed hole.
                if (!hasCollision(world.block(sampleX, y, sampleZ))) {
                    weight += 1.0F;
                }
                continue;
            }
            if (height >= 1.0F) {
                return 1.0F;
            }
            const float sampleWeight = height >= 0.8F ? 10.0F : 1.0F;
            total += height * sampleWeight;
            weight += sampleWeight;
        }
    }
    return weight > 0.0F ? total / weight : 0.0F;
}

[[nodiscard]] glm::vec2 waterFlowDirection(
    const World& world,
    int x,
    int y,
    int z) {
    const float center = waterCellHeight(world, x, y, z);
    const auto heightOrCenter = [&](int sampleX, int sampleZ) {
        if (!waterSampleLoaded(world, sampleX, sampleZ)) {
            // An unstreamed neighbour chunk reads as Air and would point the
            // flow straight at the gap; keep the whole border row flat until
            // the section is remeshed with the real neighbour.
            return center;
        }
        const float value = waterCellHeight(world, sampleX, y, sampleZ);
        if (value >= 0.0F) {
            return value;
        }
        // Replaceable empty cells are valid flow destinations and therefore
        // contribute zero height. Solid walls retain the center height so the
        // visual velocity never points through them.
        return isReplaceable(world.block(sampleX, y, sampleZ)) ? 0.0F : center;
    };
    return {
        heightOrCenter(x - 1, z) - heightOrCenter(x + 1, z),
        heightOrCenter(x, z - 1) - heightOrCenter(x, z + 1),
    };
}

[[nodiscard]] glm::vec2 flowingWaterUv(
    std::size_t cornerIndex,
    glm::vec2 direction) {
    const float lengthSquared = direction.x * direction.x + direction.y * direction.y;
    if (lengthSquared < 0.0001F) {
        return kUvs[cornerIndex];
    }
    const float angle = std::atan2(direction.y, direction.x) - 1.57079632679F;
    const float sine = std::sin(angle) * 0.25F;
    const float cosine = std::cos(angle) * 0.25F;
    const std::array<glm::vec2, 4> exactUvs{{
        {0.5F - cosine - sine, 0.5F - cosine + sine},
        {0.5F - cosine + sine, 0.5F + cosine + sine},
        {0.5F + cosine + sine, 0.5F + cosine - sine},
        {0.5F + cosine - sine, 0.5F - cosine - sine},
    }};
    // FluidRenderer pulls all four coordinates 4/sourceTextureSize toward
    // their average to avoid atlas bleeding. water_flow is 32x32 in 1.16.1.
    constexpr float centerPull = 4.0F / 32.0F;
    // The source sprite is 32x32, while each block-texture array layer is
    // 16x16. Compensate for that downsample or these centered coordinates
    // enlarge the flowing-water pattern by another factor of two.
    constexpr float sourceToArrayScale = 2.0F;
    return glm::vec2{0.5F} +
        (exactUvs[cornerIndex] - glm::vec2{0.5F}) *
            (1.0F - centerPull) * sourceToArrayScale;
}

// SwampBiome's grass mottle, seeded exactly like 1.16.1's FOLIAGE_NOISE: a
// swamp block whose noise drops below -0.1 uses the darker tone.
[[nodiscard]] bool swampDarkTone(int x, int z) {
    static const gen::SimplexNoiseSampler sampler = [] {
        gen::JavaRandom random{1234ULL};
        return gen::SimplexNoiseSampler{random};
    }();
    return sampler.sample(static_cast<double>(x) * 0.0225,
                          static_cast<double>(z) * 0.0225) < -0.1;
}

// The biome whose grass/foliage colour dominates at a block position, chosen
// bilinearly over the four surrounding 1:4 cells. The terrain grass and foliage
// pick their BAKED atlas layer from this biome, so the colour boundary follows
// the same smooth line the surface-material pass uses — no per-vertex tint is
// involved, so the rendered colour cannot wash out into the raw grey texture.
[[nodiscard]] gen::Biome dominantBiome(const World& world, int x, int z) {
    const int cellX = floorDiv(x, 4);
    const int cellZ = floorDiv(z, 4);
    const float fractionalX = static_cast<float>(x - cellX * 4) * 0.25F;
    const float fractionalZ = static_cast<float>(z - cellZ * 4) * 0.25F;
    const std::array<gen::Biome, 4> cells{{
        world.biomeAt(cellX * 4 + 2, cellZ * 4 + 2),
        world.biomeAt((cellX + 1) * 4 + 2, cellZ * 4 + 2),
        world.biomeAt(cellX * 4 + 2, (cellZ + 1) * 4 + 2),
        world.biomeAt((cellX + 1) * 4 + 2, (cellZ + 1) * 4 + 2),
    }};
    const std::array<float, 4> weights{{
        (1.0F - fractionalX) * (1.0F - fractionalZ),
        fractionalX * (1.0F - fractionalZ),
        (1.0F - fractionalX) * fractionalZ,
        fractionalX * fractionalZ,
    }};
    int best = 0;
    for (int index = 1; index < 4; ++index) {
        if (weights[static_cast<std::size_t>(index)] >
            weights[static_cast<std::size_t>(best)]) {
            best = index;
        }
    }
    return cells[static_cast<std::size_t>(best)];
}

// The per-vertex biome tint is disabled: grass and foliage colours are baked
// into their atlas layers at build time, so every vertex tint is white. The
// class stays as the parameter appendFace and appendCrossedPlant carry.
class BiomeTintCache final {
  public:
    explicit BiomeTintCache(const World&) {}

    [[nodiscard]] std::array<std::uint8_t, 3> tint(Block, Face, int, int) {
        return {255U, 255U, 255U};
    }
};

[[nodiscard]] float textureLayer(const World& world, Block block, Face face,
                                 int x, int y, int z) {
    auto layers = textureLayers(block);
    if (block == Block::Grass) {
        // The grass family's BAKED per-biome layers: the top/plant/side are
        // tinted with the biome's colour at build time, so the colour never
        // depends on per-vertex data reaching the fragment shader.
        const auto biome = dominantBiome(world, x, z);
        const auto& biomeLayers = (biome == gen::Biome::Swamp && swampDarkTone(x, z))
            ? gen::swampDarkGrassLayers()
            : gen::biomeGrassLayers(biome);
        if (biomeLayers.top != 0.0F) {
            layers.top = biomeLayers.top;
            layers.side = biomeLayers.side;
        }
    } else if (isLeaves(block)) {
        // Oak-family leaves use the baked per-biome foliage layer; spruce/birch
        // keep their fixed tinted terrain layer.
        if (block != Block::SpruceLeaves && block != Block::BirchLeaves) {
            const float foliage =
                gen::biomeFoliageLayer(dominantBiome(world, x, z), block);
            if (foliage != 0.0F) {
                layers.top = layers.side = layers.bottom = foliage;
            }
        } else {
            const float terrain = gen::terrainLeafLayer(block);
            if (terrain != 0.0F) {
                layers.top = layers.side = layers.bottom = terrain;
            }
        }
    }
    const auto orientation = world.orientation(x, y, z);
    if (block == Block::Furnace && faceMatchesOrientation(face, orientation)) {
        return kFurnaceFrontLayer;
    }
    if (block == Block::LitFurnace && faceMatchesOrientation(face, orientation)) {
        return kFurnaceFrontOnLayer;
    }
    if (isLog(block)) {
        return faceSharesAxis(face, orientation) ? layers.top : layers.side;
    }
    if (block == Block::Farmland && face == Face::PositiveY &&
        farmlandMoisture(orientation) == 7) {
        // FarmlandBlock.MOISTURE: the wet texture appears only at moisture 7,
        // exactly like the 1.16.1 blockstate (every lower level is dry). The
        // moist face sits right after the dry one in the registry-built atlas.
        return textureLayers(Block::Farmland).top + 1.0F;
    }
    if (face == Face::PositiveY) {
        return layers.top;
    }
    if (face == Face::NegativeY) {
        return layers.bottom;
    }
    return layers.side;
}

[[nodiscard]] float cornerCoordinate(
    const glm::vec3& corner,
    const glm::ivec3& axis) {
    if (axis.x != 0) {
        return corner.x;
    }
    if (axis.y != 0) {
        return corner.y;
    }
    return corner.z;
}

struct VertexLight final {
    float sky = 1.0F;
    float block = 0.0F;
};

// The four sample positions a corner averages. They are the 2×2 ring of cells
// around the corner in the face plane, chosen canonically from the face normal
// and the corner's in-plane sign, so the same world-space corner samples the
// same four cells whichever adjacent block's face is being meshed. That
// agreement is what keeps the smooth gradient continuous across block
// boundaries instead of snapping at the shared edge.
struct CornerPositions final {
    glm::ivec3 outside;
    glm::ivec3 sideA;
    glm::ivec3 sideB;
    glm::ivec3 diagonal;
};

[[nodiscard]] CornerPositions cornerPositions(
    const FaceDefinition& face, const glm::vec3& corner, int x, int y, int z) {
    glm::ivec3 tangentA;
    glm::ivec3 tangentB;
    if (face.dx != 0) {
        tangentA = {0, 1, 0};
        tangentB = {0, 0, 1};
    } else if (face.dy != 0) {
        tangentA = {1, 0, 0};
        tangentB = {0, 0, 1};
    } else {
        tangentA = {1, 0, 0};
        tangentB = {0, 1, 0};
    }
    const int signA = cornerCoordinate(corner, tangentA) < 0.5F ? -1 : 1;
    const int signB = cornerCoordinate(corner, tangentB) < 0.5F ? -1 : 1;
    const glm::ivec3 outside{x + face.dx, y + face.dy, z + face.dz};
    return {
        outside,
        outside + tangentA * signA,
        outside + tangentB * signB,
        outside + tangentA * signA + tangentB * signB,
    };
}

// Vanilla 1.16.1 getAmbientOcclusionLightLevel: a full opaque cube darkens the
// corner to 0.2, everything else keeps full brightness. The corner averages the
// four ring cells symmetrically — no per-block corner selection — so adjacent
// blocks agree exactly on shared corners and the gradient stays smooth.
template <typename Sampler>
[[nodiscard]] float vertexAmbientOcclusionHigh(const Sampler& lighting,
                                               const CornerPositions& positions) {
    const auto aoFactor = [&](const glm::ivec3& position) {
        return lighting.aoOccludes(position.x, position.y, position.z) ? 0.2F : 1.0F;
    };
    return (aoFactor(positions.outside) + aoFactor(positions.sideA) +
            aoFactor(positions.sideB) + aoFactor(positions.diagonal)) * 0.25F;
}

template <typename Sampler>
[[nodiscard]] VertexLight vertexLightHigh(const Sampler& lighting,
                                          const CornerPositions& positions) {
    const auto lightAt = [&](const glm::ivec3& position) {
        return lighting.level(position.x, position.y, position.z);
    };
    const auto outside = lightAt(positions.outside);
    const auto sideA = lightAt(positions.sideA);
    const auto sideB = lightAt(positions.sideB);
    const auto diagonal = lightAt(positions.diagonal);
    constexpr float normalization = 1.0F /
        (4.0F * static_cast<float>(ChunkLightSampler::kMaximumLightLevel));
    return {
        static_cast<float>(outside.sky + sideA.sky + sideB.sky + diagonal.sky) *
            normalization,
        static_cast<float>(outside.block + sideA.block + sideB.block +
                           diagonal.block) * normalization,
    };
}

// Standard tier: the current binary-AO algorithm, byte-for-byte unchanged.
template <typename Sampler>
[[nodiscard]] float vertexAmbientOcclusionStandard(const Sampler& lighting,
                                                   const CornerPositions& positions) {
    const bool occupiedA =
        lighting.isOpaque(positions.sideA.x, positions.sideA.y, positions.sideA.z);
    const bool occupiedB =
        lighting.isOpaque(positions.sideB.x, positions.sideB.y, positions.sideB.z);
    const bool occupiedDiagonal = lighting.isOpaque(
        positions.diagonal.x, positions.diagonal.y, positions.diagonal.z);
    // Java's smooth AO averages the neighboring light/brightness samples at
    // each corner rather than applying a binary top-down shadow. Opaque
    // samples keep a little reflected light, which removes the hard black
    // staircase visible on adjacent faces.
    constexpr float blockedBrightness = 0.35F;
    const float sideABrightness = occupiedA ? blockedBrightness : 1.0F;
    const float sideBBrightness = occupiedB ? blockedBrightness : 1.0F;
    const float diagonalBrightness = occupiedA && occupiedB
        ? std::min(sideABrightness, sideBBrightness)
        : (occupiedDiagonal ? blockedBrightness : 1.0F);
    return (1.0F + sideABrightness + sideBBrightness + diagonalBrightness) * 0.25F;
}

template <typename Sampler>
[[nodiscard]] VertexLight vertexLightStandard(
    const Sampler& lighting, const CornerPositions& positions,
    VoxelLightLevel outsideLight) {
    const bool occupiedA =
        lighting.isOpaque(positions.sideA.x, positions.sideA.y, positions.sideA.z);
    const bool occupiedB =
        lighting.isOpaque(positions.sideB.x, positions.sideB.y, positions.sideB.z);
    const auto sideALight = lighting.level(positions.sideA.x, positions.sideA.y, positions.sideA.z);
    const auto sideBLight = lighting.level(positions.sideB.x, positions.sideB.y, positions.sideB.z);
    const auto diagonalLight = occupiedA && occupiedB
        ? VoxelLightLevel{
              std::min(sideALight.sky, sideBLight.sky),
              std::min(sideALight.block, sideBLight.block)}
        : lighting.level(positions.diagonal.x, positions.diagonal.y, positions.diagonal.z);
    constexpr float normalization = 1.0F /
        (4.0F * static_cast<float>(ChunkLightSampler::kMaximumLightLevel));
    return {
        static_cast<float>(outsideLight.sky + sideALight.sky +
                           sideBLight.sky + diagonalLight.sky) * normalization,
        static_cast<float>(outsideLight.block + sideALight.block +
                           sideBLight.block + diagonalLight.block) * normalization,
    };
}

template <typename Sampler>
[[nodiscard]] float vertexAmbientOcclusion(
    const Sampler& lighting, SmoothLightingQuality quality,
    const FaceDefinition& face, const glm::vec3& corner, int x, int y, int z) {
    const auto positions = cornerPositions(face, corner, x, y, z);
    if (quality == SmoothLightingQuality::High) {
        return vertexAmbientOcclusionHigh(lighting, positions);
    }
    return vertexAmbientOcclusionStandard(lighting, positions);
}

template <typename Sampler>
[[nodiscard]] VertexLight vertexLight(
    const Sampler& lighting, SmoothLightingQuality quality,
    const FaceDefinition& face, const glm::vec3& corner, int x, int y, int z,
    VoxelLightLevel outsideLight) {
    const auto positions = cornerPositions(face, corner, x, y, z);
    if (quality == SmoothLightingQuality::High) {
        return vertexLightHigh(lighting, positions);
    }
    return vertexLightStandard(lighting, positions, outsideLight);
}

template <typename Sampler>
void appendFace(
    render::MeshData& mesh,
    const World& world,
    Block block,
    const FaceDefinition& face,
    int x,
    int y,
    int z,
    const Sampler& lighting,
    SmoothLightingQuality quality,
    const glm::vec3& sectionOrigin,
    BiomeTintCache& tints,
    bool doubleSided = false) {
    const auto firstVertex = static_cast<std::uint32_t>(mesh.vertices.size());
    const glm::vec3 origin{
        static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    // A truncated block (farmland) lowers the top of its solid box. The AO and
    // light samples keep the canonical corner (they only read the surrounding
    // cells, which a 1/16 drop does not change); only the mesh position drops.
    const float modelHeight = blockDefinition(block).modelHeight;
    std::array<float, 4> ambientOcclusion{};
    // The center sample (pos + face normal) doubles as the flat light and is
    // shared by all four corners, so it is read once per face.
    const auto outsideLight = lighting.level(
        x + face.dx, y + face.dy, z + face.dz);
    const float flatSky = static_cast<float>(outsideLight.sky) /
        static_cast<float>(ChunkLightSampler::kMaximumLightLevel);
    const float flatBlock = emittedLight(block) > 0U
        ? 1.0F
        : static_cast<float>(outsideLight.block) /
              static_cast<float>(ChunkLightSampler::kMaximumLightLevel);
    // Hoisted out of the corner loop: textureLayer probes world orientation,
    // so this reads it once per face instead of once per corner.
    const float layer = textureLayer(world, block, face.face, x, y, z);
    for (std::size_t corner = 0; corner < face.corners.size(); ++corner) {
        ambientOcclusion[corner] = vertexAmbientOcclusion(
            lighting, quality, face, face.corners[corner], x, y, z);
        auto smoothLight = vertexLight(
            lighting, quality, face, face.corners[corner], x, y, z, outsideLight);
        if (emittedLight(block) > 0U) smoothLight.block = 1.0F;
        glm::vec3 positionCorner = face.corners[corner];
        if (modelHeight < 1.0F) {
            positionCorner.y *= modelHeight;
        }
        // Which biome-colour lookup the fragment shader should apply: 1 for the
        // grass top/plant (grass colour map), 2 for oak-family leaves (foliage
        // colour map), 0 otherwise (the side keeps its baked layer, spruce/birch
        // their fixed tones, everything else is untouched). The mask rides the
        // vertex pad byte, which shares an attribute with the normal index that
        // is proven to round-trip.
        // The per-vertex biome mask is disabled: grass/leaf colours are baked
        // into their atlas layers, so the fragment shader's lookup is unused.
        const std::uint8_t biomeMask = 0U;
        const int cornerX = x + static_cast<int>(std::lround(positionCorner.x));
        const int cornerZ = z + static_cast<int>(std::lround(positionCorner.z));
        const auto tint = tints.tint(block, face.face, cornerX, cornerZ);
        mesh.vertices.push_back(packVertex(
            (origin + positionCorner) - sectionOrigin,
            face.normal,
            kUvs[corner],
            layer,
            ambientOcclusion[corner],
            1.0F,
            smoothLight.sky,
            smoothLight.block,
            flatSky,
            flatBlock,
            tint[0],
            tint[1],
            tint[2],
            biomeMask));
    }
    constexpr std::array<std::uint32_t, 6> kDefaultIndices{0, 1, 2, 2, 3, 0};
    constexpr std::array<std::uint32_t, 6> kFlippedIndices{0, 1, 3, 1, 2, 3};
    const auto& indices = ambientOcclusion[0] + ambientOcclusion[2] >
                                  ambientOcclusion[1] + ambientOcclusion[3]
                              ? kFlippedIndices
                              : kDefaultIndices;
    for (const auto index : indices) {
        mesh.indices.push_back(firstVertex + index);
    }
    if (doubleSided) {
        for (auto iterator = indices.rbegin(); iterator != indices.rend(); ++iterator) {
            mesh.indices.push_back(firstVertex + *iterator);
        }
    }
}

template <typename Sampler>
void appendWaterFace(
    render::MeshData& mesh,
    const World& world,
    const FaceDefinition& face,
    int x,
    int y,
    int z,
    const Sampler& lighting,
    SmoothLightingQuality quality,
    const glm::vec3& sectionOrigin) {
    const auto firstVertex = static_cast<std::uint32_t>(mesh.vertices.size());
    const glm::vec3 origin{
        static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    const glm::vec2 flowDirection = waterFlowDirection(world, x, y, z);
    const bool topFace = face.face == Face::PositiveY;
    const float opticalDepth = topFace
        ? waterColumnDepth(world, x, y, z)
        : waterDepthBelowSurface(world, x, y, z);
    const bool flowing = world.fluidLevel(x, y, z) != 0U ||
        flowDirection.x * flowDirection.x + flowDirection.y * flowDirection.y > 0.000001F;
    const auto flatLight = lighting.level(
        x + face.dx, y + face.dy, z + face.dz);
    for (std::size_t cornerIndex = 0; cornerIndex < face.corners.size(); ++cornerIndex) {
        glm::vec3 corner = face.corners[cornerIndex];
        if (corner.y > 0.5F) {
            corner.y = waterCornerHeight(
                world,
                x,
                y,
                z,
                static_cast<int>(std::lround(corner.x)),
                static_cast<int>(std::lround(corner.z)));
        }
        glm::vec2 uv = kUvs[cornerIndex];
        if (topFace && flowing) {
            uv = flowingWaterUv(cornerIndex, flowDirection);
        }
        const float vertexOpticalDepth = topFace
            ? waterCornerColumnDepth(
                  world,
                  x,
                  y,
                  z,
                  static_cast<int>(std::lround(corner.x)),
                  static_cast<int>(std::lround(corner.z)))
            : opticalDepth;
        const auto smoothLight = vertexLight(
            lighting, quality, face, face.corners[cornerIndex], x, y, z, flatLight);
        mesh.vertices.push_back(packVertex(
            (origin + corner) - sectionOrigin,
            face.normal,
            uv,
            topFace ? (flowing ? kWaterFlowLayer : kWaterStillLayer)
                    : kWaterFlowLayer,
            1.0F,
            vertexOpticalDepth,
            smoothLight.sky,
            smoothLight.block,
            static_cast<float>(flatLight.sky) / 15.0F,
            static_cast<float>(flatLight.block) / 15.0F));
    }
    constexpr std::array<std::uint32_t, 6> indices{0, 1, 2, 2, 3, 0};
    for (const auto index : indices) {
        mesh.indices.push_back(firstVertex + index);
    }
}

[[nodiscard]] bool shouldRenderFace(
    Block current, Block neighbor, const FaceDefinition& face) {
    if (!isRenderable(neighbor)) {
        return true;
    }
    const auto currentLayer = blockDefinition(current).renderLayer;
    const auto neighborLayer = blockDefinition(neighbor).renderLayer;
    // A truncated neighbor (farmland's 15/16 box) does not fill the whole cell,
    // so it leaves a sliver of the shared face exposed above its top. Culling
    // the face against it would leave a see-through gap at that sliver; keep the
    // face so the neighbour's side stays visible above the farmland surface.
    if (blockDefinition(neighbor).modelHeight < 1.0F) {
        return true;
    }
    if (currentLayer == BlockRenderLayer::Translucent) {
        return neighborLayer == BlockRenderLayer::Translucent && neighbor != current;
    }
    if (currentLayer == BlockRenderLayer::Cutout) {
        if (isLeaves(current) && neighbor == current) {
            // Keep one deterministic, double-sided internal sheet. Keeping
            // both produces coplanar z-fighting; removing both makes the
            // canopy as hollow as glass.
            return face.dx + face.dy + face.dz > 0;
        }
        return neighbor != current && neighborLayer != BlockRenderLayer::Opaque;
    }
    return neighborLayer != BlockRenderLayer::Opaque;
}

// AbstractBlock#getModelOffset for OffsetType.XZ/XYZ: a deterministic jitter
// baked into the vertex positions, so each plant sits a few pixels off its
// block centre with a per-position magnitude and direction, the way vanilla
// draws flowers and grass. `l` is MathHelper.hashCode(x, 0, z); only its low
// bits feed the offset, so an unsigned right shift replicates Java's
// arithmetic `l >> 16` exactly.
[[nodiscard]] glm::vec3 plantModelOffset(BlockOffsetType offsetType, int x, int z) {
    // x * 3129871 wraps in 32 bits (Java int), then sign-extends into the XOR.
    const std::uint32_t xi = static_cast<std::uint32_t>(x) * 3129871U;
    std::uint64_t l = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(static_cast<std::int32_t>(xi)) ^
        static_cast<std::int64_t>(z) * 116129781LL);
    l = l * l * 42317861ULL + l * 11ULL;
    const std::uint64_t bits = l >> 16;
    const float xOffset = (static_cast<float>(bits & 15ULL) / 15.0F - 0.5F) * 0.5F;
    const float yOffset = offsetType == BlockOffsetType::XYZ
        ? (static_cast<float>((bits >> 4) & 15ULL) / 15.0F - 1.0F) * 0.2F
        : 0.0F;
    const float zOffset = (static_cast<float>((bits >> 8) & 15ULL) / 15.0F - 0.5F) * 0.5F;
    return {xOffset, yOffset, zOffset};
}

template <typename Sampler>
void appendPlantQuad(
    render::MeshData& mesh,
    float layer,
    const std::array<glm::vec3, 4>& corners,
    int x,
    int y,
    int z,
    const Sampler& lighting,
    const glm::vec3& sectionOrigin,
    const std::array<std::uint8_t, 3>& tint = {255U, 255U, 255U},
    std::uint8_t biomeMask = 0U) {
    const auto firstVertex = static_cast<std::uint32_t>(mesh.vertices.size());
    const glm::vec3 origin{
        static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    const auto light = lighting.level(x, y, z);
    const float sky = static_cast<float>(light.sky) / 15.0F;
    const float blockLight = static_cast<float>(light.block) / 15.0F;
    for (std::size_t corner = 0; corner < corners.size(); ++corner) {
        mesh.vertices.push_back(packVertex(
            (origin + corners[corner]) - sectionOrigin,
            {0.0F, 1.0F, 0.0F},
            kUvs[corner],
            layer,
            1.0F,
            0.0F,
            sky,
            blockLight,
            sky,
            blockLight,
            tint[0],
            tint[1],
            tint[2],
            biomeMask));
    }
    // Cross models must remain visible from both directions after enabling
    // Java-style back-face culling for the shared cutout render layer.
    constexpr std::array<std::uint32_t, 12> indices{
        0, 1, 2, 2, 3, 0,
        2, 1, 0, 0, 3, 2,
    };
    for (const auto index : indices) {
        mesh.indices.push_back(firstVertex + index);
    }
}

template <typename Sampler>
void appendCrossedPlant(
    render::MeshData& mesh,
    const World& world,
    Block block,
    int x,
    int y,
    int z,
    const Sampler& lighting,
    const glm::vec3& sectionOrigin,
    float layer,
    BiomeTintCache& tints) {
    // OffsetType.None pins the plant to its block centre; XZ/XYZ shift the two
    // crossed quads by the vanilla per-position jitter below. `layer` is the
    // texture-array layer of the plant — a fixed layer for flowers and grass,
    // the crop's stage layer when called from the Crop path.
    const auto& definition = blockDefinition(block);
    const glm::vec3 offset = definition.offsetType == BlockOffsetType::None
        ? glm::vec3{0.0F}
        : plantModelOffset(definition.offsetType, x, z);
    constexpr std::array<glm::vec3, 4> first{{
        {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 1.0F},
        {1.0F, 1.0F, 1.0F}, {0.0F, 1.0F, 0.0F},
    }};
    constexpr std::array<glm::vec3, 4> second{{
        {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F},
        {0.0F, 1.0F, 1.0F}, {1.0F, 1.0F, 0.0F},
    }};
    std::array<glm::vec3, 4> shiftedFirst = first;
    std::array<glm::vec3, 4> shiftedSecond = second;
    for (auto& corner : shiftedFirst) corner += offset;
    for (auto& corner : shiftedSecond) corner += offset;
    // Tall grass follows the biome grass tint; flowers and other cross plants
    // keep white. The thin crossed quads span the block, so one tint per block
    // reads the same as per corner.
    const auto tint = tints.tint(block, Face::PositiveY, x, z);
    appendPlantQuad(mesh, layer, shiftedFirst, x, y, z, lighting, sectionOrigin, tint);
    appendPlantQuad(mesh, layer, shiftedSecond, x, y, z, lighting, sectionOrigin, tint);
}

// The vanilla `crop` blockstate model (crop.json): four orthogonal thin planes
// at the quarter offsets x=4/16, x=12/16, z=4/16 and z=12/16, rather than the
// two diagonal planes of a `cross`. Each plane is double-sided, so a crop reads
// as a solid "+" grid from any angle instead of a thin X. The planes span y
// -1/16 to 15/16 like the vanilla model, so the plant's base sinks to the
// farmland surface below the crop cell instead of floating a sixteenth up.
template <typename Sampler>
void appendCropPlant(
    render::MeshData& mesh,
    float layer,
    int x,
    int y,
    int z,
    const Sampler& lighting,
    const glm::vec3& sectionOrigin) {
    constexpr float kPlaneBottom = -0.0625F;
    constexpr float kPlaneTop = 0.9375F;
    constexpr std::array<glm::vec3, 4> planeXLow{{
        {0.25F, kPlaneBottom, 0.0F}, {0.25F, kPlaneBottom, 1.0F},
        {0.25F, kPlaneTop, 1.0F}, {0.25F, kPlaneTop, 0.0F},
    }};
    constexpr std::array<glm::vec3, 4> planeXHigh{{
        {0.75F, kPlaneBottom, 0.0F}, {0.75F, kPlaneBottom, 1.0F},
        {0.75F, kPlaneTop, 1.0F}, {0.75F, kPlaneTop, 0.0F},
    }};
    constexpr std::array<glm::vec3, 4> planeZLow{{
        {0.0F, kPlaneBottom, 0.25F}, {1.0F, kPlaneBottom, 0.25F},
        {1.0F, kPlaneTop, 0.25F}, {0.0F, kPlaneTop, 0.25F},
    }};
    constexpr std::array<glm::vec3, 4> planeZHigh{{
        {0.0F, kPlaneBottom, 0.75F}, {1.0F, kPlaneBottom, 0.75F},
        {1.0F, kPlaneTop, 0.75F}, {0.0F, kPlaneTop, 0.75F},
    }};
    appendPlantQuad(mesh, layer, planeXLow, x, y, z, lighting, sectionOrigin);
    appendPlantQuad(mesh, layer, planeXHigh, x, y, z, lighting, sectionOrigin);
    appendPlantQuad(mesh, layer, planeZLow, x, y, z, lighting, sectionOrigin);
    appendPlantQuad(mesh, layer, planeZHigh, x, y, z, lighting, sectionOrigin);
}

void appendTorchQuad(
    render::MeshData& mesh,
    const std::array<glm::vec3, 4>& positions,
    const glm::vec3& normal,
    const std::array<glm::vec2, 4>& uvs,
    float textureLayer,
    float skyLight,
    float blockLight,
    const glm::vec3& sectionOrigin) {
    const auto firstVertex = static_cast<std::uint32_t>(mesh.vertices.size());
    for (std::size_t corner = 0; corner < positions.size(); ++corner) {
        mesh.vertices.push_back(packVertex(
            positions[corner] - sectionOrigin, normal, uvs[corner], textureLayer, 1.0F, 0.0F,
            skyLight, blockLight, skyLight, blockLight));
    }
    constexpr std::array<std::uint32_t, 6> indices{0, 1, 2, 2, 3, 0};
    for (const auto index : indices) mesh.indices.push_back(firstVertex + index);
}

template <typename Sampler>
void appendTorchModel(
    render::MeshData& mesh,
    Block block,
    int x,
    int y,
    int z,
    float textureLayer,
    const Sampler& lighting,
    const glm::vec3& sectionOrigin) {
    glm::vec3 facing{0.0F};
    if (block == Block::WallTorchNorth) facing.z = -1.0F;
    if (block == Block::WallTorchEast) facing.x = 1.0F;
    if (block == Block::WallTorchSouth) facing.z = 1.0F;
    if (block == Block::WallTorchWest) facing.x = -1.0F;
    const bool wall = block != Block::Torch;
    const float skyLight = lighting.sky(x, y, z);
    const float blockLight = static_cast<float>(emittedLight(block)) / 15.0F;
    const glm::vec3 origin{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    const glm::vec3 base = wall
        ? origin + glm::vec3{0.5F, 0.18F, 0.5F} - facing * kWallTorchInset
        : origin + glm::vec3{0.5F, 0.0F, 0.5F};
    const glm::vec3 axis = wall
        ? glm::vec3{facing.x * 0.28F, 0.58F, facing.z * 0.28F}
        : glm::vec3{0.0F, 0.625F, 0.0F};
    const glm::vec3 up = glm::normalize(axis);
    const glm::vec3 right = wall
        ? glm::normalize(glm::vec3{facing.z, 0.0F, -facing.x})
        : glm::vec3{1.0F, 0.0F, 0.0F};
    const glm::vec3 forward = glm::normalize(glm::cross(right, up));
    constexpr float halfWidth = 1.0F / 16.0F;
    const glm::vec3 r = right * halfWidth;
    const glm::vec3 f = forward * halfWidth;
    const std::array<glm::vec3, 4> bottom{
        base - r - f, base + r - f, base + r + f, base - r + f};
    const std::array<glm::vec3, 4> top{
        bottom[0] + axis, bottom[1] + axis, bottom[2] + axis, bottom[3] + axis};
    constexpr float pixel = 1.0F / 16.0F;
    constexpr std::array<glm::vec2, 4> sideUvs{{
        {7.0F * pixel, 1.0F}, {7.0F * pixel, 6.0F * pixel},
        {9.0F * pixel, 6.0F * pixel}, {9.0F * pixel, 1.0F},
    }};
    constexpr std::array<glm::vec2, 4> topUvs{{
        {7.0F * pixel, 8.0F * pixel}, {9.0F * pixel, 8.0F * pixel},
        {9.0F * pixel, 6.0F * pixel}, {7.0F * pixel, 6.0F * pixel},
    }};
    constexpr std::array<glm::vec2, 4> bottomUvs{{
        {7.0F * pixel, 15.0F * pixel}, {9.0F * pixel, 15.0F * pixel},
        {9.0F * pixel, 13.0F * pixel}, {7.0F * pixel, 13.0F * pixel},
    }};
    appendTorchQuad(
        mesh, {bottom[0], bottom[1], bottom[2], bottom[3]}, -up, bottomUvs,
        textureLayer, skyLight, blockLight, sectionOrigin);
    appendTorchQuad(mesh, {top[0], top[3], top[2], top[1]}, up, topUvs,
                    textureLayer, skyLight, blockLight, sectionOrigin);
    appendTorchQuad(
        mesh, {bottom[0], top[0], top[1], bottom[1]}, -forward, sideUvs,
        textureLayer, skyLight, blockLight, sectionOrigin);
    appendTorchQuad(
        mesh, {bottom[1], top[1], top[2], bottom[2]}, right, sideUvs,
        textureLayer, skyLight, blockLight, sectionOrigin);
    appendTorchQuad(
        mesh, {bottom[2], top[2], top[3], bottom[3]}, forward, sideUvs,
        textureLayer, skyLight, blockLight, sectionOrigin);
    appendTorchQuad(
        mesh, {bottom[3], top[3], top[0], bottom[0]}, -right, sideUvs,
        textureLayer, skyLight, blockLight, sectionOrigin);
}

void appendMesh(render::MeshData& destination, const render::MeshData& source) {
    const auto vertexOffset = static_cast<std::uint32_t>(destination.vertices.size());
    destination.vertices.insert(
        destination.vertices.end(), source.vertices.begin(), source.vertices.end());
    for (const auto index : source.indices) {
        destination.indices.push_back(vertexOffset + index);
    }
}

template <typename Sampler>
bool buildSectionImpl(
    const World& world,
    ChunkPosition position,
    int sectionY,
    const Sampler& lighting,
    SmoothLightingQuality quality,
    render::RenderMeshData& result) {
    // Clear keeps the vectors' capacity so a reused RenderMeshData does not
    // reallocate for every section in the streaming burst.
    result.mesh.vertices.clear();
    result.mesh.indices.clear();
    result.cutoutMesh.vertices.clear();
    result.cutoutMesh.indices.clear();
    result.translucentMesh.vertices.clear();
    result.translucentMesh.indices.clear();
    if (sectionY < 0 || sectionY >= kSectionCount || !world.hasChunk(position)) {
        result.bounds = {};
        return false;
    }
    // Per-corner biome colour cache, scoped to this section build.
    BiomeTintCache tints{world};

    const int originX = position.x * kChunkWidth;
    const int originY = sectionY * kSectionSize;
    const int originZ = position.z * kChunkDepth;

    result.bounds = {
        {static_cast<float>(originX), static_cast<float>(originY), static_cast<float>(originZ)},
        {static_cast<float>(originX + kChunkWidth),
         static_cast<float>(originY + kSectionSize),
         static_cast<float>(originZ + kChunkDepth)},
    };
    result.mesh.vertices.reserve(4'096);
    result.mesh.indices.reserve(6'144);
    const glm::vec3 sectionOrigin{
        static_cast<float>(originX), static_cast<float>(originY), static_cast<float>(originZ)};

    const Chunk* chunk = world.chunk(position);
    if (chunk->section(sectionY).empty()) return false;
    for (int localY = 0; localY < kSectionSize; ++localY) {
        const int worldY = originY + localY;
        for (int localZ = 0; localZ < kChunkDepth; ++localZ) {
            const int worldZ = originZ + localZ;
            for (int localX = 0; localX < kChunkWidth; ++localX) {
                const int worldX = originX + localX;
                const Block current = chunk->block(localX, worldY, localZ);
                if (!isRenderable(current)) {
                    continue;
                }
                const auto& definition = blockDefinition(current);
                auto& targetMesh = definition.renderLayer == BlockRenderLayer::Translucent
                    ? result.translucentMesh
                    : (definition.renderLayer == BlockRenderLayer::Cutout
                           ? result.cutoutMesh
                           : result.mesh);
                if (definition.model == BlockModel::Cross) {
                    // Tall grass uses the dominant biome's baked plant layer;
                    // flowers keep their own texture.
                    float plantLayer = static_cast<float>(textureLayers(current).side);
                    if (current == Block::GrassPlant) {
                        const auto biome = dominantBiome(world, worldX, worldZ);
                        const auto& biomeLayers =
                            (biome == gen::Biome::Swamp && swampDarkTone(worldX, worldZ))
                                ? gen::swampDarkGrassLayers()
                                : gen::biomeGrassLayers(biome);
                        if (biomeLayers.top != 0.0F) {
                            plantLayer = biomeLayers.bottom;
                        }
                    }
                    appendCrossedPlant(
                        targetMesh, world, current, worldX, worldY, worldZ, lighting,
                        sectionOrigin, plantLayer, tints);
                    continue;
                }
                if (definition.model == BlockModel::Crop) {
                    // CropsBlock: the stage texture comes from the age stored in
                    // the orientation byte (wheat one per age, carrots/potatoes
                    // four shared stages), never a fixed layer. The mesh is the
                    // vanilla crop.json grid of four orthogonal planes.
                    const int age = world::cropAge(
                        chunk->orientation(localX, worldY, localZ));
                    appendCropPlant(
                        targetMesh, world::cropTextureLayer(current, age),
                        worldX, worldY, worldZ, lighting, sectionOrigin);
                    continue;
                }
                if (definition.model == BlockModel::Torch) {
                    // The torch's face texture (all four wall variants share the
                    // single "torch" sprite) resolves to its atlas layer at
                    // startup, like every other block — never a baked-in number.
                    appendTorchModel(
                        targetMesh, current, worldX, worldY, worldZ,
                        textureLayers(current).side, lighting, sectionOrigin);
                    continue;
                }
                if (definition.model == BlockModel::Chest) {
                    // ChestBlockEntity owns the animated base/lid render.
                    continue;
                }
                const auto orientation = chunk->orientation(localX, worldY, localZ);
                for (const auto& modelFace : kFaces) {
                    // A cube remains axis-aligned after a 90-degree blockstate
                    // rotation, so bake the oriented model face directly into
                    // the shared chunk mesh. Its UVs stay attached to the
                    // model vertices; no per-frame texture rotation is used.
                    const auto face = orientedModelFace(
                        current, orientation, modelFace);
                    // A fluid face pointing into a chunk that has not streamed
                    // in yet is culled: the neighbouring water body is assumed
                    // to continue, so the border does not render a fake
                    // waterfall from the surface to the seabed along the chunk
                    // seam. The border section is remeshed once the neighbour
                    // arrives, when real data decides the face.
                    if (isFluid(current) &&
                        !waterSampleLoaded(world, worldX + face.dx, worldZ + face.dz)) {
                        continue;
                    }
                    // Face visibility and the leaves internal-sheet check read
                    // the neighbour through the sampler (O(1) on the production
                    // path) instead of a World hash lookup per face.
                    if (shouldRenderFace(
                            current,
                            lighting.blockType(
                                worldX + face.dx, worldY + face.dy, worldZ + face.dz),
                            face)) {
                        if (isFluid(current)) {
                            appendWaterFace(
                                targetMesh, world, face, worldX, worldY, worldZ,
                                lighting, quality, sectionOrigin);
                        } else {
                            appendFace(
                                targetMesh, world, current, face, worldX, worldY, worldZ,
                                lighting, quality, sectionOrigin, tints,
                                isLeaves(current) &&
                                    lighting.blockType(
                                        worldX + face.dx,
                                        worldY + face.dy,
                                        worldZ + face.dz) == current);
                        }
                    }
                }
            }
        }
    }
    return true;
}

} // namespace

MeshLightingSnapshot::MeshLightingSnapshot(const World& world, ChunkPosition position,
                                           int minimumSectionY, int maximumSectionY,
                                           SmoothLightingQuality quality)
    : world_(world), quality_(quality) {
    const int originX = position.x * kChunkWidth;
    const int originZ = position.z * kChunkDepth;
    minimumX_ = originX - kSamplePadding;
    minimumZ_ = originZ - kSamplePadding;
    minimumY_ = std::max(0, minimumSectionY * kSectionSize - kSamplePadding);
    const int maximumX = originX + kChunkWidth + kSamplePadding - 1;
    const int maximumZ = originZ + kChunkDepth + kSamplePadding - 1;
    const int maximumY = std::min(
        kWorldHeight - 1, (maximumSectionY + 1) * kSectionSize + kSamplePadding - 1);
    width_ = maximumX - minimumX_ + 1;
    height_ = maximumY - minimumY_ + 1;
    depth_ = maximumZ - minimumZ_ + 1;

    // Fetch the request chunk and its eight neighbours once; every cell in the
    // window (±2 = within ±1 chunk) routes to one of them with integer math, so
    // the fill costs no per-cell hash lookups.
    std::array<const Chunk*, 9> chunks{};
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            chunks[(dz + 1) * 3 + (dx + 1)] = world_.chunk({position.x + dx, position.z + dz});
        }
    }

    const std::size_t cellCount =
        static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) *
        static_cast<std::size_t>(depth_);
    flags_.assign(cellCount, 0U);
    skyLevels_.assign(cellCount, 0U);
    blockLevels_.assign(cellCount, 0U);
    blockTypes_.assign(cellCount, 0U);

    for (int z = minimumZ_; z <= maximumZ; ++z) {
        const int ownerChunkZ = z < originZ ? -1 : (z >= originZ + kChunkDepth ? 1 : 0);
        const int chunkLocalZ = z - (position.z + ownerChunkZ) * kChunkDepth;
        for (int x = minimumX_; x <= maximumX; ++x) {
            const int ownerChunkX = x < originX ? -1 : (x >= originX + kChunkWidth ? 1 : 0);
            const int chunkLocalX = x - (position.x + ownerChunkX) * kChunkWidth;
            const Chunk* chunk = chunks[(ownerChunkZ + 1) * 3 + (ownerChunkX + 1)];
            for (int y = minimumY_; y <= maximumY; ++y) {
                const std::size_t cell = index(x, y, z);
                if (chunk == nullptr) {
                    // Missing neighbour: air, fully sky-lit (World::skyLight
                    // returns 15 for an unloaded chunk too).
                    blockTypes_[cell] = 0U;
                    skyLevels_[cell] = ChunkLightSampler::kMaximumLightLevel;
                    continue;
                }
                const Block value = chunk->block(chunkLocalX, y, chunkLocalZ);
                blockTypes_[cell] = static_cast<std::uint8_t>(value);
                if (mc::world::isOpaque(value)) flags_[cell] |= 0x01U;
                if (mc::world::aoOccludes(value)) flags_[cell] |= 0x02U;
                skyLevels_[cell] = chunk->skyLight(chunkLocalX, y, chunkLocalZ);
                blockLevels_[cell] = chunk->blockLight(chunkLocalX, y, chunkLocalZ);
            }
        }
    }
}

std::size_t MeshLightingSnapshot::index(int x, int y, int z) const {
    return (static_cast<std::size_t>(y - minimumY_) * static_cast<std::size_t>(depth_) +
            static_cast<std::size_t>(z - minimumZ_)) *
               static_cast<std::size_t>(width_) +
           static_cast<std::size_t>(x - minimumX_);
}

bool MeshLightingSnapshot::contains(int x, int y, int z) const {
    return x >= minimumX_ && x < minimumX_ + width_ && y >= minimumY_ &&
           y < minimumY_ + height_ && z >= minimumZ_ && z < minimumZ_ + depth_;
}

VoxelLightLevel MeshLightingSnapshot::level(int x, int y, int z) const {
    if (!contains(x, y, z)) {
        if (y < 0) return {};
        if (y >= kWorldHeight) return {ChunkLightSampler::kMaximumLightLevel, 0U};
        return {
            static_cast<std::uint8_t>(
                !mc::world::isOpaque(world_.block(x, y, z))
                    ? ChunkLightSampler::kMaximumLightLevel
                    : 0U),
            emittedLight(world_.block(x, y, z)),
        };
    }
    const std::size_t cell = index(x, y, z);
    return {skyLevels_[cell], blockLevels_[cell]};
}

float MeshLightingSnapshot::sky(int x, int y, int z) const {
    return static_cast<float>(level(x, y, z).sky) /
           static_cast<float>(ChunkLightSampler::kMaximumLightLevel);
}

float MeshLightingSnapshot::block(int x, int y, int z) const {
    return static_cast<float>(level(x, y, z).block) /
           static_cast<float>(ChunkLightSampler::kMaximumLightLevel);
}

bool MeshLightingSnapshot::isOpaque(int x, int y, int z) const {
    if (!contains(x, y, z)) {
        return y >= 0 && y < kWorldHeight &&
               mc::world::isOpaque(world_.block(x, y, z));
    }
    return (flags_[index(x, y, z)] & 0x01U) != 0U;
}

bool MeshLightingSnapshot::aoOccludes(int x, int y, int z) const {
    if (!contains(x, y, z)) return false;
    return (flags_[index(x, y, z)] & 0x02U) != 0U;
}

int MeshLightingSnapshot::opacity(int x, int y, int z) const {
    if (!contains(x, y, z)) return 0;
    return mc::world::opacity(static_cast<Block>(blockTypes_[index(x, y, z)]));
}

Block MeshLightingSnapshot::blockType(int x, int y, int z) const {
    if (!contains(x, y, z)) return Block::Air;
    return static_cast<Block>(blockTypes_[index(x, y, z)]);
}

render::MeshData ChunkMesher::build(const Chunk& chunk) {
    World world;
    world.setChunk({0, 0}, chunk);
    const ChunkLightSampler lighting{world, {0, 0}};

    render::MeshData combined;
    for (int sectionY = 0; sectionY < kSectionCount; ++sectionY) {
        const auto section = buildSection(world, {0, 0}, sectionY, lighting);
        appendMesh(combined, section.mesh);
        appendMesh(combined, section.cutoutMesh);
        appendMesh(combined, section.translucentMesh);
    }
    return combined;
}

render::RenderMeshData ChunkMesher::buildSection(
    const World& world,
    ChunkPosition position,
    int sectionY) {
    if (sectionY < 0 || sectionY >= kSectionCount || !world.hasChunk(position) ||
        world.chunk(position)->section(sectionY).empty()) {
        return {};
    }
    const ChunkLightSampler lighting{world, position};
    return buildSection(world, position, sectionY, lighting);
}

render::RenderMeshData ChunkMesher::buildSection(
    const World& world,
    ChunkPosition position,
    int sectionY,
    const ChunkLightSampler& lighting) {
    render::RenderMeshData result;
    static_cast<void>(buildSection(world, position, sectionY, lighting, result));
    return result;
}

render::RenderMeshData ChunkMesher::buildSection(
    const World& world,
    ChunkPosition position,
    int sectionY,
    SmoothLightingQuality quality) {
    if (sectionY < 0 || sectionY >= kSectionCount || !world.hasChunk(position) ||
        world.chunk(position)->section(sectionY).empty()) {
        return {};
    }
    const ChunkLightSampler lighting{world, position};
    render::RenderMeshData result;
    buildSectionImpl(world, position, sectionY, lighting, quality, result);
    return result;
}

bool ChunkMesher::buildSection(
    const World& world,
    ChunkPosition position,
    int sectionY,
    const ChunkLightSampler& lighting,
    render::RenderMeshData& result) {
    return buildSectionImpl(
        world, position, sectionY, lighting, SmoothLightingQuality::Standard, result);
}

bool ChunkMesher::buildSection(
    const World& world,
    ChunkPosition position,
    int sectionY,
    const MeshLightingSnapshot& lighting,
    render::RenderMeshData& result) {
    return buildSectionImpl(world, position, sectionY, lighting, lighting.quality(), result);
}

} // namespace mc::world

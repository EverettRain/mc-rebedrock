#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc::render {

// 20-byte packed terrain vertex. Positions are stored relative to the section
// origin in [-0.5, 16.5] (the half-block margin covers plant jitter and the
// water surface); u16 gives 17/65535 ≈ 0.00026 block resolution, so shared
// boundary coordinates between adjacent sections reconstruct within one LSB —
// no cracks. Normals are an index into kVertexNormals, UVs are u16 over [0,1),
// textureLayer is an exact u16 integer (animation checks and the
// sampler2DArray layer index must be lossless), and the four light channels
// are u8 (n/15 light levels map exactly: n*17/255 == n/15).
struct VoxelVertex final {
    std::uint16_t positionX;
    std::uint16_t positionY;
    std::uint16_t positionZ;
    std::uint8_t normalIndex;
    std::uint8_t pad;
    std::uint16_t uvX;
    std::uint16_t uvY;
    std::uint16_t textureLayer;
    // Opaque faces: AO in [0,1]. Water faces instead carry the optical column
    // depth (1..32, integer) in waterDepth, and the shader picks one per layer.
    std::uint8_t ambientOcclusion;
    std::uint8_t waterDepth;
    std::uint8_t skyLight;
    std::uint8_t blockLight;
    std::uint8_t flatSkyLight;
    std::uint8_t flatBlockLight;
    // Per-vertex biome colour tint (vanilla BiomeColors): grass tops/plants and
    // foliage multiply their texture by this, so a biome boundary reads as a
    // smooth colour gradient instead of a hard switch. White (255,255,255)
    // means no tint.
    std::uint8_t tintR;
    std::uint8_t tintG;
    std::uint8_t tintB;
    std::uint8_t tintPad;
};

static_assert(sizeof(VoxelVertex) == 24);

inline constexpr float kLocalWindowBase = -0.5F;
inline constexpr float kLocalWindowSize = 17.0F;
inline constexpr float kLocalScale = kLocalWindowSize / 65535.0F;
// UV window: flowing water scrolls its corners across [-0.375, 1.375], so the
// fixed window covers that instead of wrapping (which would tear the quad).
inline constexpr float kUvWindowBase = -0.5F;
inline constexpr float kUvWindowSize = 2.0F;
inline constexpr float kUvScale = kUvWindowSize / 65535.0F;

// Axis-aligned face normals plus the wall-torch diagonals (facing N/E/S/W ->
// up, forward), in the same order the vertex shader's table lists them.
inline constexpr std::array<glm::vec3, 14> kVertexNormals{{
    {1.0F, 0.0F, 0.0F},   { -1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F},
    {0.0F, -1.0F, 0.0F},  {0.0F, 0.0F, 1.0F},  {0.0F, 0.0F, -1.0F},
    // N torch
    {0.0F, 0.900552F, -0.434749F}, {0.0F, -0.434749F, -0.900552F},
    // E torch
    {0.434749F, 0.900552F, 0.0F}, {0.900552F, -0.434749F, 0.0F},
    // S torch
    {0.0F, 0.900552F, 0.434749F}, {0.0F, -0.434749F, 0.900552F},
    // W torch
    {-0.434749F, 0.900552F, 0.0F}, {-0.900552F, -0.434749F, 0.0F},
}};

// The normal-table index closest to `normal` (max dot product). The mesher
// emits axis-aligned face normals and the torch diagonals, both present above.
[[nodiscard]] inline std::uint8_t nearestNormalIndex(const glm::vec3& normal) {
    std::uint8_t best = 0;
    float bestDot = -2.0F;
    for (std::size_t index = 0; index < kVertexNormals.size(); ++index) {
        const glm::vec3& candidate = kVertexNormals[index];
        const float dot = normal.x * candidate.x + normal.y * candidate.y +
                          normal.z * candidate.z;
        if (dot > bestDot) {
            bestDot = dot;
            best = static_cast<std::uint8_t>(index);
        }
    }
    return best;
}

[[nodiscard]] inline VoxelVertex packVertex(
    const glm::vec3& localPosition,
    const glm::vec3& normal,
    const glm::vec2& uv,
    float textureLayer,
    float ambientOcclusion,
    float waterDepth,
    float sky,
    float block,
    float flatSky,
    float flatBlock,
    std::uint8_t tintR = 255U,
    std::uint8_t tintG = 255U,
    std::uint8_t tintB = 255U,
    std::uint8_t biomeMask = 0U) {
    const auto quantizePosition = [](float value) {
        return static_cast<std::uint16_t>(std::clamp(
            static_cast<long>(std::lround((value - kLocalWindowBase) / kLocalScale)), 0L,
            65535L));
    };
    const auto quantizeUnit = [](float value) {
        return static_cast<std::uint8_t>(std::clamp(
            static_cast<long>(std::lround(value * 255.0F)), 0L, 255L));
    };
    const auto quantizeUv = [](float value) {
        return static_cast<std::uint16_t>(std::clamp(
            static_cast<long>(std::lround((value - kUvWindowBase) / kUvScale)), 0L, 65535L));
    };
    return VoxelVertex{
        quantizePosition(localPosition.x),
        quantizePosition(localPosition.y),
        quantizePosition(localPosition.z),
        nearestNormalIndex(normal),
        biomeMask,
        quantizeUv(uv.x),
        quantizeUv(uv.y),
        static_cast<std::uint16_t>(std::lround(textureLayer)),
        quantizeUnit(ambientOcclusion),
        static_cast<std::uint8_t>(std::clamp(
            static_cast<long>(std::lround(waterDepth)), 0L, 255L)),
        quantizeUnit(sky),
        quantizeUnit(block),
        quantizeUnit(flatSky),
        quantizeUnit(flatBlock),
        tintR,
        tintG,
        tintB,
        255U,
    };
}

[[nodiscard]] inline glm::vec3 decodeLocalPosition(const VoxelVertex& vertex) {
    return {
        static_cast<float>(vertex.positionX) * kLocalScale + kLocalWindowBase,
        static_cast<float>(vertex.positionY) * kLocalScale + kLocalWindowBase,
        static_cast<float>(vertex.positionZ) * kLocalScale + kLocalWindowBase,
    };
}

// Decode helpers for tests and any CPU-side consumer; the vertex shader mirrors
// these exactly.
[[nodiscard]] inline glm::vec3 decodeWorldPosition(const VoxelVertex& vertex,
                                                   const glm::vec3& sectionOrigin) {
    return sectionOrigin + decodeLocalPosition(vertex);
}
[[nodiscard]] inline glm::vec3 decodeNormal(const VoxelVertex& vertex) {
    return kVertexNormals[vertex.normalIndex];
}
[[nodiscard]] inline glm::vec2 decodeUv(const VoxelVertex& vertex) {
    return {static_cast<float>(vertex.uvX) * kUvScale + kUvWindowBase,
            static_cast<float>(vertex.uvY) * kUvScale + kUvWindowBase};
}
[[nodiscard]] inline float decodeTextureLayer(const VoxelVertex& vertex) {
    return static_cast<float>(vertex.textureLayer);
}
[[nodiscard]] inline float decodeAmbientOcclusion(const VoxelVertex& vertex) {
    return static_cast<float>(vertex.ambientOcclusion) / 255.0F;
}
[[nodiscard]] inline float decodeSkyLight(const VoxelVertex& vertex) {
    return static_cast<float>(vertex.skyLight) / 255.0F;
}
[[nodiscard]] inline float decodeBlockLight(const VoxelVertex& vertex) {
    return static_cast<float>(vertex.blockLight) / 255.0F;
}
[[nodiscard]] inline float decodeFlatSkyLight(const VoxelVertex& vertex) {
    return static_cast<float>(vertex.flatSkyLight) / 255.0F;
}
[[nodiscard]] inline float decodeFlatBlockLight(const VoxelVertex& vertex) {
    return static_cast<float>(vertex.flatBlockLight) / 255.0F;
}
[[nodiscard]] inline float decodeWaterDepth(const VoxelVertex& vertex) {
    return static_cast<float>(vertex.waterDepth);
}

struct MeshData final {
    std::vector<VoxelVertex> vertices;
    std::vector<std::uint32_t> indices;

    [[nodiscard]] bool empty() const { return indices.empty(); }
    // Reserved heap bytes (capacity, not size) — the CPU mesh pool keeps these
    // buffers around at their peak capacity for reuse, so N-Mem measures the
    // capacity, which is the real resident cost.
    [[nodiscard]] std::size_t capacityBytes() const {
        return vertices.capacity() * sizeof(VoxelVertex) +
               indices.capacity() * sizeof(std::uint32_t);
    }
};

struct Aabb final {
    glm::vec3 minimum{};
    glm::vec3 maximum{};
};

struct RenderMeshData final {
    MeshData mesh;
    MeshData cutoutMesh;
    MeshData translucentMesh;
    Aabb bounds;

    [[nodiscard]] bool empty() const {
        return mesh.empty() && cutoutMesh.empty() && translucentMesh.empty();
    }
    [[nodiscard]] std::size_t capacityBytes() const {
        return mesh.capacityBytes() + cutoutMesh.capacityBytes() +
               translucentMesh.capacityBytes();
    }
};

} // namespace mc::render

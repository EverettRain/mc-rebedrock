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

// 20 字节的紧凑地形顶点
// 位置相对 section 原点存储，范围 [-0.5, 16.5]，那半格余量用来容纳植物抖动与水面
// u16 给出 17/65535 约 0.00026 格的分辨率
// 相邻 section 共享的边界坐标因此在一个最低位内还原，不会开裂
// 法线是 kVertexNormals 的下标，UV 是覆盖 [0,1) 的 u16
// textureLayer 是精确的 u16 整数，因为动画判定与 sampler2DArray 的层号都不允许有损
// 四个光照通道是 u8，n/15 的光照级别恰好映射，因为 n*17/255 等于 n/15
struct VoxelVertex final {
    std::uint16_t positionX;
    std::uint16_t positionY;
    std::uint16_t positionZ;
    std::uint8_t normalIndex;
    std::uint8_t pad;
    std::uint16_t uvX;
    std::uint16_t uvY;
    std::uint16_t textureLayer;
    // 不透明面在这里放 [0,1] 的环境光遮蔽
    // 水面改为在 waterDepth 里放光学柱深度，取 1 到 32 的整数，着色器按层各取其一
    std::uint8_t ambientOcclusion;
    std::uint8_t waterDepth;
    std::uint8_t skyLight;
    std::uint8_t blockLight;
    std::uint8_t flatSkyLight;
    std::uint8_t flatBlockLight;
    // 逐顶点的群系配色，对应 vanilla 的 BiomeColors
    // 草方块顶面、植物与树叶把自己的纹理乘上它，群系边界因此呈现为平滑的颜色渐变而不是硬切换
    // 白色 (255,255,255) 表示不着色
    std::uint8_t tintR;
    std::uint8_t tintG;
    std::uint8_t tintB;
    std::uint8_t tintPad;
};

static_assert(sizeof(VoxelVertex) == 24);

inline constexpr float kLocalWindowBase = -0.5F;
inline constexpr float kLocalWindowSize = 17.0F;
inline constexpr float kLocalScale = kLocalWindowSize / 65535.0F;
// UV 窗口：流动的水会把角点滚过 [-0.375, 1.375]
// 固定窗口因此覆盖这个范围，而不是回绕，回绕会把四边形撕开
inline constexpr float kUvWindowBase = -0.5F;
inline constexpr float kUvWindowSize = 2.0F;
inline constexpr float kUvScale = kUvWindowSize / 65535.0F;

// 轴对齐的面法线，外加墙上火把的斜向法线，朝向北东南西时分别指向上方与前方
// 顺序与顶点着色器里那张表一致
inline constexpr std::array<glm::vec3, 14> kVertexNormals{{
    {1.0F, 0.0F, 0.0F},   { -1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F},
    {0.0F, -1.0F, 0.0F},  {0.0F, 0.0F, 1.0F},  {0.0F, 0.0F, -1.0F},
    // 朝北的火把
    {0.0F, 0.900552F, -0.434749F}, {0.0F, -0.434749F, -0.900552F},
    // 朝东的火把
    {0.434749F, 0.900552F, 0.0F}, {0.900552F, -0.434749F, 0.0F},
    // 朝南的火把
    {0.0F, 0.900552F, 0.434749F}, {0.0F, -0.434749F, 0.900552F},
    // 朝西的火把
    {-0.434749F, 0.900552F, 0.0F}, {-0.900552F, -0.434749F, 0.0F},
}};

// 法线表里与 normal 最接近的那个下标，按点积最大取
// 网格化器产出的就是轴对齐的面法线与火把斜向法线，上面那张表两者都有
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

// 供测试与任何 CPU 侧消费者使用的解码助手，顶点着色器里是与之完全一致的一份
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
    // 预留的堆字节数，取 capacity 而不是 size
    // CPU 网格池把这些缓冲按峰值容量留着复用，所以内存统计量的是 capacity，那才是真实的常驻开销
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

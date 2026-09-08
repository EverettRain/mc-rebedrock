// 离屏预览出的网格，必须和玩家真正看到的那一份逐字节相同。
//
// 现场：台阶铺的屋顶，顶面在预览图里渲染成接近纯黑，而那一格的天光实测是满值 15。
//
// 根因不在阴影、也不在 AO。三参数的 `ChunkMesher::buildSection` 自己
// `ChunkLightSampler{world, position}` —— 那是个**重算**光照的取样器，而它判断
// 「这一格不透光」用的仍是 `isOpaque`，也就是**渲染分桶**（RN-8f 已经把值那一轴从分桶
// 拆开了，但这份重算没跟上）。台阶渲染在 Opaque 桶里，于是它那一格的天光被直接写 0。
//
// 三个预览装配点原本都是「先 `WorldLightEngine::initializeChunks` 算好光，隔两行再调
// 三参数重载**把它扔掉**」。真机走 `ChunkStreamer` → `MeshLightingSnapshot`，读的是引擎
// 存下来的值，所以这是**离屏预览专属**的偏差——出的图与玩家看到的不是同一回事，
// 而本仓所有「视觉验收」都建立在那些图上。
//
// 这条测试钉的就是那句话：**预览路径 == 生产路径**。

#include "render/MeshData.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkMesher.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"
#include "world/WorldLightEngine.hpp"
#include "world/WorldLighting.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

#ifndef MC_REBEDROCK_RENDERER_SRC
#error "MC_REBEDROCK_RENDERER_SRC must point at src/render/vulkan/VulkanRenderer.cpp"
#endif

namespace {

using mc::world::Block;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::ChunkPosition;
using mc::world::SlabPortion;
using mc::world::World;

int failures = 0;

void require(bool condition, std::string_view what) {
    if (!condition) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

constexpr int kOriginX = 6;
constexpr int kOriginZ = 6;
constexpr int kFloorY = 64;
constexpr int kRoofY = kFloorY + 3;

// 现场那个场景：5x5 石地板、三面墙、一层下半砖当天花板。
[[nodiscard]] World roofedRoom() {
    World world;
    Chunk chunk;
    for (int z = 0; z < 5; ++z) {
        for (int x = 0; x < 5; ++x) {
            chunk.setBlock(kOriginX + x, kFloorY, kOriginZ + z, Block::Stone);
        }
    }
    for (int y = kFloorY + 1; y <= kFloorY + 2; ++y) {
        for (int x = 0; x < 5; ++x) {
            chunk.setBlock(kOriginX + x, y, kOriginZ, Block::Stone);
        }
        for (int z = 1; z <= 3; ++z) {
            chunk.setBlock(kOriginX, y, kOriginZ + z, Block::Stone);
            chunk.setBlock(kOriginX + 4, y, kOriginZ + z, Block::Stone);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
    const BlockState roof =
        BlockState{Block::OakSlab, mc::world::defaultOrientation(Block::OakSlab), 0U}
            .withSlabPortion(SlabPortion::Bottom);
    for (int z = 0; z < 5; ++z) {
        for (int x = 0; x < 5; ++x) {
            world.setState(kOriginX + x, kRoofY, kOriginZ + z, roof);
        }
    }
    mc::world::WorldLightEngine lighting;
    const std::array positions{ChunkPosition{0, 0}};
    lighting.initializeChunks(world, std::span<const ChunkPosition>{positions});
    return world;
}

// 顶点的完整光照身份：天光、方块光、AO。三者任一不同就是两张不同的画面。
[[nodiscard]] bool sameLighting(const mc::render::MeshData& left,
                                const mc::render::MeshData& right, std::string_view what) {
    if (left.vertices.size() != right.vertices.size()) {
        std::cerr << "FAILED: " << what << " 顶点数不同：" << left.vertices.size() << " vs "
                  << right.vertices.size() << "\n";
        ++failures;
        return false;
    }
    for (std::size_t index = 0; index < left.vertices.size(); ++index) {
        const auto& a = left.vertices[index];
        const auto& b = right.vertices[index];
        if (a.skyLight != b.skyLight || a.blockLight != b.blockLight ||
            mc::render::decodeAmbientOcclusion(a) != mc::render::decodeAmbientOcclusion(b)) {
            std::cerr << "FAILED: " << what << " 顶点 " << index << " 不同：sky "
                      << int{a.skyLight} << " vs " << int{b.skyLight} << ", block "
                      << int{a.blockLight} << " vs " << int{b.blockLight} << "\n";
            ++failures;
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream{path};
    if (!stream) {
        std::cerr << "FAILED: cannot read " << path.string() << "\n";
        ++failures;
        return {};
    }
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

} // namespace

int main() {
    const World world = roofedRoom();
    const int section = mc::world::sectionIndexFromWorldY(kRoofY);

    // 生产路径：ChunkStreamer 用的那个快照取样器。
    const mc::world::MeshLightingSnapshot production{world, {0, 0}, section, section};
    mc::render::RenderMeshData productionMesh;
    const bool built =
        mc::world::ChunkMesher::buildSection(world, {0, 0}, section, production, productionMesh);
    require(built, "生产路径必须真的建出一个 section，否则下面全是空比空");

    // 预览路径：单参数构造的取样器，`stored_` 为真，读世界里存着的光。
    const mc::world::ChunkLightSampler stored{world};
    const auto previewMesh = mc::world::ChunkMesher::buildSection(world, {0, 0}, section, stored);

    require(!productionMesh.mesh.vertices.empty(), "这个 section 必须有不透明几何");
    sameLighting(productionMesh.mesh, previewMesh.mesh, "预览路径与生产路径");

    // ★ 非空断言：那个**重算**的取样器确实给出不同的答案。
    //
    // 少了这一条，上面那句「两条路径相同」在预览用错取样器时**也会成立**——
    // 只要两者恰好一致。它们不一致，而这正是缺陷之所以存在。
    const mc::world::ChunkLightSampler recomputing{world, ChunkPosition{0, 0}};
    const auto recomputedMesh =
        mc::world::ChunkMesher::buildSection(world, {0, 0}, section, recomputing);
    bool differs = recomputedMesh.mesh.vertices.size() != productionMesh.mesh.vertices.size();
    for (std::size_t index = 0;
         !differs && index < recomputedMesh.mesh.vertices.size(); ++index) {
        differs = recomputedMesh.mesh.vertices[index].skyLight !=
                  productionMesh.mesh.vertices[index].skyLight;
    }
    require(differs,
            "重算取样器必须与生产路径不同——它们若一致，上面那条相等断言就什么也没证明");

    // 屋顶顶面的具体数字，免得「相同」只是「同样地错」。
    {
        // `decodeLocalPosition` 给的是 **section 局部**坐标，不是世界 y 减 kMinY。
        // 下半砖的顶面在格内 y=0.5。
        const float roofTopY =
            static_cast<float>(kRoofY - mc::world::sectionOriginY(section)) + 0.5F;
        int lit = 0;
        int dark = 0;
        for (const auto& vertex : productionMesh.mesh.vertices) {
            const auto normal = mc::render::kVertexNormals[vertex.normalIndex];
            const auto position = mc::render::decodeLocalPosition(vertex);
            if (normal.y < 0.5F || std::abs(position.y - roofTopY) > 0.01F) {
                continue;
            }
            (vertex.skyLight == 255U ? lit : dark)++;
        }
        require(lit > 0, "屋顶顶面必须有顶点，否则下面那条断言是空的");
        require(dark == 0, "台阶屋顶的顶面在满天光下必须全亮——它曾经是 [0..191]");
    }

    // 源码护栏：预览侧不得再调那个自己重算光照的三参数重载。
    //
    // 运行期抓不到它：三个装配点各自都「看起来对」（先算光、再建网格），
    // 错的是它们之间少了一次传递。
    {
        const std::string source = readFile(MC_REBEDROCK_RENDERER_SRC);
        const auto found = source.find("buildSection(clientCache, {0, 0}, sectionY);");
        if (found != std::string::npos) {
            std::cerr << "FAILED: VulkanRenderer 里还有三参数的 buildSection 调用——"
                         "那个重载会自己重算一份光照，把 initializeChunks 刚算好的扔掉\n";
            ++failures;
        }
        require(source.find("previewLighting") != std::string::npos,
                "预览装配点必须把引擎算好的光作为取样器传下去");
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "preview_mesh_lighting ok\n";
    return 0;
}

// 树叶在相邻方块上留下平滑光照的阴影，和石头一样；但它**不**触发对角替换。
//
// 现场：「树叶本来也不会在树干或相邻实方块上生成平滑光照的阴影，导致当前版本的树
// 看起来缺少层次感」。
//
// 根因是一条谓词兼任了两件事。26.1 分开问：
//
//   ① 压不压暗一个角 —— `BlockBehaviour.getShadeBrightness`（BlockBehaviour.java:319）
//        state.isCollisionShapeFullBlock(level, pos) ? 0.2F : 1.0F
//      `LeavesBlock` 没有覆写它，而树叶的碰撞盒是满格（人能站上去）⇒ **0.2**。
//      覆写回 1.0F 的只有 `TransparentBlock`（玻璃与十六色染色玻璃）。
//
//   ② 算不算挡住视线，也就是要不要触发对角替换 ——
//      `BlockModelLighter.java:61` 的 `translucentN`
//        !corner.isViewBlocking(level, pos) || corner.getLightDampening() == 0
//      树叶被 `Blocks.java:6778` 显式设成 `isViewBlocking(never)` ⇒ **不挡**。
//
// 本作从前只有一个 `aoOccludes`，问的是**渲染分桶**（`isOpaque`）并手工排掉树叶，
// 于是树叶两件事都不做：树冠对树干、对地面都不投任何角阴影。
//
// 树叶正是把两个答案分开的那个方块，所以下面用同一个夹具、只换中间那块方块，
// 让三种材料给出三个不同的数：石头 0.40、树叶 0.60、空气 1.00。

#include "render/MeshData.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkMesher.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"
#include "world/WorldLightEngine.hpp"
#include "world/WorldLighting.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <string_view>

namespace {

using mc::world::Block;
using mc::world::Chunk;
using mc::world::ChunkPosition;
using mc::world::World;

int failures = 0;

void require(bool condition, std::string_view what) {
    if (!condition) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

void requireNear(float actual, float expected, std::string_view what) {
    if (std::fabs(actual - expected) > 3.0F / 255.0F) {
        std::cerr << "FAILED: " << what << ": expected " << expected << ", actual " << actual
                  << "\n";
        ++failures;
    }
}

constexpr int kFloorY = 64;
constexpr int kCentreX = 8;
constexpr int kCentreZ = 8;

// 一块石地板。地板上放 `edgeA` 和 `edgeB` 两根**两格高**的柱子，它们各占中心格顶面
// 那个角的一条边；对角那一格永远留空。
//
// 柱子必须是两格高：26.1 的 `translucentN` 读的**不是**边邻居本身，而是它再往面法线
// 外走一格（`BlockModelLighter.java:60-67`）。只摆一格高，那个探针落在空气上，
// 对角替换对石头也不会触发——本测试第一版正是这样写的，②于是量到石头 0.60。
[[nodiscard]] World floorWithNeighbours(Block edgeA, Block edgeB) {
    World world;
    Chunk chunk;
    for (int z = kCentreZ - 3; z <= kCentreZ + 3; ++z) {
        for (int x = kCentreX - 3; x <= kCentreX + 3; ++x) {
            chunk.setBlock(x, kFloorY, z, Block::Stone);
        }
    }
    for (int height = 1; height <= 2; ++height) {
        if (edgeA != Block::Air) {
            chunk.setBlock(kCentreX + 1, kFloorY + height, kCentreZ, edgeA);
        }
        if (edgeB != Block::Air) {
            chunk.setBlock(kCentreX, kFloorY + height, kCentreZ + 1, edgeB);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
    mc::world::WorldLightEngine lighting;
    const std::array positions{ChunkPosition{0, 0}};
    lighting.initializeChunks(world, std::span<const ChunkPosition>{positions});
    return world;
}

// 中心格顶面在 (x+1, z+1) 那个角的 AO。它的环是：中心 = 头顶那一格（空气），
// 两条边 = 上面摆的两块，对角 = (x+1, y+1, z+1)（空）。
[[nodiscard]] float cornerFrom(const mc::render::MeshData& mesh, int section) {
    const float topY = static_cast<float>(kFloorY - mc::world::sectionOriginY(section)) + 1.0F;
    float found = -1.0F;
    for (const auto& vertex : mesh.vertices) {
        const auto normal = mc::render::kVertexNormals[vertex.normalIndex];
        const auto position = mc::render::decodeLocalPosition(vertex);
        if (normal.y < 0.5F || std::fabs(position.y - topY) > 0.01F) {
            continue;
        }
        if (std::fabs(position.x - static_cast<float>(kCentreX + 1)) > 0.01F ||
            std::fabs(position.z - static_cast<float>(kCentreZ + 1)) > 0.01F) {
            continue;
        }
        found = mc::render::decodeAmbientOcclusion(vertex);
        break;
    }
    require(found >= 0.0F, "地板中心格的顶面必须有那个角的顶点，否则这条测试是空的");
    return found;
}

// 两个取样器各建一次，答案必须一致。
//
// ★ 这一段是补出来的。上面那些断言原本只走 `ChunkLightSampler`（stored 视图），
// 它逐格现问 `mc::world::aoDarkens` / `aoBlocksView`；而真机走的是
// `MeshLightingSnapshot`，两条谓词在那里被**打包进 flags_ 的两个位**再读回来。
// 于是「把这两个位对调」这种注入，前者一点感觉都没有——sabotage ③ 当时全绿。
// 生产路径必须自己出现在夹具里，光有一个「等价」的旁路取样器不算。
[[nodiscard]] float cornerAmbientOcclusion(const World& world) {
    const int section = mc::world::sectionIndexFromWorldY(kFloorY);

    const mc::world::MeshLightingSnapshot production{world, {0, 0}, section, section};
    mc::render::RenderMeshData productionMesh;
    const bool built =
        mc::world::ChunkMesher::buildSection(world, {0, 0}, section, production, productionMesh);
    require(built, "生产路径必须真的建出一个 section");
    const float fromProduction = cornerFrom(productionMesh.mesh, section);

    const mc::world::ChunkLightSampler stored{world};
    const auto storedMesh = mc::world::ChunkMesher::buildSection(world, {0, 0}, section, stored);
    const float fromStored = cornerFrom(storedMesh.mesh, section);

    if (std::fabs(fromProduction - fromStored) > 1.0F / 255.0F) {
        std::cerr << "FAILED: 生产取样器与 stored 取样器给出不同的角 AO：" << fromProduction
                  << " vs " << fromStored
                  << "\n  两条谓词在 MeshLightingSnapshot 里打包进 flags_ 的两个位，"
                     "对调它们只有生产路径看得见\n";
        ++failures;
    }
    return fromProduction;
}

[[nodiscard]] Block anyLeaves() { return Block::OakLeaves; }

// ---- ① 一条边：树叶压暗一个角，和石头一模一样 --------------------------
//
// 只有一条边上有方块，对角替换的前提（两条边都挡视线）不成立，所以这一档量到的
// 纯粹是「压不压暗」。(1 + 0.2 + 1 + 1) / 4 = 0.8。
void testLeavesDarkenLikeStone() {
    const float air = cornerAmbientOcclusion(floorWithNeighbours(Block::Air, Block::Air));
    const float stone = cornerAmbientOcclusion(floorWithNeighbours(Block::Stone, Block::Air));
    const float leaves = cornerAmbientOcclusion(floorWithNeighbours(anyLeaves(), Block::Air));

    requireNear(air, 1.0F, "空地上的角没有遮挡");
    requireNear(stone, 0.8F, "石头把这个角压到 (1+0.2+1+1)/4");
    requireNear(leaves, 0.8F,
                "树叶把这个角压到同一个数——26.1 的 getShadeBrightness 只问碰撞盒满不满格");
    require(leaves < air, "树叶必须真的压暗了什么，否则上面那条只是巧合");
}

// ---- ② 两条边：树叶**不**触发对角替换，石头触发 -------------------------
//
// 两条边都挡视线时，26.1 不去读对角那一格，改用 corners[0] 的样本（一块石头，0.2）：
//   石头 (1 + 0.2 + 0.2 + 0.2) / 4 = 0.40
// 树叶不挡视线，规则不触发，对角照常读到空气：
//   树叶 (1 + 0.2 + 0.2 + 1.0) / 4 = 0.60
//
// 这一档是两条谓词唯一分道扬镳的地方。把 aoBlocksView 写成 aoDarkens（或反过来），
// 上面①仍然全绿，这里立刻炸。
void testLeavesDoNotSubstituteTheDiagonal() {
    const float stone = cornerAmbientOcclusion(floorWithNeighbours(Block::Stone, Block::Stone));
    const float leaves = cornerAmbientOcclusion(floorWithNeighbours(anyLeaves(), anyLeaves()));

    requireNear(stone, 0.4F, "两块石头夹出的内角触发对角替换：(1+0.2+0.2+0.2)/4");
    requireNear(leaves, 0.6F,
                "树叶不挡视线，对角照常读到空气：(1+0.2+0.2+1)/4——它压暗但不替换");
    require(stone < leaves, "两条谓词必须给出不同的答案，否则拆开这件事没有被测到");
}

// ---- ③ 玻璃两件事都不做 -------------------------------------------------
//
// `TransparentBlock.getShadeBrightness` 覆写成 1.0F（TransparentBlock.java:36），
// `Blocks.java:563` 又给它 `isViewBlocking(never)`。它和树叶同为「满格但看得穿」，
// 答案却相反——所以「满格就压暗」这种简化会在这里现形。
void testGlassDoesNeither() {
    requireNear(cornerAmbientOcclusion(floorWithNeighbours(Block::Glass, Block::Air)), 1.0F,
                "玻璃不压暗角");
    requireNear(cornerAmbientOcclusion(floorWithNeighbours(Block::Glass, Block::Glass)), 1.0F,
                "玻璃也不触发对角替换");
}

// ---- ④ 整份名册：两条谓词各自成立，且确实是两条 -------------------------
//
// 上面三条是点，这条是面。它同时钉住「挡视线 ⇒ 压暗」这条包含关系，以及那个差集
// **非空**——差集空了，两条谓词就退化成一条，②那种夹具将来也可能因为别的原因而绿。
void testRosterKeepsThemTwoPredicates() {
    int darkenOnly = 0;
    int leavesInDifference = 0;
    for (int index = 0; index < static_cast<int>(Block::Count); ++index) {
        const Block block = static_cast<Block>(index);
        if (!mc::world::isRenderable(block)) {
            continue;
        }
        const bool darkens = mc::world::aoDarkens(block);
        const bool blocksView = mc::world::aoBlocksView(block);
        if (blocksView && !darkens) {
            std::cerr << "FAILED: " << mc::world::blockDefinition(block).identifier.toString()
                      << " 挡视线却不压暗——26.1 里挡视线的前提就是碰撞盒满格\n";
            ++failures;
        }
        if (darkens && !blocksView) {
            ++darkenOnly;
            if (mc::world::isLeaves(block)) {
                ++leavesInDifference;
            }
        }
    }
    require(darkenOnly > 0, "必须存在「压暗但不挡视线」的方块，否则两条谓词是同一条");
    require(leavesInDifference > 0, "树叶必须落在那个差集里——它就是本轮修的那一类");
    std::cout << "  压暗但不挡视线的方块：" << darkenOnly << " 个（其中树叶 "
              << leavesInDifference << " 种）\n";
}

} // namespace

int main() {
    testLeavesDarkenLikeStone();
    testLeavesDoNotSubstituteTheDiagonal();
    testGlassDoesNeither();
    testRosterKeepsThemTwoPredicates();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "leaf_ambient_occlusion ok\n";
    return 0;
}

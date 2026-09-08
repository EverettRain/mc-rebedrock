// 一个面的光照环锚在哪一格：贴着格壁的面锚在**外面**，缩在格内的面锚在**自己**这一格。
//
// 现场：两个台阶堆成一个方块（双层台阶）之后，明显比同材质的整块方块暗。
//
// 根因在 `boxFaceAxisExtent`：它返回的是**盒子在该轴上的整段范围**，而不是那一片 quad
// 所在的平面。于是 `faceAnchorsOutside` 的第一句 `minAxis != maxAxis` 对任何有厚度的盒子
// 都直接返回「锚在格内」——后面那段 `flush || collisionFillsCell` **一次也没执行过**。
//
// 26.1 取的是 quad 四个顶点的 min/max（`BlockModelLighter.prepareQuadShape`:221-234），
// 一个轴对齐的面上那四个点在自己的轴上是同一个值，所以 `:263` 判据里的 `minY == maxY`
// 恒真，真正做决定的是 `maxY > 0.9999F || isCollisionShapeFullBlock`。
//
// 双层台阶六个面全贴壁 ⇒ 全都该锚在外，答案就该与整块方块逐字节相同。它从前锚在格内，
// 而格内的中心格是它自己——不透光、天光 0——于是顶面 (0+15+15+15)/4 = 191、
// 侧面低到 64。

#include "render/MeshData.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkMesher.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"
#include "world/WorldLightEngine.hpp"
#include "world/WorldLighting.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

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

constexpr int kFloorY = 64;
constexpr int kBlockX = 8;
constexpr int kBlockZ = 8;

// 一块石地板，正中央放 `state`，然后点亮。两次调用只差那一个方块。
[[nodiscard]] World withBlock(BlockState state) {
    World world;
    Chunk chunk;
    for (int z = 4; z <= 12; ++z) {
        for (int x = 4; x <= 12; ++x) {
            chunk.setBlock(x, kFloorY, z, Block::Stone);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
    world.setState(kBlockX, kFloorY + 1, kBlockZ, state);
    mc::world::WorldLightEngine lighting;
    const std::array positions{ChunkPosition{0, 0}};
    lighting.initializeChunks(world, std::span<const ChunkPosition>{positions});
    return world;
}

struct FaceSample final {
    int normalIndex = 0;
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    int sky = 0;
    int block = 0;
};

// 正中那一格的全部顶点，按位置与法线排序，好逐一对齐比较。
[[nodiscard]] std::vector<FaceSample> centreVertices(const World& world) {
    const int section = mc::world::sectionIndexFromWorldY(kFloorY + 1);
    const mc::world::ChunkLightSampler stored{world};
    const auto mesh = mc::world::ChunkMesher::buildSection(world, {0, 0}, section, stored).mesh;
    std::vector<FaceSample> found;
    for (const auto& vertex : mesh.vertices) {
        const auto position = mc::render::decodeLocalPosition(vertex);
        if (position.x < static_cast<float>(kBlockX) - 0.01F ||
            position.x > static_cast<float>(kBlockX) + 1.01F ||
            position.z < static_cast<float>(kBlockZ) - 0.01F ||
            position.z > static_cast<float>(kBlockZ) + 1.01F) {
            continue;
        }
        found.push_back({vertex.normalIndex, position.x, position.y, position.z,
                         vertex.skyLight, vertex.blockLight});
    }
    std::sort(found.begin(), found.end(), [](const FaceSample& a, const FaceSample& b) {
        return std::tie(a.normalIndex, a.x, a.y, a.z) < std::tie(b.normalIndex, b.x, b.y, b.z);
    });
    return found;
}

[[nodiscard]] BlockState doubleSlab() {
    return BlockState{Block::StoneSlab, mc::world::defaultOrientation(Block::StoneSlab), 0U}
        .withSlabPortion(SlabPortion::Double);
}

} // namespace

int main() {
    // ---- 用户报的那一条：双层台阶必须和整块方块一样亮 --------------------
    const auto stone = centreVertices(withBlock(BlockState{Block::Stone}));
    const auto slab = centreVertices(withBlock(doubleSlab()));

    require(!stone.empty(), "整块石头必须产出顶点，否则这条测试是空的");
    require(stone.size() == slab.size(),
            "双层台阶与整块方块的几何应当一致——它就是一个满方块");

    if (stone.size() == slab.size()) {
        for (std::size_t index = 0; index < stone.size(); ++index) {
            const auto& a = stone[index];
            const auto& b = slab[index];
            if (a.sky != b.sky || a.block != b.block) {
                std::cerr << "FAILED: 双层台阶的顶点 " << index << " 与整块方块不同："
                          << "位置 (" << b.x << "," << b.y << "," << b.z << ") sky " << a.sky
                          << " vs " << b.sky << "\n"
                             "  贴着格壁的面必须锚在外面那一格；锚回自己这一格会读到它自己的\n"
                             "  天光 0（它不透光），顶面因此是 191 而不是 255。\n";
                ++failures;
                break;
            }
        }
    }

    // ---- 非空：这个夹具里确实有能区分「锚在里/外」的面 --------------------
    //
    // 若整格的光处处相同，上面那条相等断言用哪种锚点都会成立。活板门的面板缩在格内，
    // 它的下表面因此读的是自己这一格周围——与贴壁的面明显不同。少了这一条，
    // 一个「所有面一律锚在外」的实现也能让上面全绿，而那会毁掉压力板、地毯、
    // 活板门与下半砖顶面的取样基点（RN-19c 专门修过它）。
    {
        const auto trapdoor = centreVertices(
            withBlock(BlockState{Block::OakTrapdoor,
                                 mc::world::defaultOrientation(Block::OakTrapdoor), 0U}));
        require(!trapdoor.empty(), "活板门必须产出顶点");
        int darkest = 255;
        for (const auto& sample : trapdoor) {
            darkest = std::min(darkest, sample.sky);
        }
        require(darkest < 230,
                "活板门必须有明显暗于满值的面——否则这个夹具区分不出锚点，"
                "上面那条相等断言就什么也没证明");
    }

    // ---- 上半砖：它的侧面与顶面贴壁，同样该锚在外 ------------------------
    {
        const auto top = centreVertices(
            withBlock(BlockState{Block::StoneSlab,
                                 mc::world::defaultOrientation(Block::StoneSlab), 0U}
                          .withSlabPortion(SlabPortion::Top)));
        int sides = 0;
        int litSides = 0;
        for (const auto& sample : top) {
            const auto normal = mc::render::kVertexNormals[sample.normalIndex];
            if (std::fabs(normal.y) > 0.1F) {
                continue;   // 只看侧面
            }
            ++sides;
            if (sample.sky == 255) {
                ++litSides;
            }
        }
        require(sides > 0, "上半砖必须有侧面顶点");
        require(sides == litSides,
                "上半砖的侧面贴着格壁，在满天光下必须是满值——它曾经是 247..251");

        // ★ 下面两条是补出来的：上面那些断言只钉住了「贴壁的面锚在外」，
        // 于是一个「所有面一律锚在外」的实现、以及一个「正负面取反」的实现，都能全绿。
        // 上半砖同时带着两种面，正好把两个方向分开。
        int bottomMin = 255;
        int topMax = 0;
        for (const auto& sample : top) {
            const auto normal = mc::render::kVertexNormals[sample.normalIndex];
            if (normal.y < -0.5F) {
                bottomMin = std::min(bottomMin, sample.sky);
            } else if (normal.y > 0.5F) {
                topMax = std::max(topMax, sample.sky);
            }
        }
        // 它的底面在 y=0.5，**缩在格内**，取的是自己周围那一圈；锚到外面就会去采
        // 脚下那块石头——一个不透光的格子，天光 0。
        require(bottomMin > 0,
                "上半砖的底面缩在格内，不该采到脚下那块实心方块的 0"
                "（「所有面一律锚在外」会让它变成 0）");
        // 它的顶面在 y=1.0，**贴壁**，锚在外面那一格，因此至少有一角是满值。
        require(topMax == 255,
                "上半砖的顶面贴着格壁，至少有一角应当是满值"
                "（把正负面取反会让它停在 251）");
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "shaped_face_anchor ok\n";
    return 0;
}

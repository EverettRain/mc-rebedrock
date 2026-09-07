// 平滑光照取平均之前，把光为 0 的邻居换成中心格的光。
//
// 现场报告：「平地上每放一个方块就会在周围生成一圈很明显的阴影，比原版用力得多，
// 开太阳阴影时甚至强过太阳阴影」。量下来 AO 那一半一直是对的——地面环内角 0.80、
// 方块侧面贴地边 0.60，与 26.1 手算逐个数字相同。过暗全部来自**光照**那一半：
// `ringLight` 曾是朴素的四格平均，于是那个实心邻居的 0 被直接算了进去。
//
//   地面环内角   我们 0.80 x 0.75 = 0.60   vanilla 0.80 x 1.00 = 0.80   暗 25%
//   侧面贴地边   我们 0.60 x 0.50 = 0.30   vanilla 0.60 x 1.00 = 0.60   暗一半
//
// 26.1 的 `LightCoordsUtil.smoothBlend`（common/net/minecraft/util/LightCoordsUtil.java）：
//
//   if (sky(center) > 2 || block(center) > 2) {
//       if (sky(nX) == 0)   nX |= center 的 sky;
//       if (block(nX) == 0) nX |= center 的 block;   // 三个邻居各来一遍
//   }
//   return (n1 + n2 + n3 + center) >> 2;
//
// 这里钉三件事，每一件对应上面那段里的一个分句：
//   ① 中心够亮时，实心邻居的 0 不再拖低平均；
//   ② 中心不够亮时**不替换**——洞穴深处的对比度要保留，这道门不是修饰；
//   ③ sky 与 block **各判各的**——一格可能天光为 0 而方块光不为 0。
//
// 光照值直接写进世界（`setSkyLight` / `setBlockLight`）并用 `ChunkLightSampler{world}`
// 这一路取样（它的 `stored_` 为真，读的就是世界里存着的值），不靠摆几何去间接凑：
// 判据是这条混合规则，夹具应当把规则的输入摆到眼前，而不是让读者反推。
// —— 生产路径那个按位置**推导**天光的取样器会无视写进去的值，第一版正是那样写的，
// 于是四个样本恒为 15，三条断言全部落空。

#include "render/MeshData.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkMesher.hpp"
#include "world/WorldLighting.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using mc::world::Block;
using mc::world::Chunk;
using mc::world::World;

int failures = 0;

void require(bool condition, std::string_view what) {
    if (!condition) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

void requireNear(float actual, float expected, std::string_view what) {
    // 一个字节的分辨率，加一点平均两侧各自舍入的余量。
    if (std::fabs(actual - expected) > 3.0F / 255.0F) {
        std::cerr << "FAILED: " << what << ": expected " << expected << ", actual " << actual
                  << "\n";
        ++failures;
    }
}

constexpr int kFloorY = mc::world::kMinY;
constexpr int kBlockY = kFloorY + 1;

// 一块 5x5 石地板，正中央 (8, kBlockY, 8) 上面放一块石头——现场报告的那个场景。
// `sky` / `block` 是这一层空气格被强制写入的光照值。
[[nodiscard]] World placedBlockOnFloor(std::uint8_t sky, std::uint8_t block,
                                       std::uint8_t solidBlockLight = 0U) {
    World world;
    Chunk chunk;
    for (int z = 6; z <= 10; ++z) {
        for (int x = 6; x <= 10; ++x) {
            chunk.setBlock(x, kFloorY, z, Block::Stone);
        }
    }
    chunk.setBlock(8, kBlockY, 8, Block::Stone);
    world.setChunk({0, 0}, std::move(chunk));
    // 空气那一层统一给定光照；实心格保持 0，它正是被替换的那个样本。
    for (int z = 5; z <= 11; ++z) {
        for (int x = 5; x <= 11; ++x) {
            for (int y = kBlockY; y <= kBlockY + 2; ++y) {
                if (x == 8 && z == 8 && y == kBlockY) {
                    continue;  // 放置的那块石头
                }
                world.setSkyLight(x, y, z, sky);
                world.setBlockLight(x, y, z, block);
            }
        }
    }
    // 被放置的那块石头自己也可以带光。默认 0（两个通道都是 0，正是最常见的情形），
    // 但「各判各的」只有在它**一个通道为 0、另一个不为 0** 时才与「一起判」分道扬镳。
    world.setBlockLight(8, kBlockY, 8, solidBlockLight);
    return world;
}

struct Corner final {
    float ambientOcclusion = 0.0F;
    float sky = 0.0F;
    float block = 0.0F;
};

// 地板顶面上，最靠近被放置方块的那个角——报告里「一圈阴影」的最暗处。
// 它是 (9, kFloorY, 8) 这一格顶面在 x=9、z=8 处的顶点：那正好是与放置方块共享的棱。
[[nodiscard]] Corner ringCorner(const World& world) {
    const mc::world::ChunkLightSampler stored{world};
    const auto mesh = mc::world::ChunkMesher::buildSection(world, {0, 0}, 0, stored).mesh;
    const float topY = static_cast<float>(kFloorY - mc::world::kMinY + 1);
    Corner darkest{2.0F, 2.0F, 2.0F};
    bool found = false;
    for (const auto& vertex : mesh.vertices) {
        const auto normal = mc::render::kVertexNormals[vertex.normalIndex];
        const auto position = mc::render::decodeLocalPosition(vertex);
        if (normal.y < 0.5F || std::fabs(position.y - topY) > 0.01F) {
            continue;
        }
        // 与放置方块共享的那条棱上的顶点：x == 9 且 z 在 8..9 之间
        if (std::fabs(position.x - 9.0F) > 0.01F || position.z < 7.99F || position.z > 9.01F) {
            continue;
        }
        const float ao = mc::render::decodeAmbientOcclusion(vertex);
        if (ao < darkest.ambientOcclusion) {
            darkest = {ao, static_cast<float>(vertex.skyLight) / 255.0F,
                       static_cast<float>(vertex.blockLight) / 255.0F};
            found = true;
        }
    }
    require(found, "地板顶面必须有一个贴着放置方块的角，否则下面的断言无从谈起");
    return darkest;
}

// ① 中心够亮：实心邻居的 0 被换成中心格的光，平均因此不被拖低。
void testLitCentreSubstitutesTheSolidNeighbour() {
    const Corner corner = ringCorner(placedBlockOnFloor(/*sky=*/15U, /*block=*/0U));
    // AO 是 26.1 的 (0.2 + 1 + 1 + 1) / 4。它一直是对的，这里只是钉住「本轮没有动它」。
    requireNear(corner.ambientOcclusion, 0.8F, "地面环内角的 AO 仍是 vanilla 的 0.8");
    // 天光：替换之后四个样本全是 15，所以是满的。朴素平均会给 (15+15+15+0)/60 = 0.75。
    requireNear(corner.sky, 1.0F,
                "实心邻居的天光 0 必须换成中心格的 15——否则这个角比 vanilla 暗 25%");
}

// ② 中心不够亮：**不**替换。洞穴深处的对比度要保留。
//
// 这道门是 26.1 写死的 `sky(center) > 2 || block(center) > 2`。去掉它，全黑角落里的
// 那点光会被抹平；把阈值写成 `>= 2` 之类的近似同样会在这里现形。
void testDimCentreKeepsTheZero() {
    // 天光 2、方块光 0：正好在门槛上，规则说不替换。
    const Corner corner = ringCorner(placedBlockOnFloor(/*sky=*/2U, /*block=*/0U));
    requireNear(corner.sky, 2.0F * 3.0F / (4.0F * 15.0F),
                "中心格只有 2 级光时不替换：(2+2+2+0)/60");

    // 天光 3：门开了，四个样本都变成 3。
    const Corner lit = ringCorner(placedBlockOnFloor(/*sky=*/3U, /*block=*/0U));
    requireNear(lit.sky, 3.0F / 15.0F, "中心格 3 级光时替换：(3+3+3+3)/60");
    require(lit.sky > corner.sky, "门的两侧必须给出不同的答案，否则这条测试是空的");
}

// ③ sky 与 block 各判各的。
//
// 场景：天光全 0（地下），方块光 15（火把照着）。中心格的 block > 2 让门打开，
// 于是实心邻居的 block 被换成 15；而它的 sky 被换成中心格的 sky，也就是 0——
// 一个「两个通道一起判」的实现（例如「sky 与 block 都为 0 才替换」）在这里给出
// 同样的答案，所以真正把两者分开的是下面第二个场景。
void testChannelsAreDecidedIndependently() {
    {
        const Corner corner = ringCorner(placedBlockOnFloor(/*sky=*/0U, /*block=*/15U));
        requireNear(corner.block, 1.0F, "地下火把：方块光的 0 被换成中心格的 15");
        requireNear(corner.sky, 0.0F, "同一个角的天光仍然是 0——中心格自己就是 0");
    }
    {
        // 天光 15、方块光 8：两个通道都非零，替换对两者都生效。
        const Corner corner = ringCorner(placedBlockOnFloor(/*sky=*/15U, /*block=*/8U));
        requireNear(corner.sky, 1.0F, "天光通道被替换");
        requireNear(corner.block, 8.0F / 15.0F, "方块光通道也被替换，而不是只替换天光");
    }
    {
        // ★ 真正把「各判各的」与「两个通道一起判」分开的那个场景。
        //
        // 补出来的：上面两段里那块实心石头**两个通道都是 0**，于是
        // 「sky 与 block 都为 0 才替换」给出的答案与逐通道判断完全一样，
        // sabotage 打进去测试照样绿。给石头一点方块光（矿洞里被火把照到的墙就是这样），
        // 它的 sky 仍是 0 而 block 不是——两种实现在这里必须分开：
        //
        //   逐通道：sky 0 -> 中心的 15，block 6 保持 -> 天光 (15+15+15+15)/60 = 1.00
        //   一起判：block 不为 0，于是整格都不替换  -> 天光 (15+15+ 0+15)/60 = 0.75
        const Corner corner =
            ringCorner(placedBlockOnFloor(/*sky=*/15U, /*block=*/8U, /*solidBlockLight=*/6U));
        requireNear(corner.sky, 1.0F,
                    "实心格带着方块光时，它那个为 0 的天光通道仍然要被单独替换");
        requireNear(corner.block, (8.0F + 8.0F + 6.0F + 8.0F) / (4.0F * 15.0F),
                    "而它非零的方块光通道保持原值，不被中心格覆盖");
    }
}

} // namespace

int main() {
    testLitCentreSubstitutesTheSolidNeighbour();
    testDimCentreKeepsTheZero();
    testChannelsAreDecidedIndependently();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "smooth_light_zero_substitution ok\n";
    return 0;
}

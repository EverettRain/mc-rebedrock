#include "render/ParticleSystem.hpp"

#include "world/ChunkMesher.hpp"
#include "world/World.hpp"

#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <cassert>
#include <cmath>
#include <utility>

int main() {
    mc::world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    mc::world::World world;
    world.setChunk({0, 0}, std::move(chunk));

    mc::render::ParticleSystem particles;
    // 片数直接由 world::blockShape 推出，用的是 vanilla ClientLevel#addDestroyBlockEffect
    // 的算法：形状的每个盒子、每轴 max(2, ceil(宽 / 0.25)) 片
    // 这里断言的不是几个魔数，而是"粒子系统消费的确实是那份唯一的形状源"
    //   满立方体   宽 1     -> 4x4x4 = 64
    //   火把       kFloorTorchBox 0.125 x 0.625 x 0.125 -> 2x3x2 = 12
    //   十字植物   kCrossBox 0.8^3                      -> 4x4x4 = 64
    // 注：26.1 的 FlowerBlock 形状是 Block.column(6, 0, 10)（0.375 宽 x 0.625 高），
    // 按同一算法应得 2x3x2 = 12。差异出在 kCrossBox 这份形状数据本身——它让 18 种
    // cross 方块共用一个盒子——而不在本文件。形状一旦修正，粒子会自动跟上，这正是
    // 收编到单一形状源的目的。
    particles.spawnBlockBreak({4, 2, 4}, mc::world::Block::Grass);
    assert(particles.particles().size() == 64U);
    particles.spawnBlockBreak({4, 2, 4}, mc::world::Block::Torch);
    assert(particles.particles().size() == 64U + 12U);
    particles.spawnBlockBreak({4, 2, 4}, mc::world::Block::Dandelion);
    assert(particles.particles().size() == 64U + 12U + 64U);
    particles.spawnWaterSplash({4.5F, 2.0F, 4.5F});
    assert(particles.particles().size() == 64U + 12U + 64U + 10U);

    // 手抄的模型表漏掉的那一类：台阶只占下半格，因此只撒半数粉尘，且粉尘落在它自己
    // 的半格里而不是摊满整格。这在按 BlockModel 硬判的旧实现里是拿不到的
    {
        mc::render::ParticleSystem slab;
        slab.spawnBlockBreak({4, 2, 4}, mc::world::Block::StoneSlab);
        assert(slab.particles().size() == 32U);
        for (const auto& particle : slab.particles()) {
            assert(particle.position.y >= 2.0F && particle.position.y <= 2.5F);
        }
    }

    // 细瘦的盒子：火把粉尘必须落在火把那 2/16 宽的盒子里，不能撒满整格
    // 旧实现把盒内归一化偏移当成整格坐标用，粉尘因此铺满了 1x1 的底面
    {
        mc::render::ParticleSystem torch;
        torch.spawnBlockBreak({4, 2, 4}, mc::world::Block::Torch);
        for (const auto& particle : torch.particles()) {
            assert(particle.position.x >= 4.40F && particle.position.x <= 4.60F);
            assert(particle.position.z >= 4.40F && particle.position.z <= 4.60F);
            assert(particle.position.y >= 2.0F && particle.position.y <= 2.625F);
        }
    }
    // Vanilla block dust obeys gravity (0.04 blocks/tick^2 = 16 blocks/s^2):
    // the burst sheds its upward kick and settles on the floor rather than
    // flying away from the block in a straight line. A particle that ignored
    // gravity would still be rising well above the block after a second.
    particles.update(0.1F, world);
    assert(!particles.particles().empty());
    particles.update(1.0F, world);
    for (const auto& particle : particles.particles()) {
        assert(particle.position.y <= 1.5F);
    }
    particles.update(1.0F, world);
    assert(particles.particles().empty());

    // The 粒子效果 density knob scales the spawn counts: 高 (2x) doubles the
    // break burst exactly (every cell spawns a second jittered copy) and the
    // splash count, while 低 (0.5x) drops cells probabilistically so the total
    // lands somewhere under the full cube's 64.
    mc::render::ParticleSystem dense;
    dense.setLevelScale(2.0F);
    dense.spawnBlockBreak({4, 2, 4}, mc::world::Block::Grass);
    assert(dense.particles().size() == 128U);
    dense.spawnWaterSplash({4.5F, 2.0F, 4.5F});
    assert(dense.particles().size() == 128U + 20U);
    mc::render::ParticleSystem sparse;
    sparse.setLevelScale(0.5F);
    sparse.spawnBlockBreak({4, 2, 4}, mc::world::Block::Grass);
    assert(sparse.particles().size() < 64U && sparse.particles().size() > 8U);

    // A splash landing on water rests on the water's surface like it would on
    // any solid block: a rain splash over a pool never sinks below the surface
    // (the pool is water at y=0, surface y=1). Without the fluid-as-collision
    // fix the droplets fall past y=1 into the water body.
    for (int z = 12; z < 16; ++z) {
        for (int x = 12; x < 16; ++x) {
            world.setBlock(x, 0, z, mc::world::Block::Water);
        }
    }
    mc::render::ParticleSystem onWater;
    onWater.spawnRainImpact({14.0F, 1.0F, 14.0F}, true);
    assert(onWater.particles().size() == 1U);
    onWater.spawnRainSplash({14.0F, 2.5F, 14.0F});
    for (int step = 0; step < 8; ++step) {
        onWater.update(0.08F, world);
        for (const auto& particle : onWater.particles()) {
            assert(particle.position.y >= 1.0F);
        }
    }

    // Ambient weather owns at most 75% of the shared pool, leaving room for
    // interaction feedback even during continuous rain.
    mc::render::ParticleSystem weatherOnly;
    assert(weatherOnly.particleLimit() == 8000U);
    assert(weatherOnly.weatherParticleLimit() == 6000U);
    for (int index = 0; index < 8000; ++index) {
        weatherOnly.spawnRainImpact({0.0F, 2.0F, 0.0F}, false);
    }
    assert(weatherOnly.weatherParticleCount() == weatherOnly.weatherParticleLimit());
    assert(weatherOnly.particles().size() == weatherOnly.weatherParticleLimit());

    // If gameplay already occupies the reserved quarter and weather fills the
    // remaining global capacity, a new full-block break evicts 64 oldest
    // weather records and still emits all 64 dust pieces.
    mc::render::ParticleSystem priority;
    for (int event = 0; event < 32; ++event) {
        priority.spawnBlockBreak({event, 2, 4}, mc::world::Block::Grass);
    }
    while (priority.particles().size() < priority.particleLimit()) {
        priority.spawnRainImpact({0.0F, 2.0F, 0.0F}, false);
    }
    const std::size_t weatherBeforeBreak = priority.weatherParticleCount();
    priority.spawnBlockBreak({20, 2, 4}, mc::world::Block::Grass);
    assert(priority.particles().size() == priority.particleLimit());
    assert(priority.weatherParticleCount() + 64U == weatherBeforeBreak);

    // Landing splashes retain their own density multiplier: 疯狂 produces
    // twelve droplets per sampled landing rather than flattening back to four.
    mc::render::ParticleSystem crazyWeather;
    crazyWeather.setLevelScale(3.0F);
    assert(crazyWeather.particleLimit() == 24000U);
    assert(crazyWeather.weatherParticleLimit() == 18000U);
    crazyWeather.spawnRainSplash({0.0F, 2.0F, 0.0F});
    assert(crazyWeather.particles().size() == 12U);

    // RN-21 缺陷 1：采样水的图集层的粒子必须自带生物群系水色。
    //
    // BM-1 之后图集存的是未 tint 的原图，而 particle_instanced.vert 的 decodeTint 把
    // 0 解成白色，所以「不给 tint」= 白乘灰白 = 灰白。三个发射点各查一次：tint 非零，
    // 且等于取色单一源 world::biomeTintAt 给出的那个值。
    {
        mc::world::World world;
        mc::world::Chunk chunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                chunk.setBlock(x, 1, z, mc::world::Block::Stone);
            }
        }
        world.setChunk({0, 0}, std::move(chunk));
        const auto color = mc::world::biomeTintAt(world, mc::world::BiomeTintKind::Water, 8, 8);
        const std::uint32_t waterTint = mc::render::packParticleTint(
            {static_cast<float>(color[0]) / 255.0F, static_cast<float>(color[1]) / 255.0F,
             static_cast<float>(color[2]) / 255.0F});
        assert(waterTint != mc::render::kNoParticleTint);

        const auto allTinted = [&](const mc::render::ParticleSystem& system) {
            if (system.particles().empty()) {
                return false;
            }
            for (const auto& particle : system.particles()) {
                if (particle.tint != waterTint) {
                    return false;
                }
            }
            return true;
        };

        mc::render::ParticleSystem bucket;
        bucket.spawnWaterSplash({8.5F, 2.0F, 8.5F}, waterTint);
        assert(allTinted(bucket));

        mc::render::ParticleSystem impact;
        impact.spawnRainImpact({8.5F, 2.0F, 8.5F}, true, waterTint);
        assert(allTinted(impact));

        mc::render::ParticleSystem splash;
        splash.spawnRainSplash({8.5F, 2.0F, 8.5F}, {0.0F, 0.0F}, waterTint);
        assert(allTinted(splash));

        // 不给 tint 的那条路仍然是「不着色」，也就是今天四个发射点的行为：
        // 这条断言存在的意义是让「把 tint 改回 0」这类回退立刻变红，而不是静默变灰白。
        mc::render::ParticleSystem untinted;
        untinted.spawnRainSplash({8.5F, 2.0F, 8.5F});
        assert(!untinted.particles().empty());
        assert(untinted.particles().front().tint == mc::render::kNoParticleTint);

        // 破坏粉尘走同一条轴：vanilla 的 TerrainParticle 按方块的 tintSource 着色，
        // 树叶属于会着色的那一类（BlockTintSources.foliage() 没有覆写
        // colorAsTerrainParticle），草方块则专门覆写成白。
        assert(mc::world::biomeTintKind(mc::world::Block::OakLeaves,
                                        mc::world::Face::PositiveY) ==
               mc::world::BiomeTintKind::Foliage);
        const auto leafColor =
            mc::world::biomeTintAt(world, mc::world::BiomeTintKind::Foliage, 8, 8);
        const std::uint32_t leafTint = mc::render::packParticleTint(
            {static_cast<float>(leafColor[0]) / 255.0F,
             static_cast<float>(leafColor[1]) / 255.0F,
             static_cast<float>(leafColor[2]) / 255.0F});
        mc::render::ParticleSystem leaves;
        leaves.spawnBlockBreak({8, 2, 8}, mc::world::Block::OakLeaves, leafTint);
        assert(!leaves.particles().empty());

        // 26.1 `TerrainParticle` 的构造：先无条件 `rCol = gCol = bCol = 0.6F`，
        // **然后**才 `*=` tintSource 的颜色。两条规则相乘，不是逐发射压过类型表——
        // 这条断言从前写的是 `== leafTint`，也就是把那个 0.6 整个丢掉。
        const auto expectedLeaf = mc::render::packParticleTint(
            mc::render::unpackParticleTint(leafTint) * 0.6F);
        assert(leaves.particles().front().tint == expectedLeaf);
        assert(expectedLeaf != leafTint && "0.6 必须真的改变了结果，否则这条断言是空的");

        // 不带群系色的方块（石头）同样要暗一档：0.6 来自类型表，与发射方给不给色无关。
        // 它**不是**哨兵——哨兵的含义是「发射方没给颜色」，而这里颜色确实是 0.6 灰。
        mc::render::ParticleSystem stone;
        stone.spawnBlockBreak({8, 2, 8}, mc::world::Block::Stone);
        assert(!stone.particles().empty());
        assert(stone.particles().front().tint ==
               mc::render::packParticleTint({0.6F, 0.6F, 0.6F}));
        assert(stone.particles().front().tint != mc::render::kNoParticleTint);

        // 而雨和水花那几种的类型表是白，相乘是恒等——RN-21 的行为一个字节都不该动。
        // 上面那三条 allTinted 已经钉住了「给了色就照实带上」，这里补「没给色仍是哨兵」。
        mc::render::ParticleSystem plainImpact;
        plainImpact.spawnRainImpact({8.5F, 2.0F, 8.5F}, true);
        assert(!plainImpact.particles().empty());
        assert(plainImpact.particles().front().tint == mc::render::kNoParticleTint);
    }

    // 着色不得消耗随机数（除非亮度区间真的非退化）。
    //
    // 这一段是补出来的：sabotage「亮度抖动无条件抽一次随机数」在上面每一条断言下都是
    // 绿的——颜色一个字节没变，变的是**随机流的相位**。每多抽一次，后面每一颗粒子的
    // 尺寸、速度、寿命就整体移位一格，而那是逐帧可见的、又没有任何断言看着的。
    //
    // 判据落在尺寸序列上：尺寸在 `add()` **之前**于发射点抽出，所以 add 里多抽一次会
    // 让**下一颗**的尺寸改变。第 0 颗因此不动，第 1 颗起全变——断言取前八颗。
    // 这些是从当前实现捕获的黄金值，它们的意义不是「必须是这些数」，而是
    // 「不要在不该抽的时候抽随机数」；真要改随机流，连同这里一起改并说明理由。
    {
        mc::render::ParticleSystem dust;
        dust.spawnBlockBreak({8, 2, 8}, mc::world::Block::Stone);
        assert(dust.particles().size() == 64U);
        constexpr float kExpectedSizes[8] = {
            0.071228199F, 0.092889383F, 0.098835059F, 0.076254770F,
            0.071592093F, 0.050190657F, 0.065863699F, 0.066037469F,
        };
        for (std::size_t index = 0; index < 8U; ++index) {
            const float actual = dust.particles()[index].size;
            assert(std::fabs(actual - kExpectedSizes[index]) < 1e-6F &&
                   "破坏粉尘的尺寸序列变了：着色路径多抽或少抽了随机数");
        }
    }
    return 0;
}

#include "render/ParticleSystem.hpp"

#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <cassert>
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
    return 0;
}

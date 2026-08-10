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
    // A full cube breaks into the vanilla 4x4x4 = 64 dust pieces, while the
    // smaller outline shapes (torch 2x3x2, cross plant 3x4x3) shed fewer.
    particles.spawnBlockBreak({4, 2, 4}, mc::world::Block::Grass);
    assert(particles.particles().size() == 64U);
    particles.spawnBlockBreak({4, 2, 4}, mc::world::Block::Torch);
    assert(particles.particles().size() == 64U + 12U);
    particles.spawnBlockBreak({4, 2, 4}, mc::world::Block::Dandelion);
    assert(particles.particles().size() == 64U + 12U + 36U);
    particles.spawnWaterSplash({4.5F, 2.0F, 4.5F});
    assert(particles.particles().size() == 64U + 12U + 36U + 10U);
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

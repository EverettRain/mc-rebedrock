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
    return 0;
}

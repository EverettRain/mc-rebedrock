// RN-9d/9e：客户端 animateTick 派发层与它的第一个消费者（附魔台的银河字母）。
//
// 断言的不是「附魔台会冒粒子」这一件事，而是这一层的三个契约：
//   1. 派发靠表，表里没有的方块一颗粒子都不发；
//   2. 表项拿到的世界判定与服务端权威判定是同一份（书架的传导规则）；
//   3. 抽样有半径，走远了就不再发。

#include "render/BlockAnimateTick.hpp"
#include "render/ParticleSystem.hpp"
#include "render/ParticleTypes.hpp"

#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <cassert>
#include <cstddef>
#include <utility>

namespace {

using mc::render::ParticleType;

constexpr int kTableX = 8;
constexpr int kTableY = 2;
constexpr int kTableZ = 8;

// 一张只有石头地面的平地，附魔台立在 (8,2,8)。
mc::world::World buildWorld() {
    mc::world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, mc::world::Block::Stone);
            chunk.setBlock(x, 1, z, mc::world::Block::Stone);
        }
    }
    mc::world::World world;
    world.setChunk({0, 0}, std::move(chunk));
    world.setBlock(kTableX, kTableY, kTableZ, mc::world::Block::EnchantingTable);
    return world;
}

// 在附魔台同层的四个正对方向、距离 2 格处摆书架，中间那一格留空（传导）。
void placeBookshelves(mc::world::World& world) {
    world.setBlock(kTableX + 2, kTableY, kTableZ, mc::world::Block::Bookshelf);
    world.setBlock(kTableX - 2, kTableY, kTableZ, mc::world::Block::Bookshelf);
    world.setBlock(kTableX, kTableY, kTableZ + 2, mc::world::Block::Bookshelf);
    world.setBlock(kTableX, kTableY, kTableZ - 2, mc::world::Block::Bookshelf);
}

std::size_t countEnchantParticles(const mc::render::ParticleSystem& particles) {
    std::size_t count = 0;
    for (const auto& particle : particles.particles()) {
        if (particle.type == ParticleType::Enchant) {
            ++count;
        }
    }
    return count;
}

// 跑 `ticks` 个客户端 tick，返回期间累计出现过的附魔粒子数。
// 粒子寿命 30-39 tick，这里的 tick 数远小于它，所以池子里的数量就是累计数。
std::size_t runTicks(mc::world::World& world, const glm::vec3& camera, int ticks) {
    mc::render::ParticleSystem particles;
    mc::render::BlockAnimateTicker ticker;
    for (int tick = 0; tick < ticks; ++tick) {
        ticker.update(1.0F / 20.0F, camera, world, particles);
    }
    assert(static_cast<int>(ticker.tickCount()) == ticks);
    return countEnchantParticles(particles);
}

} // namespace

int main() {
    const glm::vec3 nearCamera{static_cast<float>(kTableX) + 0.5F,
                               static_cast<float>(kTableY) + 1.5F,
                               static_cast<float>(kTableZ) + 0.5F};

    // 1) 摆够书架 -> 若干 tick 内确实发出粒子。
    //    单个 tick 的期望产量很低（抽中该格约 2%，每个偏移再 1/16），所以给足 tick 数。
    {
        auto world = buildWorld();
        placeBookshelves(world);
        const std::size_t spawned = runTicks(world, nearCamera, 600);
        assert(spawned > 0U);
    }

    // 2) 一颗书架都没有 -> 一颗粒子都不发。附魔台被抽中过很多次，
    //    发不出来的原因只能是 isValidBookShelf 判定，不是没被抽中。
    {
        auto world = buildWorld();
        assert(runTicks(world, nearCamera, 600) == 0U);
    }

    // 3) 书架在，但中间那一格被实心方块堵死 -> 不发。
    //    这是 EnchantingTableBlock#isValidBookShelf 的传导规则，
    //    表现层必须与服务端的附魔力判定用同一份实现，否则会出现
    //    「冒粒子但没有附魔力」或反过来。
    {
        auto world = buildWorld();
        placeBookshelves(world);
        world.setBlock(kTableX + 1, kTableY, kTableZ, mc::world::Block::Stone);
        world.setBlock(kTableX - 1, kTableY, kTableZ, mc::world::Block::Stone);
        world.setBlock(kTableX, kTableY, kTableZ + 1, mc::world::Block::Stone);
        world.setBlock(kTableX, kTableY, kTableZ - 1, mc::world::Block::Stone);
        assert(runTicks(world, nearCamera, 600) == 0U);
    }

    // 4) 相机远到抽样半径（近 16 / 远 32）之外 -> 不发。
    //    这条钉住的是「派发是以相机为中心抽样的」，而不是全世界扫描。
    {
        auto world = buildWorld();
        placeBookshelves(world);
        const glm::vec3 farCamera{static_cast<float>(kTableX) + 400.0F, nearCamera.y,
                                  static_cast<float>(kTableZ) + 400.0F};
        assert(runTicks(world, farCamera, 600) == 0U);
    }

    // 5) 表里没有的方块不派发：附魔台换成书架本身（书架没有 animateTick 表项），
    //    周围书架照旧 -> 不发。派发靠表，不靠位置。
    {
        auto world = buildWorld();
        placeBookshelves(world);
        world.setBlock(kTableX, kTableY, kTableZ, mc::world::Block::Bookshelf);
        assert(runTicks(world, nearCamera, 600) == 0U);
    }
    assert(mc::render::animateTickOf(mc::world::Block::Bookshelf) == nullptr);
    assert(mc::render::animateTickOf(mc::world::Block::EnchantingTable) != nullptr);

    // 6) 生成出来的粒子确实是 FlyTowards 那一族：不吃重力，位置是寿命的参数化函数，
    //    从「锚点 + 偏移」出发、向锚点收敛。这里直接用类型层验，不依赖抽样运气。
    {
        auto world = buildWorld();
        mc::render::ParticleSystem particles;
        const glm::vec3 anchor{10.0F, 20.0F, 30.0F};
        const glm::vec3 offset{2.0F, -1.5F, 0.5F};
        assert(particles.add(mc::render::ParticleSpawn{
            .type = ParticleType::Enchant,
            .position = anchor,
            .motion = offset,
            .size = 0.05F,
            .lifetimeSeconds = 2.0F,
        }));
        // 生成的一刻在「锚点 + 偏移」，也就是书架那一侧
        assert(particles.particles().front().position == anchor + offset);
        // 推进到寿命的一半：位置应在 anchor + offset/2 再减去下坠项，
        // 而不是被重力拉走。y 方向的下坠是 (age/lifetime)^4 * 1.2
        particles.update(1.0F, world);
        const auto& midway = particles.particles().front();
        const glm::vec3 expected =
            anchor + offset * 0.5F - glm::vec3{0.0F, 0.5F * 0.5F * 0.5F * 0.5F * 1.2F, 0.0F};
        assert(std::abs(midway.position.x - expected.x) < 1e-4F);
        assert(std::abs(midway.position.y - expected.y) < 1e-4F);
        assert(std::abs(midway.position.z - expected.z) < 1e-4F);
        // 偏蓝的 tint 与随寿命增长的自发光都已挂上
        assert(midway.tint != mc::render::kNoParticleTint);
        assert(midway.emission > 0.0F);
    }

    return 0;
}

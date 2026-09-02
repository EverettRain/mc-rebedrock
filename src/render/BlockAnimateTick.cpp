#include "render/BlockAnimateTick.hpp"

#include "gameplay/EnchantingTable.hpp"
#include "gameplay/Random.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <cmath>

namespace mc::render {

// ---- RN-9e：第一个消费者 —— 附魔台的银河字母 ----
//
// EnchantingTableBlock#animateTick（26.1 `:69-85`）逐字：
//
//   for (BlockPos offset : BOOKSHELF_OFFSETS)
//       if (random.nextInt(16) == 0 && isValidBookShelf(level, pos, offset))
//           level.addParticle(ParticleTypes.ENCHANT,
//               pos.getX() + 0.5, pos.getY() + 2.0, pos.getZ() + 0.5,
//               offset.getX() + random.nextFloat() - 0.5,
//               offset.getY() - random.nextFloat() - 1.0,
//               offset.getZ() + random.nextFloat() - 0.5);
//
// 偏移量在 vanilla 里作为「速度」传入，实际被 FlyTowardsPositionParticle 当成
// **起点相对终点的位移**：粒子从 (锚点 + 偏移) 出发，寿命末收敛到锚点，
// 也就是从书架飞向附魔台，不是反过来。
//
// 书架判定复用服务端已有的 `gameplay::bookshelfOffsets()` / `isValidBookShelf()`
// ——那是 ENCH-2 的权威实现，包含「中间那一格必须能传导」的规则。这里绝不重抄一份：
// 抄一份就意味着表现与实际附魔力可以对不上。
void animateTickEnchantingTable(const AnimateTickContext& context) {
    const glm::ivec3 table = context.position;
    // FlyTowardsPositionParticle 的终点：附魔台正上方 2 格的格心
    const glm::vec3 anchor{static_cast<float>(table.x) + 0.5F,
                           static_cast<float>(table.y) + 2.0F,
                           static_cast<float>(table.z) + 0.5F};
    for (const glm::ivec3& offset : gameplay::bookshelfOffsets()) {
        if (rng::nextInt(context.random, 16U) != 0U) {
            continue;
        }
        if (!gameplay::isValidBookShelf(context.world, table, offset)) {
            continue;
        }
        const glm::vec3 start{
            static_cast<float>(offset.x) + rng::nextFloat(context.random) - 0.5F,
            static_cast<float>(offset.y) - rng::nextFloat(context.random) - 1.0F,
            static_cast<float>(offset.z) + rng::nextFloat(context.random) - 0.5F,
        };
        static_cast<void>(context.particles.add(ParticleSpawn{
            .type = ParticleType::Enchant,
            .position = anchor,
            .motion = start,
            // quadSize = 0.1 * (rand * 0.5 + 0.2)
            .size = 0.1F * (rng::nextFloat(context.random) * 0.5F + 0.2F),
            // lifetime = (int)(rand * 10) + 30 个 tick
            .lifetimeSeconds =
                (static_cast<float>(static_cast<int>(rng::nextFloat(context.random) * 10.0F)) +
                 30.0F) /
                20.0F,
        }));
    }
}

// ---- 派发 ----

void BlockAnimateTicker::reset() {
    accumulator_ = 0.0F;
    tick_ = 0U;
    random_ = 0U;
}

void BlockAnimateTicker::update(float deltaSeconds, const glm::vec3& cameraPosition,
                                const world::World& world, ParticleSystem& particles) {
    constexpr float kTickSeconds = 1.0F / 20.0F;
    accumulator_ += std::max(deltaSeconds, 0.0F);
    // 长时间卡顿或刚换世界时不要把积压的 tick 一次补完 —— 那会在一帧里发出几百颗
    // 粒子。上限取 4 个 tick，与 vanilla 客户端 tick 追赶的量级相当
    constexpr int kMaximumCatchUpTicks = 4;
    if (accumulator_ > kTickSeconds * static_cast<float>(kMaximumCatchUpTicks)) {
        accumulator_ = kTickSeconds * static_cast<float>(kMaximumCatchUpTicks);
    }
    const glm::ivec3 cameraBlock{
        static_cast<int>(std::floor(cameraPosition.x)),
        static_cast<int>(std::floor(cameraPosition.y)),
        static_cast<int>(std::floor(cameraPosition.z)),
    };
    while (accumulator_ >= kTickSeconds) {
        accumulator_ -= kTickSeconds;
        ++tick_;
        runTick(cameraBlock, world, particles);
    }
}

void BlockAnimateTicker::runTick(const glm::ivec3& cameraBlock, const world::World& world,
                                 ParticleSystem& particles) {
    // 每个 tick 开一条以 tick 号为种子的 Java LCG 流。整条流在本 tick 内既选格子
    // 也喂给表项，与 vanilla 用 level.random 选位置、用 animateRandom 喂 animateTick
    // 的两条流不同——那两条流在 vanilla 里也只是各自独立，没有任何跨 tick 语义，
    // 合成一条不改变分布，却让「同一个 tick 的表现可复现」这件事只需要一个种子
    random_ = rng::seedFromValue(tick_ * 0x9E3779B97F4A7C15ULL);
    for (int sample = 0; sample < kSamplesPerTick; ++sample) {
        sampleOnce(cameraBlock, kNearRadius, world, particles);
        sampleOnce(cameraBlock, kFarRadius, world, particles);
    }
}

void BlockAnimateTicker::sampleOnce(const glm::ivec3& cameraBlock, int radius,
                                    const world::World& world, ParticleSystem& particles) {
    // ClientLevel#doAnimateTick：三个轴各取 nextInt(r) - nextInt(r)，
    // 得到一个中心密、边缘疏的三角分布，而不是均匀立方体
    const auto bound = static_cast<std::uint32_t>(radius);
    const glm::ivec3 position{
        cameraBlock.x + static_cast<int>(rng::nextInt(random_, bound)) -
            static_cast<int>(rng::nextInt(random_, bound)),
        cameraBlock.y + static_cast<int>(rng::nextInt(random_, bound)) -
            static_cast<int>(rng::nextInt(random_, bound)),
        cameraBlock.z + static_cast<int>(rng::nextInt(random_, bound)) -
            static_cast<int>(rng::nextInt(random_, bound)),
    };
    if (!world::isWorldYInRange(position.y)) {
        return;
    }
    // 未加载的列读回空气，与 World::block/state 的既有约定一致，因此这里不必
    // 另外问「这块加载了吗」
    const world::BlockState state = world.state(position.x, position.y, position.z);
    const AnimateTickFn entry = animateTickOf(state.block());
    if (entry == nullptr) {
        return;
    }
    entry(AnimateTickContext{world, position, state, random_, particles});
}

} // namespace mc::render

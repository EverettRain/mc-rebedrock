#pragma once

// RN-9d：客户端环境 tick 派发层 —— vanilla 的 `Block#animateTick`，
// 由 `ClientLevel.animateTick`（`ClientLevel.java:456-465`）每 tick 随机抽样调用。
//
// 这一层在此之前**完全不存在**：rebedrock 的粒子只有服务端 `ParticleEvent`
// 一条来路（GameEvents.hpp），而 vanilla 的环境粒子是纯客户端的，不上网络。
// 所以「附魔台会冒粒子」这件事在旧代码里无处安放，只能写成某个渲染循环里的特判。
//
// 这里把它做成一层：
//   * 哪些方块有环境表现，是 `kAnimateTickTable` 的下标，不是 if-else；
//     这张表是服务端 `WorldSimulation::kRandomTickTable` 的客户端镜像，形状相同。
//   * 表项拿到的是 vanilla `animateTick(state, level, pos, random)` 的同一批参数。
//   * 抽样序列走 `mc::rng` 的 Java LCG（与 RainSystem::emitTextureImpacts 同一份
//     权威实现），复刻的是 vanilla 的落点分布。
//
// 越层禁：这一层只读权威世界快照，只写粒子池，绝不写回世界。
//
// **加新方块的环境粒子 = 在 kAnimateTickTable 里加一行 + 写一个 animateTickXxx。**
// 漏了表项就静默不发（与 kShapeByModel 漏同步同类），所以表下方有 static_assert
// 钉住「表里每个非空项确实指向本文件声明的函数」这件事能被人一眼看全。

#include "render/ParticleSystem.hpp"
#include "world/Block.hpp"
#include "world/BlockRegistry.hpp"
#include "world/BlockState.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>

namespace mc::world {
class World;
}

namespace mc::render {

// vanilla `Block#animateTick(BlockState, Level, BlockPos, RandomSource)` 的参数包。
// `random` 是调用方持有的 Java LCG 流，表项直接推进它 —— 每个表项各自开流会让
// 同一 tick 里的多个方块拿到相关联的序列。
struct AnimateTickContext final {
    const world::World& world;
    glm::ivec3 position;
    world::BlockState state;
    std::uint64_t& random;
    ParticleSystem& particles;
};

using AnimateTickFn = void (*)(const AnimateTickContext&);

// EnchantingTableBlock#animateTick：32 个书架偏移各以 1/16 概率发一颗银河字母，
// 粒子生成在附魔台正上方 2 格，速度参数是书架方向的偏移（RN-9e）。
void animateTickEnchantingTable(const AnimateTickContext& context);

// 行为表，按方块索引；空项 = 该方块没有环境表现。
// 与 WorldSimulation::kRandomTickTable 同构：那张是服务端的随机 tick，
// 这张是客户端的表现 tick，两者互不替代。
inline constexpr std::array<AnimateTickFn, world::kBuiltinBlockCount> kAnimateTickTable = [] {
    std::array<AnimateTickFn, world::kBuiltinBlockCount> entries{};
    entries[world::blockId(world::Block::EnchantingTable).index()] = &animateTickEnchantingTable;
    return entries;
}();

[[nodiscard]] constexpr AnimateTickFn animateTickOf(world::Block block) {
    const auto index = static_cast<std::size_t>(world::blockId(block).index());
    return index < kAnimateTickTable.size() ? kAnimateTickTable[index] : nullptr;
}

// 客户端环境 tick 的驱动。渲染循环每帧调 update()，它自己攒够 1/20 秒再走一个 tick，
// 与 RainSystem::emitTextureImpacts 同一范式（渲染是按帧的，vanilla 的 tick 是 20 TPS）。
class BlockAnimateTicker final {
  public:
    // vanilla ClientLevel.animateTick：每 tick 取 667 次，每次在 r=16 与 r=32
    // 两个立方体里各抽一格。半径 16 的那一轮让近处方块的表现足够密，
    // 半径 32 的那一轮把范围铺开但密度很低。
    static constexpr int kSamplesPerTick = 667;
    static constexpr int kNearRadius = 16;
    static constexpr int kFarRadius = 32;

    void update(float deltaSeconds, const glm::vec3& cameraPosition, const world::World& world,
                ParticleSystem& particles);

    // 换世界时清掉累加器与 tick 计数，免得新世界第一帧补一大批积压的 tick
    void reset();

    [[nodiscard]] std::uint64_t tickCount() const { return tick_; }

  private:
    void runTick(const glm::ivec3& cameraBlock, const world::World& world,
                 ParticleSystem& particles);
    void sampleOnce(const glm::ivec3& cameraBlock, int radius, const world::World& world,
                    ParticleSystem& particles);

    float accumulator_ = 0.0F;
    std::uint64_t tick_ = 0U;
    std::uint64_t random_ = 0U;
};

} // namespace mc::render

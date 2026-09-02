#pragma once

#include "render/ParticleTypes.hpp"
#include "world/Block.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc::world {
class World;
}

namespace mc::render {

// 一个 CPU 模拟的粒子，所有种类共用这一种表示
// 速度与寿命都换算成秒，vanilla 的每 tick 量在生成处乘以 20 转过来
//
// 「这一颗是什么种类」是 `type`，外观与运动的其余部分从 kParticleTypes 查
// （ParticleTypes.hpp）。分类不再是这里的一个字段：它是种类的属性，
// 存两份就会有两份的不一致
struct BlockParticle final {
    ParticleType type = ParticleType::BlockDust;
    glm::vec3 position{0.0F};
    // Ballistic：速度，格/秒。
    // FlyTowards：起点相对 anchor 的偏移（vanilla 的 xd/yd/zd），不是速度
    glm::vec3 velocity{0.0F};
    // FlyTowards 的终点，也就是 vanilla 的 (xStart, yStart, zStart)。
    // Ballistic 不读它
    glm::vec3 anchor{0.0F};
    float textureLayer = 0.0F;
    glm::vec2 uvOrigin{0.0F};
    float uvScale = 0.25F;
    float size = 0.10F;
    float ageSeconds = 0.0F;
    float lifetimeSeconds = 0.8F;
    float opacity = 1.0F;
    // 打包的 RGB（见 packParticleTint）；0 = 不着色
    std::uint32_t tint = kNoParticleTint;
    // 本帧的自发光 0..1，由 emissionPower 逐帧算出
    float emission = 0.0F;
};

// 一次生成请求 —— vanilla 的 `Level#addParticle(type, x,y,z, xd,yd,zd)`。
// 这是粒子进入系统的**唯一**入口：新粒子是 kParticleTypes 的一行加一处发射，
// 不是 ParticleSystem 上一个新方法
struct ParticleSpawn final {
    ParticleType type = ParticleType::BlockDust;
    glm::vec3 position{0.0F};
    // Ballistic：初速，格/秒。FlyTowards：起点相对 position 的偏移
    glm::vec3 motion{0.0F};
    float size = 0.10F;
    float lifetimeSeconds = 0.8F;
    // 只在类型没有精灵集（kNoParticleSprite）时使用：方块粉尘取被挖方块自己的
    // 贴图，水花取水的贴图。有精灵集的类型从集里随机取一张，这三项被忽略
    float textureLayer = 0.0F;
    glm::vec2 uvOrigin{0.0F};
    float uvScale = 1.0F;
};

class ParticleSystem final {
public:
    // 粒子效果等级的密度旋钮，scale 同时乘在逐事件生成数量和存活上限上
    // 低档 0.5、中档 1.0、高档 2.0、疯狂 3.0
    // 选项加载或被循环切换时调用一次，不在每帧路径上
    void setLevelScale(float scale);

    void spawnBlockBreak(const glm::ivec3& blockPosition, world::Block block);
    void spawnWaterSplash(const glm::vec3& position);
    // 在采样到的固体或流体撞击点生成一个 vanilla 的 RainSplashParticle
    // 水面撞击稍微放大一点，否则在流动的水纹理上看不出来
    // 地面撞击保持原本的紧凑尺寸
    void spawnRainImpact(const glm::vec3& position, bool onWater);
    // 雨滴落到水面或固体地面时向外炸开的短命水花，对应 vanilla 的 SplashParticle
    // 数量刻意压小，否则连续降雨会把粒子表撑满
    // direction 非零时表示雨滴撞上的墙面外法线，水滴沿背离墙面的半圆扇形喷出而不是四散
    void spawnRainSplash(const glm::vec3& position, const glm::vec2& direction = {0.0F, 0.0F});

    // 唯一的生成入口。返回是否真的加进了池子（满了或天气预算用尽时返回 false，
    // 调用方据此中断自己的生成循环）
    bool add(const ParticleSpawn& spawn);

    void update(float deltaSeconds, const world::World& world);

    [[nodiscard]] const std::vector<BlockParticle>& particles() const {
        return particles_;
    }
    [[nodiscard]] std::size_t particleLimit() const { return particleLimit_; }
    [[nodiscard]] std::size_t weatherParticleCount() const { return weatherParticleCount_; }
    [[nodiscard]] std::size_t weatherParticleLimit() const { return weatherParticleLimit_; }

private:
    // 当前密度下一次事件该生成多少粒子，取整数部分加小数部分的概率进位
    // 因此长期期望正好是 base 乘以等级系数，而不是被截断后偏低
    [[nodiscard]] int scaledCount(int base);

    [[nodiscard]] float randomUnit();

    // 按种类推进一颗粒子。两条运动模型各一个分支，这是整层唯一的分派点
    void advanceBallistic(BlockParticle& particle, float elapsed, float airDrag,
                          float groundDrag, const world::World& world);
    static void advanceFlyTowards(BlockParticle& particle);

    // 玩法反馈优先于环境天气，天气最多只能占用池子的四分之三
    // 剩下那四分之一是留给挖掘、水桶这类即时反馈的硬预留
    // 池子满而预留又已被占用时，新的交互先用一趟线性扫描淘汰最旧的天气粒子再开始生成
    void reserveGameplayCapacity(std::size_t requested);
    [[nodiscard]] bool weatherCapacityAvailable() const;

    std::vector<BlockParticle> particles_;
    std::uint32_t randomState_ = 0x50415254U;
    // 当前密度下的存活粒子上限，基准 8000 并随 setLevelScale 缩放
    // 所有生成路径在表满之后一律跳过，不做扩容
    std::size_t particleLimit_ = 8000U;
    std::size_t weatherParticleLimit_ = 6000U;
    std::size_t weatherParticleCount_ = 0U;
    float levelScale_ = 1.0F;
};

} // namespace mc::render

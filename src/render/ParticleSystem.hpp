#pragma once

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

// 粒子的来源分类，决定它在池子满时的优先级
// 玩法反馈优先于环境天气，天气最多只能占用池子的四分之三
enum class ParticleCategory : std::uint8_t {
    Gameplay,
    Weather,
};

// 一个 CPU 模拟的粒子，方块粉尘、水花与雨滴溅射共用这一种表示
// 速度与寿命都换算成秒，vanilla 的每 tick 量在生成处乘以 20 转过来
struct BlockParticle final {
    glm::vec3 position{0.0F};
    glm::vec3 velocity{0.0F};
    float textureLayer = 0.0F;
    glm::vec2 uvOrigin{0.0F};
    float uvScale = 0.25F;
    float size = 0.10F;
    float ageSeconds = 0.0F;
    float lifetimeSeconds = 0.8F;
    float opacity = 1.0F;
    ParticleCategory category = ParticleCategory::Gameplay;
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

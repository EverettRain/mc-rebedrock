#pragma once

// RN-9c：粒子类型层 —— 对应 vanilla 的 `ParticleTypes` 注册表 + 每个 `Particle`
// 子类的构造参数。
//
// 从前 `ParticleSystem` 上是四个硬编码的 `spawnXxx` 方法，「一颗粒子长什么样、
// 怎么动」散在这四份实现里，没有「种类」这个概念：加一种粒子只能再加一个方法，
// 再抄一遍重力/阻力/落地/预算那套。这里把**粒子本身**（外观 + 运动）收成一张
// constexpr 表，唯一的生成入口是 `ParticleSystem::add`，对应 vanilla 的
// `Level#addParticle`。
//
// 边界：**发射**（多少颗、在哪、初速多少）不在这张表里，它留在各效果自己手里
// ——vanilla 也是这样（各 spawner / animateTick 各写各的）。统一的是粒子本身。
// 这条边界正是 `spawnBlockBreak` 的形状细分逻辑不必动的原因。
//
// 表里放数据不放函数指针：tint 不写成 `TintRule` 枚举 + switch，而写成
// `tintBase * U[brightnessMin, brightnessMax]` —— 附魔粒子的
// `{0.9,0.9,1.0} × U[0.4,1.0]` 逐字就是 vanilla 的
// `br = rand*0.6+0.4; rCol = gCol = 0.9*br; bCol = br`；不着色的类型填
// `{1,1,1} × U[1,1]`，同一条公式，无分支。

#include "render/vulkan/BlockAtlasLayout.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace mc::render {

// 粒子的来源分类，决定它在池子满时的优先级
// 玩法反馈优先于环境天气，天气最多只能占用池子的四分之三
enum class ParticleCategory : std::uint8_t {
    Gameplay,
    Weather,
};

// 位置怎么随时间走。对应 vanilla 每个 Particle 子类各自的 tick()，
// 这是表里唯一真正的分派点（两个分支），其余全是数据。
enum class ParticleMotion : std::uint8_t {
    // vanilla Particle#tick 的默认实现：重力 + 空气阻力 + 落地停住
    Ballistic,
    // FlyTowardsPositionParticle#tick：不积分速度，位置是寿命的参数化函数，
    // 从「锚点 + 偏移」出发、在寿命末收敛到锚点本身
    FlyTowards,
};

enum class ParticleType : std::uint8_t {
    // BlockDustParticle：挖掉一格时按形状撒出的地形粉尘
    BlockDust,
    // 水桶/入水的爆发水花
    WaterSplash,
    // vanilla ParticleTypes.RAIN，雨打在面上的那一颗向上水珠
    RainImpact,
    // SplashParticle，雨滴自己落地时四散的水珠
    RainSplash,
    // FlyTowardsPositionParticle + minecraft:enchant 精灵集，
    // 书架飞向附魔台的银河字母
    Enchant,
    Count,
};

// 「这一类没有自己的精灵集，纹理由发射侧给」。方块粉尘取被挖方块自己的贴图，
// 水花取水的贴图，两者都不是固定的一组精灵。
inline constexpr ParticleSprite kNoParticleSprite = ParticleSprite::Count;

struct ParticleTypeDefinition final {
    ParticleMotion motion = ParticleMotion::Ballistic;
    // vanilla Particle#hasPhysics：false 的粒子穿过方块，也不吃地面阻力
    bool collides = true;
    // 重力倍率，1.0 = vanilla 的每 tick² 0.04 格。FlyTowards 不读它
    float gravityScale = 1.0F;
    // 寿命末是否淡出。vanilla 的多数粒子直接消失（LifetimeAlpha.ALWAYS_OPAQUE），
    // 但本项目的地形/天气粒子一直带一段淡出尾巴，那是既有观感，保持
    bool fadesOut = true;
    ParticleCategory category = ParticleCategory::Gameplay;
    ParticleSprite sprite = kNoParticleSprite;
    // 颜色 = tintBase × U[brightnessMin, brightnessMax]，见文件头
    glm::vec3 tintBase{1.0F, 1.0F, 1.0F};
    float brightnessMin = 1.0F;
    float brightnessMax = 1.0F;
    // 自发光 = (age/lifetime)^emissionPower，0 表示不发光。
    // 4 是 FlyTowardsPositionParticle#getLightCoords 的
    // `brightness *= brightness; brightness *= brightness`
    float emissionPower = 0.0F;
};

inline constexpr std::array<ParticleTypeDefinition, static_cast<std::size_t>(ParticleType::Count)>
    kParticleTypes{{
        // BlockDust
        {},
        // WaterSplash
        {},
        // RainImpact
        {.category = ParticleCategory::Weather},
        // RainSplash
        {.category = ParticleCategory::Weather},
        // Enchant：不吃重力也不吃碰撞，寿命末直接消失，偏蓝，越靠近附魔台越亮
        {.motion = ParticleMotion::FlyTowards,
         .collides = false,
         .gravityScale = 0.0F,
         .fadesOut = false,
         .category = ParticleCategory::Gameplay,
         .sprite = ParticleSprite::Enchant,
         .tintBase = {0.9F, 0.9F, 1.0F},
         .brightnessMin = 0.4F,
         .brightnessMax = 1.0F,
         .emissionPower = 4.0F},
    }};

[[nodiscard]] constexpr const ParticleTypeDefinition& particleTypeOf(ParticleType type) {
    return kParticleTypes[static_cast<std::size_t>(type)];
}

[[nodiscard]] constexpr ParticleCategory categoryOf(ParticleType type) {
    return particleTypeOf(type).category;
}

// 枚举序数 == 表下标。加一种粒子而忘了在表里补一行，编译期就停在这里
// ——同 BlockShape 的 kShapeByModel 那道闸门（那张表漏同步过一次，代价是运行期 SIGBUS）
static_assert(kParticleTypes.size() == static_cast<std::size_t>(ParticleType::Count));
static_assert(particleTypeOf(ParticleType::Enchant).motion == ParticleMotion::FlyTowards);
static_assert(particleTypeOf(ParticleType::Enchant).sprite == ParticleSprite::Enchant);
static_assert(particleTypeOf(ParticleType::RainImpact).category == ParticleCategory::Weather);
static_assert(particleTypeOf(ParticleType::BlockDust).sprite == kNoParticleSprite);

// RGB 打包进 ParticleRecord.layerLight.z 的那一个 float（见 GpuSceneBuffer.hpp）。
// 0 是「不着色」的哨兵，所以纯黑不可表达 —— 没有粒子想要纯黑，而白色（0xFFFFFF）
// 仍在 fp32 的无损整数区间内。
[[nodiscard]] constexpr std::uint32_t packParticleTint(const glm::vec3& color) {
    const auto channel = [](float value) -> std::uint32_t {
        const float clamped = value < 0.0F ? 0.0F : (value > 1.0F ? 1.0F : value);
        return static_cast<std::uint32_t>(clamped * 255.0F + 0.5F);
    };
    return (channel(color.x) << 16U) | (channel(color.y) << 8U) | channel(color.z);
}

// 不着色的哨兵。着色器把 0 解成白色而不是黑色。
inline constexpr std::uint32_t kNoParticleTint = 0U;

} // namespace mc::render

#pragma once

// 方块/实体/特效纹理数组的固定层布局
// 烘焙侧和采样侧共用这一份起始层号，布局一改两边同时改，不会失配
// 烘焙侧是 TextureManager，采样侧是渲染器各绘制通道和天空着色器

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace mc::render {

// 烘进特殊区的水/岩浆动画帧数
inline constexpr std::uint32_t kWaterAnimationFrameCount = 32;
inline constexpr std::uint32_t kLavaStillFrameCount = 20;
inline constexpr std::uint32_t kLavaFlowFrameCount = 16;

// 固定特殊区里其余各段的层数
// 下面的 static_assert 用它们把每个起始层号推回去，起始层号因此不再是手算出来的字面量
// 烘焙侧（BlockAtlasBaker）在 append 每一段之前也用同一批常量校验一次实际写入位置
inline constexpr std::uint32_t kCuboidFaceCount = 6;   // 一个长方体展开成 6 个面
inline constexpr std::uint32_t kPlayerPartCount = 6;   // 头/身/左右臂/左右腿
inline constexpr std::uint32_t kDestroyStageCount = 10;
inline constexpr std::uint32_t kChestPartCount = 3;    // 底座/盖/搭扣
inline constexpr std::uint32_t kChestItemFaceCount = 3; // 箱子物品的顶/正/侧
inline constexpr std::uint32_t kMoonPhaseCount = 8;
inline constexpr std::uint32_t kSunFrameCount = 1;
inline constexpr std::uint32_t kExperienceOrbLayerCount = 1;

// ---- 粒子精灵集（RN-9a）----
//
// 一个精灵集对应 vanilla 的一份 `assets/minecraft/particles/<id>.json`，它的
// `textures` 数组就是下面那张名字表：粒子每次生成从集里随机取一张
// （`SpriteSet#get(random)`）。这里把「哪种粒子用哪些贴图」写成表，
// 而不是在图集布局里为某一种粒子手算一个层号——新粒子是加一条记录，不是加一个洞。
//
// 贴图本身不入库：运行时从用户自备资源包读，缺失回落棋盘格（版权铁律）。
enum class ParticleSprite : std::uint8_t {
    // minecraft:enchant —— 银河字母（Standard Galactic Alphabet），
    // 附魔台与海洋罗盘共用的那 26 张
    Enchant,
    Count,
};

// particles/enchant.json 的 textures 数组，逐字。
inline constexpr std::array<std::string_view, 26> kEnchantSpriteTextures{
    "sga_a", "sga_b", "sga_c", "sga_d", "sga_e", "sga_f", "sga_g", "sga_h", "sga_i",
    "sga_j", "sga_k", "sga_l", "sga_m", "sga_n", "sga_o", "sga_p", "sga_q", "sga_r",
    "sga_s", "sga_t", "sga_u", "sga_v", "sga_w", "sga_x", "sga_y", "sga_z",
};

struct ParticleSpriteSet final {
    // 这一集在图集里的第一层；集内第 i 张就是 firstLayer + i
    float firstLayer;
    // 贴图名（不带 `particle/` 前缀与 `.png` 后缀），顺序即层序
    std::span<const std::string_view> textures;
};

// 粒子精灵区紧接在经验球之后，是固定特殊区的最后一段
inline constexpr float kParticleSpriteFirstLayer = 177.0F;

inline constexpr std::array<ParticleSpriteSet, static_cast<std::size_t>(ParticleSprite::Count)>
    kParticleSpriteSets{{
        {kParticleSpriteFirstLayer, kEnchantSpriteTextures},
    }};

// 精灵区总层数 = 各集之和。改任何一集的张数，下面 kFirstBlockTextureLayer 的
// static_assert 会立刻把顺移的义务提出来
inline constexpr std::uint32_t kParticleSpriteLayerCount = [] {
    std::uint32_t total = 0U;
    for (const auto& set : kParticleSpriteSets) {
        total += static_cast<std::uint32_t>(set.textures.size());
    }
    return total;
}();

[[nodiscard]] constexpr const ParticleSpriteSet& particleSpriteSet(ParticleSprite sprite) {
    return kParticleSpriteSets[static_cast<std::size_t>(sprite)];
}

// 图集开头是一段顺序确定的固定特殊区，网格化器和天空着色器因此能用 constexpr 起始层号
// 特殊区含水与岩浆动画帧、玩家皮肤各面、箱子部件、破坏阶段、太阳与月相
// 特殊区之后是方块纹理，启动时按名字从方块注册表解析，没有占位层，每层都是真纹理
// 再往后是每个已注册物品各一层
// 总层数在运行期由建好的图集算出
inline constexpr std::uint32_t kWaterStillLayer = 0U;
inline constexpr std::uint32_t kWaterFlowLayer = 32U;
inline constexpr std::uint32_t kLavaStillLayer = 64U;
inline constexpr std::uint32_t kLavaFlowLayer = 84U;
// 第一层方块纹理的层号
// 它之前全部属于固定特殊区
// 水 0-63、岩浆 64-99、玩家皮肤 100-135、破坏阶段 136-145、箱子部件 146-163
// 箱子物品面 164-166、月相 167-174、太阳 175、经验球 176、粒子精灵 177-202
inline constexpr std::uint32_t kFirstBlockTextureLayer = 203U;
inline constexpr float kPlayerHeadFirstLayer = 100.0F;
inline constexpr float kPlayerBodyFirstLayer = 106.0F;
inline constexpr float kPlayerRightArmFirstLayer = 112.0F;
inline constexpr float kPlayerLeftArmFirstLayer = 118.0F;
inline constexpr float kPlayerRightLegFirstLayer = 124.0F;
inline constexpr float kPlayerLeftLegFirstLayer = 130.0F;
inline constexpr float kDestroyStageFirstLayer = 136.0F;
inline constexpr float kChestBaseFirstLayer = 146.0F;
inline constexpr float kChestLidFirstLayer = 152.0F;
inline constexpr float kChestItemTopLayer = 164.0F;
inline constexpr float kChestItemFrontLayer = 165.0F;
inline constexpr float kChestItemSideLayer = 166.0F;
// 八张 environment/celestial/moon/<phase>.png 占固定层 167-174
// environment/celestial/sun.png 占 175
// 天空着色器通过 CameraUniform.celestialLayers 读它们，该字段由渲染器用这里的起始层号填充
inline constexpr float kMoonPhaseFirstLayer = 167.0F;
inline constexpr float kSunLayer = 175.0F;
// 从 entity/experience/experience_orb.png（4x4 的表）里取出的一张 16x16 球体精灵
// WorldRenderer 里的经验球公告板直接采样这整层
inline constexpr float kExperienceOrbLayer = 176.0F;

// ---- 段间关系的编译期校验 ----
//
// 上面每个起始层号原本都是手算出来的字面量，段与段之间只靠注释和 BlockAtlasBaker
// 里的行尾 `// 100..135` 对齐。唯一的护栏是烘焙末尾那句总数校验
// （layers.size() != kFirstBlockTextureLayer），它只看总和：某一段少一层、另一段
// 多一层时总数不变，校验照过，程序照常启动，但箱子会去采样破坏阶段的贴图
//
// 下面把每个起点重新表述成「上一段起点 + 上一段层数」。改动任何一段的层数而忘了
// 顺移后面的起点，编译期就会停下来。范式同 world/BlockShape.hpp 用 static_assert
// 钉住枚举序数与表下标
static_assert(kWaterStillLayer == 0U);
static_assert(kWaterFlowLayer == kWaterStillLayer + kWaterAnimationFrameCount);
static_assert(kLavaStillLayer == kWaterFlowLayer + kWaterAnimationFrameCount);
static_assert(kLavaFlowLayer == kLavaStillLayer + kLavaStillFrameCount);

static_assert(kPlayerHeadFirstLayer ==
              static_cast<float>(kLavaFlowLayer + kLavaFlowFrameCount));
static_assert(kPlayerBodyFirstLayer == kPlayerHeadFirstLayer + kCuboidFaceCount);
static_assert(kPlayerRightArmFirstLayer == kPlayerBodyFirstLayer + kCuboidFaceCount);
static_assert(kPlayerLeftArmFirstLayer == kPlayerRightArmFirstLayer + kCuboidFaceCount);
static_assert(kPlayerRightLegFirstLayer == kPlayerLeftArmFirstLayer + kCuboidFaceCount);
static_assert(kPlayerLeftLegFirstLayer == kPlayerRightLegFirstLayer + kCuboidFaceCount);

static_assert(kDestroyStageFirstLayer ==
              kPlayerHeadFirstLayer + static_cast<float>(kPlayerPartCount * kCuboidFaceCount));
static_assert(kChestBaseFirstLayer ==
              kDestroyStageFirstLayer + static_cast<float>(kDestroyStageCount));
static_assert(kChestLidFirstLayer == kChestBaseFirstLayer + kCuboidFaceCount);
// 搭扣是第三个箱子部件，占 158..163，没有具名起点：物品面紧跟在三个部件之后
static_assert(kChestItemTopLayer ==
              kChestBaseFirstLayer + static_cast<float>(kChestPartCount * kCuboidFaceCount));
static_assert(kChestItemFrontLayer == kChestItemTopLayer + 1.0F);
static_assert(kChestItemSideLayer == kChestItemFrontLayer + 1.0F);

static_assert(kMoonPhaseFirstLayer ==
              kChestItemTopLayer + static_cast<float>(kChestItemFaceCount));
static_assert(kSunLayer == kMoonPhaseFirstLayer + static_cast<float>(kMoonPhaseCount));
static_assert(kExperienceOrbLayer == kSunLayer + static_cast<float>(kSunFrameCount));
static_assert(kParticleSpriteFirstLayer ==
              kExperienceOrbLayer + static_cast<float>(kExperienceOrbLayerCount));
// 每一集都紧接上一集，集与集之间不留洞：加一集只需在 kParticleSpriteSets 里
// 写出它的起点，这条 assert 负责证明那个起点没写错
static_assert([] {
    float expected = kParticleSpriteFirstLayer;
    for (const auto& set : kParticleSpriteSets) {
        if (set.firstLayer != expected) {
            return false;
        }
        expected += static_cast<float>(set.textures.size());
    }
    return true;
}());
static_assert(kFirstBlockTextureLayer ==
              static_cast<std::uint32_t>(kParticleSpriteFirstLayer) + kParticleSpriteLayerCount);

} // namespace mc::render

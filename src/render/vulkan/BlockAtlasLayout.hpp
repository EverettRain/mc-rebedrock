#pragma once

// 方块/实体/特效纹理数组的固定层布局
// 烘焙侧和采样侧共用这一份起始层号，布局一改两边同时改，不会失配
// 烘焙侧是 TextureManager，采样侧是渲染器各绘制通道和天空着色器

#include <cstddef>
#include <cstdint>

namespace mc::render {

// 烘进特殊区的水/岩浆动画帧数
inline constexpr std::uint32_t kWaterAnimationFrameCount = 32;
inline constexpr std::uint32_t kLavaStillFrameCount = 20;
inline constexpr std::uint32_t kLavaFlowFrameCount = 16;

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
// 箱子物品面 164-166、月相 167-174、太阳 175、经验球 176
inline constexpr std::uint32_t kFirstBlockTextureLayer = 177U;
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

} // namespace mc::render

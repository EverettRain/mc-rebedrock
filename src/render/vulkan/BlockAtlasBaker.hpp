#pragma once

// 方块、实体、特效纹理数组的 CPU 侧
// 按名字从方块、物品、实体注册表和群系定义解析出每张源图
// 烘到各自固定的图集层后返回打包好的 RGBA 层，层号见 BlockAtlasLayout
// 与 TextureManager 分开，好让渲染器的纹理层只依赖 VulkanResources
// 所有对玩法/世界内容的耦合都收在这里

#include "assets/ResourceProvider.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace mc::render {

// 一张按连续帧烘入的非流体动画方块纹理（岩浆块，以及后续加入的海晶石/海晶灯等）
// `baseLayer` 是首帧所在层
// 地形着色器按 `floor(tick/frameTime mod frameCount)` 往后推，与流体轮播同一套
// 任何带 `.mcmeta` 的方块动画条都会真的动起来，而不是只烘第 0 帧
struct BlockTextureAnimation final {
    float baseLayer = 0.0F;
    std::uint32_t frameCount = 0;
    float frameTime = 1.0F;
};

// 每层 `width * height * 4` 字节，`rgba` 里各层首尾相接
// 层数即 `rgba.size() / (width * height * 4)`
struct TextureArrayPixels final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
    // 水静止/流动、岩浆静止/流动各自的每帧停留 tick 数，从各纹理的 .mcmeta 读出后转交给地形着色器
    std::array<float, 4> fluidAnimationFrameTimes{1.0F, 1.0F, 1.0F, 1.0F};
    // 全部非流体动画方块纹理，顺序与烘焙顺序一致
    std::vector<BlockTextureAnimation> blockAnimations;
};

[[nodiscard]] TextureArrayPixels bakeBlockAtlas(const assets::ResourceProvider& resources);

} // namespace mc::render

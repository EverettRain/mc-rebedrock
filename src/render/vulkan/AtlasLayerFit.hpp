#pragma once

// 让方块图集扛得住不合规资源包的纯函数助手
// 图集的每一层都是参考尺寸的固定瓦片，参考尺寸取 grass_block_top
// 各层原样首尾相接，任何尺寸不符的层都会毁掉整个图集
// 而资源包完全可能给出别的分辨率
// 比如 HD 包、混进来的动画条，或在 HD 图集里以 16x16 顶上的品红缺失纹理占位
// 这里不让启动失败，而是把超规格的层最近邻缩放到参考尺寸
// 与流体动画适配器同样遵守"绝不因不合规资源包崩溃"的契约
// 独立成头文件（不碰 Vulkan 和方块/物品注册表），便于 headless 单测

#include "assets/ImageData.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <string_view>

namespace mc::render {

// --- vanilla ModelPart.Cube 的展开图（box-UV net）--------------------------------
//
// 一个 (w,h,d) 的方体在 (u0,v0) 处展开成六块矩形：
//
//        [ DOWN ][  UP  ]                 v0 .. v0+d
//   [-X][  -Z  ][  +X  ][  +Z  ]          v0+d .. v0+d+h
//
// 逐字取自 `ModelPart.Cube`：**DOWN 在 (u0+d, v0)、UP 在 (u0+d+w, v0)**。
//
// ⚠ 世界里的上下面对应哪一块，取决于调用者画这个方体时**有没有实体根节点的 Y 翻转**。
// vanilla 的 `EntityRenderer` 对生物/玩家做 `scale(-1,-1,1)`，模型的 Y 朝下，
// 所以生物**世界朝上**的那一面读的是 net 的 DOWN 块——皮肤里头顶在 (8,0) 正是这么来的。
// 方块实体没有这个翻转（`ChestRenderer` 只做偏航），所以箱子**世界朝上**的一面读 net 的 UP 块。
//
// 这两条曾经混在一起：箱子复用了为玩家写的展开函数，只给盖子手工 swap 了一次上下面，
// 底座没管，于是开盖后箱口那一面显示的是木板（与箱子顶面同一块像素），而不是有箱口的那一块。
struct NetRect final {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// 六块矩形，按物品着色器的面序：+X、-X、+Y、-Y、+Z、-Z。
// `entityYFlip` = 调用者是否经实体根节点的 Y 翻转绘制（生物/玩家为 true，方块实体为 false）。
[[nodiscard]] constexpr std::array<NetRect, 6> cuboidNetRects(int textureX, int textureY, int width,
                                                             int height, int depth,
                                                             bool entityYFlip) {
    const NetRect down{textureX + depth, textureY, width, depth};
    const NetRect up{textureX + depth + width, textureY, width, depth};
    return {
        NetRect{textureX + depth + width, textureY + depth, depth, height}, // +X
        NetRect{textureX, textureY + depth, depth, height},                 // -X
        entityYFlip ? down : up,                                            // +Y
        entityYFlip ? up : down,                                            // -Y
        NetRect{textureX + depth, textureY + depth, width, height},         // +Z
        NetRect{textureX + depth * 2 + width, textureY + depth, width, height}, // -Z
    };
}

// 把图像的一块区域最近邻缩放进目标矩形
// 下面那个方形目标的便捷重载，覆盖实体/GUI 展开图缩进方形图集瓦片的场景
[[nodiscard]] inline assets::ImageData resizedRegion(const assets::ImageData& image, int sourceX,
                                                     int sourceY, int sourceWidth, int sourceHeight,
                                                     int targetWidth, int targetHeight) {
    assets::ImageData result;
    result.width = targetWidth;
    result.height = targetHeight;
    result.rgba.resize(static_cast<std::size_t>(targetWidth * targetHeight * 4));
    for (int y = 0; y < targetHeight; ++y) {
        for (int x = 0; x < targetWidth; ++x) {
            const int sx = sourceX + x * sourceWidth / targetWidth;
            const int sy = sourceY + y * sourceHeight / targetHeight;
            const std::size_t source = static_cast<std::size_t>((sy * image.width + sx) * 4);
            const std::size_t target = static_cast<std::size_t>((y * targetWidth + x) * 4);
            std::copy_n(image.rgba.begin() + static_cast<std::ptrdiff_t>(source), 4,
                        result.rgba.begin() + static_cast<std::ptrdiff_t>(target));
        }
    }
    return result;
}

[[nodiscard]] inline assets::ImageData resizedRegion(const assets::ImageData& image, int sourceX,
                                                     int sourceY, int sourceWidth, int sourceHeight,
                                                     int targetSize) {
    return resizedRegion(image, sourceX, sourceY, sourceWidth, sourceHeight, targetSize,
                         targetSize);
}

// 返回一份尺寸保证与 `reference` 一致的 `layer` 拷贝，从而能安全接进定长图集
// 尺寸本就相符则原样返回
// 不符则最近邻缩放并打一行日志
// 退化的空图像回落成品红缺失纹理占位，保证烘焙无论如何都不会崩
[[nodiscard]] inline assets::ImageData conformToAtlasLayer(const assets::ImageData& reference,
                                                           const assets::ImageData& layer,
                                                           std::string_view name) {
    if (layer.width == reference.width && layer.height == reference.height) {
        return layer;
    }
    std::cerr << "[texture-atlas] " << name << " resized " << layer.width << 'x' << layer.height
              << " -> " << reference.width << 'x' << reference.height << " to fit the atlas.\n";
    if (layer.width <= 0 || layer.height <= 0) {
        return assets::ImageData::missingTexture(reference.width, reference.height);
    }
    return resizedRegion(layer, 0, 0, layer.width, layer.height, reference.width, reference.height);
}

// 岩浆块、海晶灯、海晶石这类竖直动画条不会被截到第 0 帧
// bakeBlockAtlas 把整条的每一帧连续烘进图集，并记一条 BlockTextureAnimation
// 地形着色器据此像轮播流体那样轮播它
// 非动画条的方块纹理直接走 conformToAtlasLayer

} // namespace mc::render

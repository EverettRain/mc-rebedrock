#pragma once

// 混合空间：vanilla 在 sRGB 编码值上做 alpha 混合，我们在线性空间做
//
// vanilla（OpenGL，普通 RGBA8 帧缓冲）从不做伽马转换：纹素是什么值就拿什么值
// 乘顶点色、什么值就拿什么值混合。本渲染器的交换链是 `B8G8R8A8_SRGB`，着色器
// 输出线性值，硬件因此在**线性空间**混合，混完再编码回 sRGB。
//
// 不透明的东西两边一模一样，所以整套 GUI 看起来都对。半透明的**深色**叠加层
// 不是：同一个 alpha 在线性空间里放走的背景亮得多。以提示框的填充
// （0xF0100010，alpha 240/255）压在浅灰界面（sRGB 0.776）上为例——
// 原版给出编码值 0.105，我们给出 0.215，整整两倍，肉眼就是"更透、更看不清字"。
//
// 正确的收口是让 GUI 也在编码值上混合（给交换链图像另开一个 UNORM 视图、
// HUD 单独一趟，需要 `VK_KHR_swapchain_mutable_format`），那是渲染器级的改动，
// 已登记欠账。在那之前，知道自己压在什么底上的叠加层可以解一次方程，换一个
// 在线性混合下产生同一结果的 alpha——这里就是那道方程，纯值且有单测。
#include <algorithm>
#include <cmath>

namespace mc::ui {

// IEC 61966-2-1 的传输函数，与着色器里的那两个同式
[[nodiscard]] inline float srgbToLinear(float encoded) {
    return encoded <= 0.04045F ? encoded / 12.92F
                               : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] inline float linearToSrgb(float linear) {
    return linear <= 0.0031308F ? linear * 12.92F
                                : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
}

// vanilla 的混合：直接在编码值上插值
[[nodiscard]] inline float srgbSpaceBlend(float source, float backdrop, float alpha) {
    return alpha * source + (1.0F - alpha) * backdrop;
}

// 我们的混合：解码 → 插值 → 编码
[[nodiscard]] inline float linearSpaceBlend(float source, float backdrop, float alpha) {
    return linearToSrgb(alpha * srgbToLinear(source) + (1.0F - alpha) * srgbToLinear(backdrop));
}

// 在线性空间混合时，用哪个 alpha 才能得到 vanilla 在 sRGB 空间混合的结果。
// source/backdrop 是 sRGB 编码的通道值（0..1），alpha 是原版美术自带的那个。
// 源与底一样亮时方程退化（两边怎么混都是同一个颜色），此时原样返回。
[[nodiscard]] inline float linearBlendAlphaMatchingSrgb(float source, float backdrop,
                                                        float alpha) {
    const float target = srgbToLinear(srgbSpaceBlend(source, backdrop, alpha));
    const float sourceLinear = srgbToLinear(source);
    const float backdropLinear = srgbToLinear(backdrop);
    const float span = sourceLinear - backdropLinear;
    if (std::fabs(span) < 1.0e-6F) {
        return alpha;
    }
    return std::clamp((target - backdropLinear) / span, 0.0F, 1.0F);
}

} // namespace mc::ui

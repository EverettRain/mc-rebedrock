#pragma once

#include <string>
#include <string_view>

// UI-3：GUI spec §1.2 的字体度量，作为**布局公式的基础**。
//
// 这里只有纯算术，不碰 Vulkan 也不碰字体纹理，所以每一条都能被无头断言——而它们恰恰是
// 那种错了也看不出、只会让整屏差一个像素的东西。spec §1.2 把这条写成硬要求：
// **所有"居中"都是整数运算，复现时必须同样用整数除法，否则会与原版差 1px。**
//
// 单位一律是**逻辑像素**（GUI 像素），也就是 spec §0.3 的坐标系。乘 GUI 缩放是调用方
// 最后一步的事，绝不能在中途乘——先乘再取整就不是整数版面了。

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mc::ui {

// 26.1 `Font.lineHeight = 9`。字形本身高 8，行与行之间因此多一像素的呼吸。
// 两者是**不同的量**：垂直居中用的是字形高 8，行距用的是 9。
inline constexpr float kFontLineHeight = 9.0F;
inline constexpr float kFontGlyphHeight = 8.0F;

// 文字阴影：偏移 +1/+1 逻辑像素，颜色 = 主色 × 0.25。
//
// 26.1 `Font.java:428` 是 `ARGB.scaleRGB(textColor, 0.25F)`，而 `ARGB.scaleRGB` 在
// **0..255 的编码字节**上乘再截断（`(int)(red(color) * scale)`）。GUI 全程在 sRGB 编码值上
// 工作（见 hud.frag 的头注释），所以这里的 float 通道值乘 255 就是那个字节，
// 照着截断一次才与原版逐字节相同——直接乘 0.25 会在 1.0 这样的主色上差一个 255 分之一。
inline constexpr float kTextShadowScale = 0.25F;
inline constexpr float kAsciiShadowOffset = 1.0F;
// 26.1 `UnihexProvider` 的字形按半尺寸绘制，它的 `getShadowOffset()` 因此返回 0.5，
// 而不是 `GlyphInfo` 默认的 1.0。对 unicode 文本用 1.0 会让阴影粗一倍。
inline constexpr float kUnicodeShadowOffset = 0.5F;

[[nodiscard]] inline float textShadowChannel(float channel) {
    const auto encoded = static_cast<int>(std::clamp(channel, 0.0F, 1.0F) * 255.0F);
    const auto scaled = static_cast<int>(static_cast<float>(encoded) * kTextShadowScale);
    return static_cast<float>(scaled) / 255.0F;
}

// `font.width(text)` 是**整数**：26.1 的 `Font.width` 对 splitter 的浮点宽度取 `Mth.ceil`。
// 居中要用这个整数，用浮点宽度再取整是另一个数。
// 把一段文字截到放得进 `maxWidth` 为止，末尾加省略号。
//
// ★ 26.1 对超宽标签的做法是 **scissor 剪裁 + 正弦缓动来回滚动**（marquee，
//   `ActiveTextCollector.defaultScrollingHelper`），不是截断——那要 scissor、
//   还要把时间钉进 determinism knob，否则截图两遍不再逐字节相同（偏差 D17）。
//   在那之前，截断至少保证**文字不画到控件外、更不画到画布外**：资源包描述
//   曾经一路画出屏幕右边缘。
//
// `measure` 是调用方给的字宽函数（字宽依赖字体与缩放，ui:: 不接触资源）。
// 抽成纯函数而不是绘制侧的一段循环，理由是护栏 20：绘制侧的算术改坏了不动任何返回值。
template <typename MeasureFn>
[[nodiscard]] std::string truncateToWidth(std::string_view text, float maxWidth,
                                          MeasureFn measure) {
    if (maxWidth <= 0.0F) {
        return {};
    }
    if (measure(text) <= maxWidth) {
        return std::string{text};
    }
    constexpr std::string_view kEllipsis = "...";
    const float ellipsisWidth = measure(kEllipsis);
    // 连省略号都放不下：宁可给空串，也不要画一个比格子还宽的 "..."
    if (ellipsisWidth > maxWidth) {
        return {};
    }
    std::size_t keep = text.size();
    while (keep > 0U) {
        // ★ 按**字节**退是不对的：UTF-8 的多字节码点会被切成半个字符，画出来是乱码。
        //   退到一个不是续字节（10xxxxxx）的位置为止。
        --keep;
        while (keep > 0U && (static_cast<unsigned char>(text[keep]) & 0xC0U) == 0x80U) {
            --keep;
        }
        if (measure(text.substr(0, keep)) + ellipsisWidth <= maxWidth) {
            break;
        }
    }
    return std::string{text.substr(0, keep)} + std::string{kEllipsis};
}

[[nodiscard]] inline int textWidthLogical(float unscaledWidth) {
    return static_cast<int>(std::ceil(unscaledWidth - 1.0e-4F));
}

// 水平居中：`x = x_center - font.width(s) / 2`，**整数除法**。
//
// C++ 与 Java 的 `/` 对负数都向零截断，所以文字比容器还宽时两边的结果也一致
// （会向右溢出而不是向左）——照抄的是行为，不只是公式。
[[nodiscard]] constexpr int centredX(int containerX, int containerWidth, int contentWidth) {
    return containerX + (containerWidth - contentWidth) / 2;
}

// 垂直居中于高 `rowHeight` 的一行：`y + (h - 8) / 2`。
// 按钮惯用 `(20 - 8) / 2 = 6`，spec §1.2 与 §2.1 都写着这个 6。
[[nodiscard]] constexpr int centredTextY(int rowY, int rowHeight) {
    return rowY + (rowHeight - static_cast<int>(kFontGlyphHeight)) / 2;
}

} // namespace mc::ui

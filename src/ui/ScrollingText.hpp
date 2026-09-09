#pragma once

// UI-6f（D17）：**超宽文字的来回滚动**（26.1 `ActiveTextCollector.defaultScrollingHelper`，
// `ActiveTextCollector.java:62-86`）。
//
// 26.1 对放不下的标签**不是截断加省略号**，是把它剪裁在控件里、左右缓动来回滚：
//
//     if (lineWidth > availableMessageWidth) {
//         maxPosition = lineWidth - availableMessageWidth;
//         time   = Util.getMillis() / 1000.0;
//         period = max(maxPosition * 0.5, 3.0);
//         alpha  = sin((PI/2) * cos((PI*2) * time / period)) / 2.0 + 0.5;
//         pos    = lerp(alpha, 0.0, maxPosition);
//         withScissor(left, right, top, bottom);
//         accept(LEFT, left - (int)pos, ...);
//     } else {
//         accept(CENTER, clamp(centerX, left + lineWidth/2, right - lineWidth/2), ...);
//     }
//
// 注意**装得下的那一支也不是简单居中**：中心被夹在 `[left + w/2, right - w/2]`，
// 所以贴边的格子里文字不会探出去。
//
// ★ 这是**时间驱动的动画**，会破坏截图通道"同一条命令行跑两遍逐字节相同"这条验收
//   条件。所以出图必须把它钉住——`seconds < 0` 就是那个钉子（见 kPinnedTime），
//   钉住时偏移为 0，也就是**停在文字开头**。

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mc::ui {

// 26.1 的 `Math.max(maxPosition * 0.5, 3.0)`：滚一个来回至少 3 秒，
// 文字越长周期越长（越长滚得越慢，读起来才跟得上）。
inline constexpr double kScrollingTextMinimumPeriod = 3.0;

// 传给 `scrollingTextAt` 表示"钉住不动"（出图用）。
inline constexpr double kPinnedTime = -1.0;

struct ScrollingText final {
    // 放不下、需要剪裁并滚动。为假时调用方按"居中且夹在两端之内"画。
    bool scrolls = false;
    // 文字左缘相对格子左缘的偏移，**非负**：画在 `left - offset`。
    float offset = 0.0F;

    [[nodiscard]] constexpr bool operator==(const ScrollingText&) const = default;
};

// `availableWidth` 是格子里能放文字的宽度（26.1 的 `right - left`）。
[[nodiscard]] inline ScrollingText scrollingTextAt(float textWidth, float availableWidth,
                                                   double seconds) {
    if (textWidth <= availableWidth || availableWidth <= 0.0F) {
        return {};   // 装得下：不滚，偏移 0
    }
    const double maxPosition = static_cast<double>(textWidth - availableWidth);
    // ★ 钉住：出图路径走这一支，于是同一条命令行跑两遍逐字节相同。
    if (seconds < 0.0) {
        return {true, 0.0F};
    }
    const double period = std::max(maxPosition * 0.5, kScrollingTextMinimumPeriod);
    const double alpha =
        std::sin((std::numbers::pi / 2.0) *
                 std::cos((std::numbers::pi * 2.0) * seconds / period)) /
            2.0 +
        0.5;
    // lerp(alpha, 0, maxPosition)
    const double position = alpha * maxPosition;
    return {true, static_cast<float>(position)};
}

// 装得下时文字的左缘 x。26.1：`clamp(centerX, left + w/2, right - w/2)` 之后再减半宽。
//
// ★ 它**不是**"简单居中"：贴边的格子里，居中会让文字探出格子，夹住之后不会。
[[nodiscard]] inline float centredTextLeft(float cellLeft, float cellWidth, float textWidth) {
    const float centre = cellLeft + cellWidth * 0.5F;
    const float half = textWidth * 0.5F;
    const float clamped = std::clamp(centre, cellLeft + half, cellLeft + cellWidth - half);
    return clamped - half;
}

} // namespace mc::ui

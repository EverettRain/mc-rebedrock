#pragma once

// 滑块的光标换算：光标 x → 取值比例 [0,1]。
//
// ★ 它存在的理由是一次真实的缺陷（UI-6d 之后的现场报告：「一调节模糊强度就跳到 0，
//   且无法再次调整」）。根因是 `SliderBind::onDrag(float)` 这个回调**有两种约定**：
//   三个既有滑块（渲染距离 / 模拟距离 / 主音量）**忽略参数**、各自去读光标，
//   而 UI-6d 表驱动的整数滑块**把参数当权威**。按下时那句 `onDrag(0.0F)`
//   （注释还写着 "the appliers read the cursor themselves"）于是把模糊值一把打成 0；
//   而拖拽分派里没有它的分支，所以再也拖不动——两个症状同一个根因。
//
//   收口成一条约定：**fraction 是权威的**，由拖拽循环从控件自己的矩形算出来。
//   于是"这一页第几个控件是滑块"不再需要任何人硬编码——从前那三个 update 函数各写着
//   `frontendButtonRect(..., 2U, ...)` / `3U`，UI-6d 重排视频设置之后它们读的已经是
//   隔壁控件的矩形了，而症状只是"拖起来手感不对"，没有任何东西会红。
//
// 数值照 26.1 `AbstractSliderButton`：
//   取值   `(mouseX - (getX() + 4)) / (width - 8)`（`AbstractSliderButton.java:71`）
//   把手   画在 `getX() + value * (width - 8)`，宽 8（同文件 :96）
// 起点差的那 4 是**半个把手**：输入时抓的是把手中心，绘制时画的是把手左缘。
// 两处用同一个 `kSliderHandleWidth`，不是各写一个 4 和一个 8。

#include "ui/HudLayout.hpp"

#include <algorithm>

namespace mc::ui {

// 把手宽度（逻辑像素）。26.1 的滑块把手是 8x20 的原生美术。
inline constexpr int kSliderHandleWidth = 8;

// 光标 x（帧缓冲像素）→ 取值比例 [0,1]。
//
// `rect` 是控件在帧缓冲像素里的矩形，`scale` 是 GUI 缩放——两者一起才定得下把手宽度，
// 因为把手是**逻辑像素**里的 8。
[[nodiscard]] inline float sliderFractionFromCursor(const UiRect& rect, float cursorX,
                                                    float scale) {
    const float handle = static_cast<float>(kSliderHandleWidth) * scale;
    // 行程是轨道减掉一个把手宽：把手左缘从 rect.x 走到 rect.x + travel。
    const float travel = std::max(rect.width - handle, 1.0F);
    return std::clamp((cursorX - rect.x - handle * 0.5F) / travel, 0.0F, 1.0F);
}

// 把手左缘的 x（帧缓冲像素）。绘制侧用它，与上面那个函数互为逆。
[[nodiscard]] inline float sliderHandleX(const UiRect& rect, float fraction, float scale) {
    const float handle = static_cast<float>(kSliderHandleWidth) * scale;
    const float travel = std::max(rect.width - handle, 0.0F);
    return rect.x + std::clamp(fraction, 0.0F, 1.0F) * travel;
}

} // namespace mc::ui

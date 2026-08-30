#pragma once

// 通用的页面交互：命中测试加点击与拖拽派发
// 它整体取代了 VulkanRenderer 里按下与抬起两个处理函数中的 switch
// 一次点击按几何解析到某个控件，再触发该控件自己的回调
// 这里没有逐按钮的分支，也不需要知道任何页面的存在
//
// 不碰 Vulkan，只在 ui::Page 这个值模型与指针坐标上工作
// 无头测试因此能搭一个页面、点一个坐标，断言正确的回调跑了，也断言被禁用的控件什么都不跑
// 悬停、按下与可用状态的语义复用 ui::buttonActivated 与 buttonVisualState
// 与从前逐按钮的路径完全一致

#include "ui/ButtonControl.hpp"
#include "ui/Widget.hpp"

#include <cstddef>

namespace mc::ui {

// 指针落在其矩形内、且位于最上层的那个可交互控件的下标，没有则返回 npos
// 平局时靠后的控件胜出，因为后画即在上，这与绘制顺序一致
// 不可交互的控件跳过，指 Label 与 Panel
inline constexpr std::size_t kNoWidget = static_cast<std::size_t>(-1);

[[nodiscard]] inline std::size_t hitTest(const Page& page, float pointerX,
                                         float pointerY) noexcept {
    std::size_t hit = kNoWidget;
    for (std::size_t i = 0; i < page.size(); ++i) {
        const Widget& widget = page[i];
        if (!widget.interactive()) {
            continue;
        }
        if (widget.rect.contains(pointerX, pointerY)) {
            hit = i;  // keep scanning so the last (topmost) match wins
        }
    }
    return hit;
}

// 为落在同一个控件上的一次按下与抬起触发激活
// pressed 是按下时命中的那个控件，由先前的命中测试给出
// 只有抬起同样落在那个控件上、且它可用时，激活才会执行
// 这正是 ButtonControl::buttonActivated 的约定，现在作用于整个页面
// 返回被激活的控件下标，什么都没触发则返回 kNoWidget
//
// Slider 是拖拽控件而不是点击控件，它在这里永远不激活，它的效果已经由拖拽回调施加
// ListRow、Button 与 Toggle 才触发 onActivate
[[nodiscard]] inline std::size_t dispatchActivate(const Page& page, std::size_t pressedIndex,
                                                  float releaseX, float releaseY) {
    if (pressedIndex >= page.size()) {
        return kNoWidget;
    }
    const Widget& widget = page[pressedIndex];
    if (widget.kind == WidgetKind::Slider) {
        return kNoWidget;  // drags act through onDrag/onCommit, not activation
    }
    if (!buttonActivated(widget.rect, releaseX, releaseY, /*wasPressed=*/true, widget.enabled)) {
        return kNoWidget;
    }
    if (widget.onActivate) {
        widget.onActivate();
    }
    return pressedIndex;
}

// 给无头测试路径用的便捷函数，任何在同一处按下并抬起的调用方也可以用
// 它对坐标做命中测试，命中一个可用的可交互控件就激活它
// 返回被激活的下标，或者 kNoWidget
[[nodiscard]] inline std::size_t clickAt(const Page& page, float pointerX, float pointerY) {
    const std::size_t index = hitTest(page, pointerX, pointerY);
    if (index == kNoWidget || !page[index].enabled) {
        return kNoWidget;
    }
    return dispatchActivate(page, index, pointerX, pointerY);
}

// 指针下方是一个可用的 Slider 时，在它上面开始一次拖拽
// 调用方给出它按布局算好的轨道比例，由滑块的 onDrag 施加
// 拖拽开始了就返回滑块下标，否则返回 kNoWidget
[[nodiscard]] inline std::size_t beginSliderDrag(const Page& page, float pointerX, float pointerY,
                                                 float trackFraction) {
    const std::size_t index = hitTest(page, pointerX, pointerY);
    if (index == kNoWidget) {
        return kNoWidget;
    }
    const Widget& widget = page[index];
    if (widget.kind != WidgetKind::Slider || !widget.enabled || !widget.slider.onDrag) {
        return kNoWidget;
    }
    widget.slider.onDrag(trackFraction);
    return index;
}

}  // namespace mc::ui

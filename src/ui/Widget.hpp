#pragma once

// 数据驱动的菜单对象模型
// 一个菜单页面是一列扁平的 Widget 值，形状上允许嵌套，但既不是常驻的面向对象树，也不是虚表继承体系
// 每个控件自带几何、标签、是否可用，以及一个激活时触发的 std::function 回调
// 它取代了从前那三者的耦合：一个 MenuButton 枚举、每页一份 constexpr 数组
// 外加约 300 行的 switch 派发
// 现在页面在 PageBuilder 一处装配，在 MenuInteraction 通用地命中与派发
// 再由渲染器的绘制后端按 kind 通用地画出
//
// 这里不碰 Vulkan 也不碰 GLFW，它住在 mc_rebedrock_runtime 里
// 模型、命中测试与派发因此能被无头单测覆盖：搭一个页面，点一个坐标，断言对应的回调被触发
// 回调可以捕获渲染器需要的任何 Vulkan、存档或音频状态，ui 命名空间从不接触这些
//
// Widget 设计成可嵌套的，Panel 能装子控件，并携带简单的相对布局提示
// 将来的容器界面，比如创造页签加滚动格加搜索框，因此是在同一个模型上扩展，而不是推倒重写
// 目前所有页面都是扁平的，嵌套只是留好了形状，还没有搭成框架

#include "ui/HudLayout.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mc::ui {

enum class WidgetKind : std::uint8_t {
    Button,     // a clickable button (GuiNineSlice + label)
    Slider,     // a horizontal slider with a draggable handle
    ListRow,    // one selectable row in a scrolling list (worlds/languages)
    Label,      // static text, never interactive
    Panel,      // a non-interactive container (holds children; shape only in PX-4)
    Toggle,     // a button whose label reflects an on/off (cycled) option
    TextField,  // an editable text line (create/edit world name)
};

// 滑块的数据与回调
// value() 给出当前用于显示的值，绘制后端据此画滑块位置，该值可能是归一化的也可能是原始的
// onDrag(fraction) 施加一个新位置，fraction 是轨道上 [0,1] 的比例
// 规矩是把副作用留在回调里而不是留在遍历里
// 遍历中绝不出现 if (kind == Slider) 这样的分支，它只负责调 onDrag
struct SliderBind final {
    std::function<float()> value;             // current fraction in [0,1], for drawing
    std::function<void(float)> onDrag;        // apply a new fraction in [0,1]
    std::function<void()> onCommit;           // release: persist / play feedback
};

// 一个菜单元素，以值的形式存在，可拷贝可移动，一个页面持有它们的 vector
// onActivate 是点击动作，供 Button、Toggle 与 ListRow 使用，Slider 改用 slider 字段
// debugId 是可选的稳定标识，只留给测试与日志，绝不用来分派行为
struct Widget final {
    WidgetKind kind = WidgetKind::Button;
    UiRect rect{};
    std::string label{};
    bool enabled = true;
    std::uint16_t debugId = 0;  // optional test/debug tag; 0 == none

    std::function<void()> onActivate{};  // Button/Toggle/ListRow click
    SliderBind slider{};                 // Slider only

    // 为将来的容器界面预留的嵌套形状，目前每个扁平页面里它都是空的
    std::vector<Widget> children{};

    [[nodiscard]] bool interactive() const noexcept {
        return kind != WidgetKind::Label && kind != WidgetKind::Panel;
    }
};

// 一个页面就是一列控件值，每次打开页面时重建
// 不做脏标记，菜单是冷路径，重建一次的代价微不足道
using Page = std::vector<Widget>;

}  // namespace mc::ui
